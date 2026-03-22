/*
 * xhci.h -- USB xHCI host controller driver.
 *
 * Minimal xHCI driver: controller init, port detection, device
 * enumeration via GET_DESCRIPTOR.  No class drivers.
 */

#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include <stdbool.h>

/* ── PCI identification ──────────────────────────────────────────────────── */

#define XHCI_CLASS      0x0C   /* Serial Bus Controller */
#define XHCI_SUBCLASS   0x03   /* USB */
#define XHCI_PROG_IF    0x30   /* xHCI */

/* ── Capability register offsets (from MMIO base) ────────────────────────── */

#define XHCI_CAP_CAPLENGTH   0x00
#define XHCI_CAP_HCIVERSION  0x02
#define XHCI_CAP_HCSPARAMS1  0x04
#define XHCI_CAP_HCSPARAMS2  0x08
#define XHCI_CAP_HCCPARAMS1  0x10
#define XHCI_CAP_DBOFF       0x14
#define XHCI_CAP_RTSOFF      0x18

/* ── Operational register offsets (from op_base) ─────────────────────────── */

#define XHCI_OP_USBCMD   0x00
#define XHCI_OP_USBSTS   0x04
#define XHCI_OP_PAGESIZE  0x08
#define XHCI_OP_DNCTRL   0x14
#define XHCI_OP_CRCR      0x18
#define XHCI_OP_DCBAAP    0x30
#define XHCI_OP_CONFIG    0x38
#define XHCI_OP_PORTSC(n) (0x400 + 0x10 * ((n) - 1))

/* USBCMD bits */
#define XHCI_CMD_RUN    (1u << 0)
#define XHCI_CMD_HCRST  (1u << 1)
#define XHCI_CMD_INTE   (1u << 2)

/* USBSTS bits */
#define XHCI_STS_HCH   (1u << 0)
#define XHCI_STS_CNR   (1u << 12)

/* PORTSC bits */
#define XHCI_PORTSC_CCS   (1u << 0)
#define XHCI_PORTSC_PED   (1u << 1)
#define XHCI_PORTSC_PR    (1u << 4)
#define XHCI_PORTSC_PP    (1u << 9)
#define XHCI_PORTSC_SPEED_MASK  (0xFu << 10)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_CSC   (1u << 17)
#define XHCI_PORTSC_PRC   (1u << 21)

/* PORTSC preserve mask: bits safe to keep on read-modify-write. */
#define XHCI_PORTSC_PRESERVE  0x0E00C3E0u
/* Change bits (write-1-to-clear). */
#define XHCI_PORTSC_CHANGE    0x00FE0002u

/* Port speed values */
#define XHCI_SPEED_FULL   1
#define XHCI_SPEED_LOW    2
#define XHCI_SPEED_HIGH   3
#define XHCI_SPEED_SUPER  4

/* ── Interrupter register offsets (from runtime base + 0x20) ─────────────── */

#define XHCI_IR_IMAN     0x00
#define XHCI_IR_IMOD     0x04
#define XHCI_IR_ERSTSZ   0x08
#define XHCI_IR_ERSTBA   0x10
#define XHCI_IR_ERDP     0x18

/* ── TRB (Transfer Request Block) ────────────────────────────────────────── */

typedef struct {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) XhciTrb;

/* TRB types (bits [15:10] of control). */
#define TRB_NORMAL        1
#define TRB_SETUP_STAGE   2
#define TRB_DATA_STAGE    3
#define TRB_STATUS_STAGE  4
#define TRB_LINK          6
#define TRB_ENABLE_SLOT   9
#define TRB_ADDRESS_DEV   11
#define TRB_NOOP_CMD      23
#define TRB_XFER_EVENT    32
#define TRB_CMD_COMPLETE  33
#define TRB_PORT_STATUS   34

#define TRB_TYPE(t)  ((uint32_t)(t) << 10)

/* ── Event Ring Segment Table Entry ──────────────────────────────────────── */

typedef struct {
    uint64_t base_addr;
    uint32_t size;
    uint32_t reserved;
} __attribute__((packed)) XhciErstEntry;

/* ── USB device descriptor ───────────────────────────────────────────────── */

typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) UsbDeviceDesc;

/* ── Enumerated USB device info ──────────────────────────────────────────── */

#define XHCI_MAX_DEVICES  16

typedef struct {
    bool     present;
    uint8_t  port;        /* root hub port (1-based) */
    uint8_t  slot_id;
    uint8_t  speed;       /* XHCI_SPEED_* */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  dev_class;
    uint8_t  dev_subclass;
    uint8_t  dev_protocol;
} XhciUsbDevice;

/* ── Public API ──────────────────────────────────────────────────────────── */

int  xhci_init(void);
bool xhci_available(void);
int  xhci_device_count(void);
const XhciUsbDevice *xhci_get_device(int index);
void xhci_list_devices(void);

#endif /* XHCI_H */
