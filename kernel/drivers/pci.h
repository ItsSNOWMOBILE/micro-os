/*
 * pci.h -- PCI bus enumeration and configuration space access.
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* PCI configuration space I/O ports. */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* BDF address for CONFIG_ADDR (bit 31 = enable). */
#define PCI_ADDR(bus, dev, func, off) \
    (0x80000000u | ((uint32_t)(bus) << 16) | ((uint32_t)(dev) << 11) | \
     ((uint32_t)(func) << 8) | ((uint32_t)(off) & 0xFC))

/* Standard PCI header offsets. */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION        0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D
#define PCI_SECONDARY_BUS   0x19

/* PCI command register bits. */
#define PCI_CMD_IO          0x0001
#define PCI_CMD_MEMORY      0x0002
#define PCI_CMD_BUS_MASTER  0x0004

/* PCI device class codes. */
#define PCI_CLASS_STORAGE   0x01
#define PCI_CLASS_NETWORK   0x02
#define PCI_CLASS_DISPLAY   0x03
#define PCI_CLASS_BRIDGE    0x06
#define PCI_CLASS_SERIAL    0x0C

/* Maximum devices we track. */
#define PCI_MAX_DEVICES     64

/* BAR types. */
#define PCI_BAR_IO          0x01
#define PCI_BAR_MEM32       0x00
#define PCI_BAR_MEM64       0x04

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint32_t bar[6];
} PciDevice;

/* Read/write PCI config space. */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t val);

/* Enable bus mastering for a device. */
void pci_enable_bus_master(PciDevice *dev);

/* Scan all PCI buses and populate device list. */
void pci_init(void);

/* Get the discovered device list. */
int              pci_device_count(void);
const PciDevice *pci_get_device(int index);

/* Find a device by class/subclass. Returns NULL if not found. */
const PciDevice *pci_find_device(uint8_t class_code, uint8_t subclass);

/* Find a device by vendor/device ID. Returns NULL if not found. */
const PciDevice *pci_find_by_id(uint16_t vendor, uint16_t device);

/* Print all discovered devices (for lspci shell command). */
void pci_list_devices(void);

#endif /* PCI_H */
