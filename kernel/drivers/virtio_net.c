/*
 * virtio_net.c -- Virtio network device driver (legacy PCI transport).
 *
 * Uses legacy virtio I/O port registers.  Two split virtqueues:
 * queue 0 for RX, queue 1 for TX.  Polling mode (no IRQ).
 */

#include "virtio_net.h"
#include "pci.h"
#include "../kernel.h"
#include "../console.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"

/* Virtio-net PCI IDs. */
#define VIRTIO_VENDOR  0x1AF4
#define VIRTIO_NET_DEV 0x1000

static bool     initialised;
static uint16_t iobase;
static uint8_t  mac_addr[6];

/* ── Virtqueue state ───────────────────────────────────────────────────── */

typedef struct {
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    uint16_t            size;
    uint16_t            last_used;
    /* Per-descriptor packet buffers. */
    uint8_t            *buffers[VIRTQ_SIZE];
} Virtqueue;

static Virtqueue rxq, txq;

/* Each RX/TX buffer: virtio-net header + max Ethernet frame. */
#define PKT_BUF_SIZE  (VIRTIO_NET_HDR_SIZE + 1518)

/* ── Virtqueue setup ───────────────────────────────────────────────────── */

/*
 * Legacy virtqueue layout (must be physically contiguous):
 *   desc table:  size * 16 bytes, aligned to page
 *   avail ring:  4 + size * 2 bytes, immediately after desc
 *   used ring:   4 + size * 8 bytes, aligned to page boundary after avail
 *
 * For VIRTQ_SIZE=128: desc=2048, avail=260, used=1028 => fits in 1 page
 * comfortably with proper alignment.
 */
static uint64_t
align_up(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

static int
virtq_init(Virtqueue *vq, int qsel)
{
    /* Select queue. */
    outw(iobase + VIRTIO_QUEUE_SEL, (uint16_t)qsel);
    uint16_t dev_size = inw(iobase + VIRTIO_QUEUE_SIZE);
    if (dev_size == 0) return -1;

    /* We only use up to VIRTQ_SIZE buffers, but the layout must match
     * the device's queue size so the device and driver agree on where
     * the avail/used rings live in memory. */
    vq->size = (dev_size > VIRTQ_SIZE) ? VIRTQ_SIZE : dev_size;

    /* Calculate layout using DEVICE's queue size (not our capped size). */
    uint64_t desc_sz  = (uint64_t)dev_size * 16;
    uint64_t avail_sz = 6 + (uint64_t)dev_size * 2;  /* flags + idx + ring[N] + used_event */
    uint64_t used_sz  = 6 + (uint64_t)dev_size * 8;  /* flags + idx + ring[N] + avail_event */

    uint64_t avail_off = desc_sz;
    uint64_t used_off  = align_up(avail_off + avail_sz, 4096);
    uint64_t total     = used_off + used_sz;

    /* Allocate contiguous pages. */
    uint64_t npages = (total + 4095) / 4096;
    /* We need contiguous physical pages. Allocate one at a time and hope
     * they're adjacent (they usually are early in boot). */
    uint8_t *base = pmm_alloc_page();
    if (!base) return -1;
    for (uint64_t i = 1; i < npages; i++) {
        void *p = pmm_alloc_page();
        if (!p || (uint8_t *)p != base + i * 4096) {
            /* Not contiguous — cannot proceed safely. */
            if (p) pmm_free_page(p);
            for (uint64_t j = 0; j < i; j++)
                pmm_free_page(base + j * 4096);
            return -1;
        }
    }

    memset(base, 0, npages * 4096);

    vq->desc  = (struct virtq_desc *)(base);
    vq->avail = (struct virtq_avail *)(base + avail_off);
    vq->used  = (struct virtq_used *)(base + used_off);
    vq->last_used = 0;

    /* Tell device where the queue lives (page frame number). */
    outl(iobase + VIRTIO_QUEUE_ADDR, (uint32_t)((uint64_t)base >> 12));

    return 0;
}

/* ── RX queue setup ────────────────────────────────────────────────────── */

static void
rx_fill(void)
{
    /* Allocate packet buffers and populate descriptors. */
    for (uint16_t i = 0; i < rxq.size; i++) {
        if (!rxq.buffers[i]) {
            rxq.buffers[i] = kmalloc(PKT_BUF_SIZE);
            if (!rxq.buffers[i]) break;
        }
        rxq.desc[i].addr  = (uint64_t)rxq.buffers[i];
        rxq.desc[i].len   = PKT_BUF_SIZE;
        rxq.desc[i].flags = VIRTQ_DESC_F_WRITE;
        rxq.desc[i].next  = 0;

        rxq.avail->ring[rxq.avail->idx % rxq.size] = i;
        rxq.avail->idx++;
    }

    /* Kick the RX queue so device knows buffers are available. */
    outw(iobase + VIRTIO_QUEUE_NOTIFY, 0);
}

/* ── Init ──────────────────────────────────────────────────────────────── */

int
virtio_net_init(void)
{
    const PciDevice *dev = pci_find_by_id(VIRTIO_VENDOR, VIRTIO_NET_DEV);
    if (!dev) return -1;

    /* BAR0 must be I/O port space. */
    if (!(dev->bar[0] & 0x01)) return -1;
    iobase = dev->bar[0] & 0xFFFC;

    /* Enable bus mastering. */
    pci_enable_bus_master((PciDevice *)dev);

    /* Reset device. */
    outb(iobase + VIRTIO_DEV_STATUS, 0);
    io_wait();

    /* Acknowledge. */
    outb(iobase + VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK);
    outb(iobase + VIRTIO_DEV_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* Feature negotiation: accept MAC feature (bit 5). */
    uint32_t features = inl(iobase + VIRTIO_DEV_FEATURES);
    outl(iobase + VIRTIO_GUEST_FEATURES, features & (1u << 5));

    /* Set up queues. */
    if (virtq_init(&rxq, 0) < 0) return -1;
    if (virtq_init(&txq, 1) < 0) return -1;

    /* Driver ready. */
    outb(iobase + VIRTIO_DEV_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* Read MAC address. */
    for (int i = 0; i < 6; i++)
        mac_addr[i] = inb(iobase + VIRTIO_NET_MAC + i);

    /* Fill RX queue with buffers. */
    rx_fill();

    initialised = true;
    kprintf("virtio-net: %02x:%02x:%02x:%02x:%02x:%02x on port 0x%x\n",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5],
            (unsigned)iobase);
    return 0;
}

bool
virtio_net_available(void)
{
    return initialised;
}

void
virtio_net_get_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++)
        mac[i] = mac_addr[i];
}

/* ── TX ────────────────────────────────────────────────────────────────── */

static uint16_t tx_next_desc;

int
virtio_net_tx(const void *frame, uint16_t len)
{
    if (!initialised || len > 1518) return -1;

    uint16_t di = tx_next_desc % txq.size;

    /* Allocate TX buffer if needed. */
    if (!txq.buffers[di])
        txq.buffers[di] = kmalloc(PKT_BUF_SIZE);
    if (!txq.buffers[di]) return -1;

    /* Prepend virtio-net header (all zeros). */
    memset(txq.buffers[di], 0, VIRTIO_NET_HDR_SIZE);
    memcpy(txq.buffers[di] + VIRTIO_NET_HDR_SIZE, frame, len);

    txq.desc[di].addr  = (uint64_t)txq.buffers[di];
    txq.desc[di].len   = VIRTIO_NET_HDR_SIZE + len;
    txq.desc[di].flags = 0;
    txq.desc[di].next  = 0;

    txq.avail->ring[txq.avail->idx % txq.size] = di;
    txq.avail->idx++;
    tx_next_desc++;

    /* Memory barrier before notification. */
    __asm__ volatile("mfence" ::: "memory");

    /* Notify device. */
    outw(iobase + VIRTIO_QUEUE_NOTIFY, 1);

    return 0;
}

/* ── RX ────────────────────────────────────────────────────────────────── */

int
virtio_net_rx_poll(void (*cb)(const void *frame, uint16_t len, void *ctx),
                   void *ctx)
{
    if (!initialised) return 0;

    int count = 0;
    while (rxq.last_used != rxq.used->idx) {
        uint16_t ui = rxq.last_used % rxq.size;
        uint32_t di = rxq.used->ring[ui].id;
        uint32_t plen = rxq.used->ring[ui].len;

        if (di >= rxq.size) { rxq.last_used++; continue; }

        if (plen > VIRTIO_NET_HDR_SIZE && rxq.buffers[di]) {
            cb(rxq.buffers[di] + VIRTIO_NET_HDR_SIZE,
               (uint16_t)(plen - VIRTIO_NET_HDR_SIZE), ctx);
        }

        /* Re-queue the descriptor. */
        rxq.desc[di].addr  = (uint64_t)rxq.buffers[di];
        rxq.desc[di].len   = PKT_BUF_SIZE;
        rxq.desc[di].flags = VIRTQ_DESC_F_WRITE;
        rxq.avail->ring[rxq.avail->idx % rxq.size] = (uint16_t)di;
        rxq.avail->idx++;

        rxq.last_used++;
        count++;
    }

    if (count > 0)
        outw(iobase + VIRTIO_QUEUE_NOTIFY, 0);

    return count;
}
