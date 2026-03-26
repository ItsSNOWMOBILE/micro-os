/*
 * xhci.c -- USB xHCI host controller driver.
 *
 * Discovers an xHCI controller via PCI, initialises the controller
 * (DCBAA, command ring, event ring, scratchpad), enumerates connected
 * USB devices via Enable Slot + Address Device + GET_DESCRIPTOR,
 * and exposes the results via xhci_list_devices().
 *
 * Polling mode only (no MSI/IRQ).
 */

#include "xhci.h"
#include "pci.h"
#include "../kernel.h"
#include "../console.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../drivers/timer.h"
#include "../mm/vmm.h"

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

static volatile uint32_t *
mmio32(uintptr_t addr)
{
    return (volatile uint32_t *)addr;
}

static uint32_t
rd32(uintptr_t addr)
{
    return *mmio32(addr);
}

static void
wr32(uintptr_t addr, uint32_t val)
{
    *mmio32(addr) = val;
}

static void
wr64(uintptr_t addr, uint64_t val)
{
    wr32(addr, (uint32_t)val);
    wr32(addr + 4, (uint32_t)(val >> 32));
}

/* ── Controller state ────────────────────────────────────────────────────── */

static bool initialised;

static uintptr_t mmio_base;
static uintptr_t op_base;
static uintptr_t rt_base;
static uintptr_t db_base;

static uint32_t max_slots;
static uint32_t max_ports;
static uint32_t ctx_size;  /* 32 or 64 bytes */

/* ── Ring sizes ──────────────────────────────────────────────────────────── */

#define CMD_RING_SIZE   64
#define EVT_RING_SIZE   64
#define EP0_RING_SIZE   32

/* ── Command ring ────────────────────────────────────────────────────────── */

static XhciTrb *cmd_ring;
static int       cmd_enq;
static int       cmd_cycle;

/* ── Event ring ──────────────────────────────────────────────────────────── */

static XhciTrb      *evt_ring;
static XhciErstEntry *erst;
static int            evt_deq;
static int            evt_cycle;

/* ── DCBAA ───────────────────────────────────────────────────────────────── */

static uint64_t *dcbaa;

/* ── Device tracking ─────────────────────────────────────────────────────── */

static XhciUsbDevice devices[XHCI_MAX_DEVICES];
static int           device_count;

/* ── Per-slot data ───────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *output_ctx;
    uint8_t *input_ctx;
    XhciTrb *ep0_ring;
    int      ep0_enq;
    int      ep0_cycle;
} SlotData;

#define MAX_SLOTS_INTERNAL 32
static SlotData slots[MAX_SLOTS_INTERNAL];

/* ── Timeout helpers ─────────────────────────────────────────────────────── */

static bool
wait_bits_set(uintptr_t reg, uint32_t mask, int ms)
{
    uint64_t deadline = timer_ticks() + (uint64_t)ms / 10 + 1;
    while (!(rd32(reg) & mask)) {
        if (timer_ticks() > deadline) return false;
    }
    return true;
}

static bool
wait_bits_clear(uintptr_t reg, uint32_t mask, int ms)
{
    uint64_t deadline = timer_ticks() + (uint64_t)ms / 10 + 1;
    while (rd32(reg) & mask) {
        if (timer_ticks() > deadline) return false;
    }
    return true;
}

/* ── Ring operations ─────────────────────────────────────────────────────── */

static void
cmd_ring_init(void)
{
    cmd_ring = kmalloc_aligned(CMD_RING_SIZE * sizeof(XhciTrb), 64);
    memset(cmd_ring, 0, CMD_RING_SIZE * sizeof(XhciTrb));
    cmd_enq = 0;
    cmd_cycle = 1;

    /* Link TRB at the end wrapping back to start. */
    XhciTrb *link = &cmd_ring[CMD_RING_SIZE - 1];
    link->param_lo = (uint32_t)(uintptr_t)cmd_ring;
    link->param_hi = (uint32_t)((uintptr_t)cmd_ring >> 32);
    link->status   = 0;
    link->control  = TRB_TYPE(TRB_LINK) | (1 << 1) | cmd_cycle;  /* TC=1 */
}

static void
cmd_ring_push(XhciTrb *trb)
{
    trb->control = (trb->control & ~1u) | cmd_cycle;
    /* Write all fields before the cycle bit becomes visible to HW. */
    cmd_ring[cmd_enq].param_lo = trb->param_lo;
    cmd_ring[cmd_enq].param_hi = trb->param_hi;
    cmd_ring[cmd_enq].status   = trb->status;
    __asm__ volatile("" ::: "memory");  /* barrier: fields before control */
    cmd_ring[cmd_enq].control  = trb->control;
    cmd_enq++;
    if (cmd_enq >= CMD_RING_SIZE - 1) {
        /* Update link TRB cycle bit. */
        cmd_ring[CMD_RING_SIZE - 1].control =
            (cmd_ring[CMD_RING_SIZE - 1].control & ~1u) | cmd_cycle;
        cmd_enq = 0;
        cmd_cycle ^= 1;
    }
}

static void
cmd_ring_doorbell(void)
{
    wr32(db_base, 0);
}

static void
evt_ring_init(void)
{
    evt_ring = kmalloc_aligned(EVT_RING_SIZE * sizeof(XhciTrb), 64);
    memset(evt_ring, 0, EVT_RING_SIZE * sizeof(XhciTrb));

    erst = kmalloc_aligned(sizeof(XhciErstEntry), 64);
    erst[0].base_addr = (uintptr_t)evt_ring;
    erst[0].size      = EVT_RING_SIZE;
    erst[0].reserved  = 0;

    evt_deq = 0;
    evt_cycle = 1;
}

static XhciTrb *
evt_ring_poll(int timeout_ms)
{
    uint64_t deadline = timer_ticks() + (uint64_t)timeout_ms / 10 + 1;
    while (timer_ticks() < deadline) {
        __asm__ volatile("" ::: "memory");  /* compiler barrier before DMA read */
        volatile XhciTrb *trb = (volatile XhciTrb *)&evt_ring[evt_deq];
        if ((trb->control & 1) == (uint32_t)evt_cycle) {
            evt_deq++;
            if (evt_deq >= EVT_RING_SIZE) {
                evt_deq = 0;
                evt_cycle ^= 1;
            }
            /* Update ERDP. */
            uintptr_t ir0 = rt_base + 0x20;
            wr64(ir0 + XHCI_IR_ERDP,
                 (uintptr_t)&evt_ring[evt_deq] | (1u << 3));  /* EHB=1 */
            return (XhciTrb *)trb;
        }
    }
    return NULL;
}

/* ── EP0 transfer ring ───────────────────────────────────────────────────── */

static XhciTrb *
ep0_ring_alloc(void)
{
    XhciTrb *ring = kmalloc_aligned(EP0_RING_SIZE * sizeof(XhciTrb), 64);
    if (!ring) return NULL;
    memset(ring, 0, EP0_RING_SIZE * sizeof(XhciTrb));

    /* Link TRB. */
    XhciTrb *link = &ring[EP0_RING_SIZE - 1];
    link->param_lo = (uint32_t)(uintptr_t)ring;
    link->param_hi = (uint32_t)((uintptr_t)ring >> 32);
    link->control  = TRB_TYPE(TRB_LINK) | (1 << 1) | 1;
    return ring;
}

static void
ep0_push(SlotData *s, XhciTrb *trb)
{
    trb->control = (trb->control & ~1u) | s->ep0_cycle;
    /* Write all fields before the cycle bit becomes visible to HW. */
    s->ep0_ring[s->ep0_enq].param_lo = trb->param_lo;
    s->ep0_ring[s->ep0_enq].param_hi = trb->param_hi;
    s->ep0_ring[s->ep0_enq].status   = trb->status;
    __asm__ volatile("" ::: "memory");  /* barrier: fields before control */
    s->ep0_ring[s->ep0_enq].control  = trb->control;
    s->ep0_enq++;
    if (s->ep0_enq >= EP0_RING_SIZE - 1) {
        s->ep0_ring[EP0_RING_SIZE - 1].control =
            (s->ep0_ring[EP0_RING_SIZE - 1].control & ~1u) | s->ep0_cycle;
        s->ep0_enq = 0;
        s->ep0_cycle ^= 1;
    }
}

/* ── Command helpers ─────────────────────────────────────────────────────── */

/* Send a command TRB and wait for completion event. Returns completion code.
 * Skips any port status change events that arrive before the completion. */
static int
cmd_send(XhciTrb *trb, XhciTrb *evt_out)
{
    cmd_ring_push(trb);
    cmd_ring_doorbell();

    for (int attempts = 0; attempts < 5; attempts++) {
        XhciTrb *evt = evt_ring_poll(1000);
        if (!evt) return -1;

        int type = (evt->control >> 10) & 0x3F;
        if (type == TRB_CMD_COMPLETE) {
            if (evt_out) *evt_out = *evt;
            return (evt->status >> 24) & 0xFF;
        }
        /* Port status change or other non-command event — skip and retry. */
    }
    return -1;
}

/* ── Enable Slot ─────────────────────────────────────────────────────────── */

static int
enable_slot(void)
{
    XhciTrb trb = {0};
    trb.control = TRB_TYPE(TRB_ENABLE_SLOT);

    XhciTrb evt;
    int cc = cmd_send(&trb, &evt);
    if (cc != 1) return -1;  /* 1 = Success */

    return (evt.control >> 24) & 0xFF;  /* slot ID */
}

/* ── Address Device ──────────────────────────────────────────────────────── */

static int
address_device(int slot_id, int port, int speed)
{
    if (slot_id <= 0 || slot_id >= MAX_SLOTS_INTERNAL) return -1;

    SlotData *s = &slots[slot_id];

    /* Allocate output device context. */
    size_t ctx_total = ctx_size * 32;  /* slot + 31 endpoints */
    s->output_ctx = kmalloc_aligned(ctx_total, 64);
    if (!s->output_ctx) return -1;
    memset(s->output_ctx, 0, ctx_total);

    /* Point DCBAA to output context. */
    dcbaa[slot_id] = (uintptr_t)s->output_ctx;

    /* Allocate EP0 transfer ring. */
    s->ep0_ring = ep0_ring_alloc();
    if (!s->ep0_ring) return -1;
    s->ep0_enq = 0;
    s->ep0_cycle = 1;

    /* Allocate input context (input control ctx + slot + 31 endpoints). */
    size_t input_total = ctx_size * 33;
    s->input_ctx = kmalloc_aligned(input_total, 64);
    if (!s->input_ctx) return -1;
    memset(s->input_ctx, 0, input_total);

    /* Input Control Context: add slot (bit 0) and EP0 (bit 1). */
    uint32_t *icc = (uint32_t *)s->input_ctx;
    icc[1] = 0x3;  /* Add Context Flags: slot + EP0 */

    /* Slot Context (at offset ctx_size). */
    uint32_t *slot_ctx = (uint32_t *)(s->input_ctx + ctx_size);
    /* DW0: Context Entries=1, Speed, Route String=0 */
    slot_ctx[0] = (1u << 27) | ((uint32_t)speed << 20);
    /* DW1: Root Hub Port Number */
    slot_ctx[1] = ((uint32_t)port << 16);

    /* Endpoint 0 Context (at offset ctx_size * 2). */
    uint32_t *ep0_ctx = (uint32_t *)(s->input_ctx + ctx_size * 2);

    /* Max packet size based on speed. */
    uint16_t max_pkt;
    switch (speed) {
        case XHCI_SPEED_LOW:   max_pkt = 8;   break;
        case XHCI_SPEED_FULL:  max_pkt = 8;   break;
        case XHCI_SPEED_HIGH:  max_pkt = 64;  break;
        case XHCI_SPEED_SUPER: max_pkt = 512; break;
        default:               max_pkt = 8;   break;
    }

    /* DW1: CErr=3, EP Type=4 (Control Bidi), Max Packet Size */
    ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)max_pkt << 16);
    /* DW2/DW3: TR Dequeue Pointer | DCS=1 */
    uintptr_t ep0_phys = (uintptr_t)s->ep0_ring;
    ep0_ctx[2] = (uint32_t)(ep0_phys | 1);
    ep0_ctx[3] = (uint32_t)(ep0_phys >> 32);
    /* DW4: Average TRB Length = 8 */
    ep0_ctx[4] = 8;

    /* Issue Address Device command. */
    XhciTrb trb = {0};
    trb.param_lo = (uint32_t)(uintptr_t)s->input_ctx;
    trb.param_hi = (uint32_t)((uintptr_t)s->input_ctx >> 32);
    trb.control  = TRB_TYPE(TRB_ADDRESS_DEV) | ((uint32_t)slot_id << 24);

    XhciTrb evt;
    int cc = cmd_send(&trb, &evt);
    if (cc != 1) {
        kprintf("xhci: address device failed (slot %d, cc=%d)\n", slot_id, cc);
        return -1;
    }

    return 0;
}

/* ── Control transfer (GET_DESCRIPTOR) ───────────────────────────────────── */

static int
get_descriptor(int slot_id, uint16_t wValue, uint16_t wIndex,
               void *buf, uint16_t wLength)
{
    if (slot_id <= 0 || slot_id >= MAX_SLOTS_INTERNAL) return -1;
    SlotData *s = &slots[slot_id];

    /* Setup Stage TRB. */
    XhciTrb setup = {0};
    setup.param_lo = 0x80 | (0x06 << 8) | ((uint32_t)wValue << 16);
    setup.param_hi = (uint32_t)wIndex | ((uint32_t)wLength << 16);
    setup.status   = 8;  /* TRB transfer length = 8 */
    setup.control  = TRB_TYPE(TRB_SETUP_STAGE) | (1 << 6) | (3u << 16);
    /* IDT=1, TRT=3 (IN data stage) */
    ep0_push(s, &setup);

    /* Data Stage TRB. */
    XhciTrb data = {0};
    data.param_lo = (uint32_t)(uintptr_t)buf;
    data.param_hi = (uint32_t)((uintptr_t)buf >> 32);
    data.status   = wLength;
    data.control  = TRB_TYPE(TRB_DATA_STAGE) | (1u << 16);  /* DIR=1 (IN) */
    ep0_push(s, &data);

    /* Status Stage TRB. */
    XhciTrb status = {0};
    status.control = TRB_TYPE(TRB_STATUS_STAGE) | (1 << 5);  /* IOC=1 */
    ep0_push(s, &status);

    /* Ring doorbell for this slot, target EP0 (DCI=1). */
    wr32(db_base + 4 * slot_id, 1);

    /* Wait for transfer event. */
    XhciTrb *evt = evt_ring_poll(2000);
    if (!evt) return -1;

    int type = (evt->control >> 10) & 0x3F;
    int cc   = (evt->status >> 24) & 0xFF;

    if (type != TRB_XFER_EVENT) return -1;
    /* Success=1 or Short Packet=13 both OK. */
    if (cc != 1 && cc != 13) return -1;

    int residual = evt->status & 0xFFFFFF;
    return wLength - residual;
}

/* ── Port reset ──────────────────────────────────────────────────────────── */

static bool
port_reset(int port)
{
    uintptr_t portsc_addr = op_base + XHCI_OP_PORTSC(port);
    uint32_t val = rd32(portsc_addr);

    /* Already enabled (USB3 devices may auto-enable)? */
    if (val & XHCI_PORTSC_PED)
        return true;

    /* Issue port reset. */
    val = (val & XHCI_PORTSC_PRESERVE) | XHCI_PORTSC_PR;
    wr32(portsc_addr, val);

    /* Wait for PRC (Port Reset Change). */
    if (!wait_bits_set(portsc_addr, XHCI_PORTSC_PRC, 500))
        return false;

    /* Clear PRC. */
    val = rd32(portsc_addr);
    wr32(portsc_addr, (val & XHCI_PORTSC_PRESERVE) | XHCI_PORTSC_PRC);

    /* Check PED. */
    val = rd32(portsc_addr);
    return (val & XHCI_PORTSC_PED) != 0;
}

/* ── Enumerate one port ──────────────────────────────────────────────────── */

static void
enumerate_port(int port)
{
    uintptr_t portsc_addr = op_base + XHCI_OP_PORTSC(port);
    uint32_t portsc = rd32(portsc_addr);

    if (!(portsc & XHCI_PORTSC_CCS))
        return;  /* No device connected. */

    int speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;

    /* Reset port. */
    if (!port_reset(port)) return;

    /* Enable Slot. */
    int slot_id = enable_slot();
    if (slot_id < 0) return;

    /* Address Device. */
    if (address_device(slot_id, port, speed) < 0)
        return;

    /* GET_DESCRIPTOR(Device). */
    UsbDeviceDesc desc;
    memset(&desc, 0, sizeof(desc));
    int n = get_descriptor(slot_id, 0x0100, 0, &desc, sizeof(desc));
    if (n < 8) {
        /* Try short read first (8 bytes), then full. */
        n = get_descriptor(slot_id, 0x0100, 0, &desc, 8);
        if (n >= 8 && desc.bLength >= 18)
            get_descriptor(slot_id, 0x0100, 0, &desc, 18);
    }

    /* Record the device. */
    if (device_count < XHCI_MAX_DEVICES) {
        XhciUsbDevice *d = &devices[device_count++];
        d->present      = true;
        d->port         = (uint8_t)port;
        d->slot_id      = (uint8_t)slot_id;
        d->speed        = (uint8_t)speed;
        d->vendor_id    = desc.idVendor;
        d->product_id   = desc.idProduct;
        d->dev_class    = desc.bDeviceClass;
        d->dev_subclass = desc.bDeviceSubClass;
        d->dev_protocol = desc.bDeviceProtocol;
    }
}

/* ── Scratchpad allocation ───────────────────────────────────────────────── */

static void
alloc_scratchpad(uint32_t hcsparams2)
{
    uint32_t sp_lo = (hcsparams2 >> 21) & 0x1F;
    uint32_t sp_hi = (hcsparams2 >> 27) & 0x1F;
    uint32_t num_sp = (sp_hi << 5) | sp_lo;
    if (num_sp == 0) return;

    uint64_t *sp_array = kmalloc_aligned(num_sp * sizeof(uint64_t), 64);
    if (!sp_array) return;

    for (uint32_t i = 0; i < num_sp; i++) {
        void *buf = pmm_alloc_page();
        if (!buf) break;
        /* Safe: PMM returns addresses within the 0-4 GiB identity map. */
        memset(buf, 0, 4096);
        sp_array[i] = (uintptr_t)buf;
    }

    dcbaa[0] = (uintptr_t)sp_array;
}

/* ── Init ────────────────────────────────────────────────────────────────── */

int
xhci_init(void)
{
    /* Find xHCI controller on PCI bus (class 0x0C, subclass 0x03, prog_if 0x30). */
    const PciDevice *pdev = NULL;
    for (int i = 0; i < pci_device_count(); i++) {
        const PciDevice *d = pci_get_device(i);
        if (d->class_code == XHCI_CLASS &&
            d->subclass   == XHCI_SUBCLASS &&
            d->prog_if    == XHCI_PROG_IF) {
            pdev = d;
            break;
        }
    }
    if (!pdev) return -1;

    /* BAR0 is a memory BAR. */
    if (pdev->bar[0] & 1) return -1;  /* must be MMIO, not I/O */
    uint64_t bar0 = pdev->bar[0] & 0xFFFFFFF0u;
    if (((pdev->bar[0] >> 1) & 0x3) == 0x2) {
        /* 64-bit BAR: combine BAR0 and BAR1. */
        mmio_base = (uintptr_t)(bar0 | ((uint64_t)pdev->bar[1] << 32));
    } else {
        /* 32-bit BAR. */
        mmio_base = (uintptr_t)bar0;
    }
    if (!mmio_base) return -1;

    /* Map MMIO region if above 4 GiB (our identity mapping only covers 0-4G). */
    if (mmio_base >= 0x100000000ULL)
        vmm_map_mmio(mmio_base, 0x10000);  /* map 64 KiB of MMIO space */

    /* Enable bus mastering + memory space. */
    pci_enable_bus_master((PciDevice *)pdev);
    pci_write16(pdev->bus, pdev->device, pdev->function, PCI_COMMAND,
                pci_read16(pdev->bus, pdev->device, pdev->function, PCI_COMMAND)
                | PCI_CMD_MEMORY);

    /* Read capability registers. */
    uint8_t  cap_length = (uint8_t)rd32(mmio_base + XHCI_CAP_CAPLENGTH);
    uint32_t hcsparams1 = rd32(mmio_base + XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2 = rd32(mmio_base + XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1 = rd32(mmio_base + XHCI_CAP_HCCPARAMS1);
    uint32_t db_off     = rd32(mmio_base + XHCI_CAP_DBOFF) & 0xFFFFFFFC;
    uint32_t rts_off    = rd32(mmio_base + XHCI_CAP_RTSOFF) & 0xFFFFFFE0;

    op_base = mmio_base + cap_length;
    rt_base = mmio_base + rts_off;
    db_base = mmio_base + db_off;

    max_slots = hcsparams1 & 0xFF;
    max_ports = (hcsparams1 >> 24) & 0xFF;
    ctx_size  = (hccparams1 & (1 << 2)) ? 64 : 32;

    if (max_slots > MAX_SLOTS_INTERNAL)
        max_slots = MAX_SLOTS_INTERNAL;

    kprintf("xhci: %u ports, %u slots, ctx=%u bytes\n",
            max_ports, max_slots, ctx_size);

    /* ── Halt ──────────────────────────────────────────────────────── */
    wr32(op_base + XHCI_OP_USBCMD, rd32(op_base + XHCI_OP_USBCMD) & ~XHCI_CMD_RUN);
    if (!wait_bits_set(op_base + XHCI_OP_USBSTS, XHCI_STS_HCH, 50)) {
        kprintf("xhci: halt timeout\n");
        return -1;
    }

    /* ── Reset ─────────────────────────────────────────────────────── */
    wr32(op_base + XHCI_OP_USBCMD, XHCI_CMD_HCRST);
    if (!wait_bits_clear(op_base + XHCI_OP_USBCMD, XHCI_CMD_HCRST, 200)) {
        kprintf("xhci: reset timeout\n");
        return -1;
    }
    if (!wait_bits_clear(op_base + XHCI_OP_USBSTS, XHCI_STS_CNR, 200)) {
        kprintf("xhci: not ready after reset\n");
        return -1;
    }

    /* ── Configure slots ───────────────────────────────────────────── */
    wr32(op_base + XHCI_OP_CONFIG, max_slots);

    /* ── DCBAA ─────────────────────────────────────────────────────── */
    dcbaa = kmalloc_aligned((max_slots + 1) * sizeof(uint64_t), 64);
    memset(dcbaa, 0, (max_slots + 1) * sizeof(uint64_t));
    wr64(op_base + XHCI_OP_DCBAAP, (uintptr_t)dcbaa);

    /* ── Scratchpad ────────────────────────────────────────────────── */
    alloc_scratchpad(hcsparams2);

    /* ── Command ring ──────────────────────────────────────────────── */
    cmd_ring_init();
    wr64(op_base + XHCI_OP_CRCR, (uintptr_t)cmd_ring | 1);

    /* ── Event ring ────────────────────────────────────────────────── */
    evt_ring_init();
    uintptr_t ir0 = rt_base + 0x20;
    wr32(ir0 + XHCI_IR_ERSTSZ, 1);
    wr64(ir0 + XHCI_IR_ERDP, (uintptr_t)evt_ring);
    wr64(ir0 + XHCI_IR_ERSTBA, (uintptr_t)erst);

    /* ── Start controller ──────────────────────────────────────────── */
    wr32(op_base + XHCI_OP_USBCMD, XHCI_CMD_RUN);
    if (!wait_bits_clear(op_base + XHCI_OP_USBSTS, XHCI_STS_HCH, 50)) {
        kprintf("xhci: start timeout\n");
        return -1;
    }

    initialised = true;
    kprintf("[ok] xHCI (USB 3.0)\n");

    /* ── Enumerate ports ───────────────────────────────────────────── */
    for (uint32_t p = 1; p <= max_ports; p++)
        enumerate_port((int)p);

    if (device_count > 0)
        kprintf("xhci: %d USB device(s) found\n", device_count);

    return 0;
}

bool
xhci_available(void)
{
    return initialised;
}

int
xhci_device_count(void)
{
    return device_count;
}

const XhciUsbDevice *
xhci_get_device(int index)
{
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

/* ── lsusb output ────────────────────────────────────────────────────────── */

static const char *
speed_str(uint8_t speed)
{
    switch (speed) {
        case XHCI_SPEED_LOW:   return "1.5 Mbps";
        case XHCI_SPEED_FULL:  return "12 Mbps";
        case XHCI_SPEED_HIGH:  return "480 Mbps";
        case XHCI_SPEED_SUPER: return "5 Gbps";
        default:               return "unknown";
    }
}

static const char *
class_str(uint8_t cls)
{
    switch (cls) {
        case 0x00: return "per-interface";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case 0x03: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0E: return "Video";
        case 0x0F: return "Personal Healthcare";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFE: return "App-Specific";
        case 0xFF: return "Vendor-Specific";
        default:   return "Unknown";
    }
}

void
xhci_list_devices(void)
{
    if (!initialised) {
        kprintf("xHCI not initialised\n");
        return;
    }

    if (device_count == 0) {
        kprintf("No USB devices found\n");
        return;
    }

    kprintf("Bus  Port  Speed      VID:PID      Class\n");
    for (int i = 0; i < device_count; i++) {
        const XhciUsbDevice *d = &devices[i];
        kprintf("  0  %-4u  %-9s  %04x:%04x    %s\n",
                (unsigned)d->port,
                speed_str(d->speed),
                (unsigned)d->vendor_id,
                (unsigned)d->product_id,
                class_str(d->dev_class));
    }
}
