/*
 * pci.c -- PCI bus enumeration via configuration mechanism #1.
 *
 * Scans all 256 buses, 32 devices per bus, 8 functions per device.
 * Reads vendor/device IDs, class codes, BARs, and IRQ lines.
 * Multi-function devices are detected via the header type register.
 */

#include "pci.h"
#include "../kernel.h"
#include "../console.h"

static PciDevice devices[PCI_MAX_DEVICES];
static int       num_devices;

/* ── Config space access ───────────────────────────────────────────────── */

uint32_t
pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, dev, func, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t
pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, dev, func, off));
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> ((off & 2) * 8));
}

uint8_t
pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, dev, func, off));
    return (uint8_t)(inl(PCI_CONFIG_DATA) >> ((off & 3) * 8));
}

void
pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val)
{
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, dev, func, off));
    outl(PCI_CONFIG_DATA, val);
}

void
pci_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint16_t val)
{
    outl(PCI_CONFIG_ADDR, PCI_ADDR(bus, dev, func, off));
    uint32_t tmp = inl(PCI_CONFIG_DATA);
    int shift = (off & 2) * 8;
    tmp &= ~(0xFFFFu << shift);
    tmp |= (uint32_t)val << shift;
    outl(PCI_CONFIG_DATA, tmp);
}

void
pci_enable_bus_master(PciDevice *dev)
{
    uint16_t cmd = pci_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY | PCI_CMD_IO;
    pci_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/* ── Scanning ──────────────────────────────────────────────────────────── */

static void
scan_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint16_t vendor = pci_read16(bus, dev, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF)
        return;

    if (num_devices >= PCI_MAX_DEVICES)
        return;

    PciDevice *d = &devices[num_devices++];
    d->bus       = bus;
    d->device    = dev;
    d->function  = func;
    d->vendor_id = vendor;
    d->device_id = pci_read16(bus, dev, func, PCI_DEVICE_ID);
    d->class_code = pci_read8(bus, dev, func, PCI_CLASS);
    d->subclass  = pci_read8(bus, dev, func, PCI_SUBCLASS);
    d->prog_if   = pci_read8(bus, dev, func, PCI_PROG_IF);
    d->header_type = pci_read8(bus, dev, func, PCI_HEADER_TYPE);
    d->irq_line  = pci_read8(bus, dev, func, PCI_INTERRUPT_LINE);

    /* Read BARs (only for type 0 headers — normal devices). */
    if ((d->header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; i++)
            d->bar[i] = pci_read32(bus, dev, func, PCI_BAR0 + i * 4);
    }
}

static void scan_bus(uint8_t bus);

static void
scan_device(uint8_t bus, uint8_t dev)
{
    uint16_t vendor = pci_read16(bus, dev, 0, PCI_VENDOR_ID);
    if (vendor == 0xFFFF)
        return;

    scan_function(bus, dev, 0);

    /* Check if multi-function device. */
    uint8_t header = pci_read8(bus, dev, 0, PCI_HEADER_TYPE);
    if (header & 0x80) {
        for (uint8_t func = 1; func < 8; func++)
            scan_function(bus, dev, func);
    }

    /* If this is a PCI-to-PCI bridge, scan the secondary bus. */
    uint8_t class = pci_read8(bus, dev, 0, PCI_CLASS);
    uint8_t subclass = pci_read8(bus, dev, 0, PCI_SUBCLASS);
    if (class == PCI_CLASS_BRIDGE && subclass == 0x04) {
        uint8_t secondary = pci_read8(bus, dev, 0, PCI_SECONDARY_BUS);
        if (secondary != 0)
            scan_bus(secondary);
    }
}

static void
scan_bus(uint8_t bus)
{
    for (uint8_t dev = 0; dev < 32; dev++)
        scan_device(bus, dev);
}

void
pci_init(void)
{
    num_devices = 0;

    /* Check if host bridge is multi-function (multiple PCI domains). */
    uint8_t header = pci_read8(0, 0, 0, PCI_HEADER_TYPE);
    if (header & 0x80) {
        for (uint8_t func = 0; func < 8; func++) {
            if (pci_read16(0, 0, func, PCI_VENDOR_ID) != 0xFFFF)
                scan_bus(func);
        }
    } else {
        scan_bus(0);
    }

    kprintf("PCI: found %d device%s\n", num_devices,
            num_devices == 1 ? "" : "s");
}

/* ── Queries ───────────────────────────────────────────────────────────── */

int
pci_device_count(void)
{
    return num_devices;
}

const PciDevice *
pci_get_device(int index)
{
    if (index < 0 || index >= num_devices)
        return NULL;
    return &devices[index];
}

const PciDevice *
pci_find_device(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < num_devices; i++) {
        if (devices[i].class_code == class_code &&
            devices[i].subclass == subclass)
            return &devices[i];
    }
    return NULL;
}

const PciDevice *
pci_find_by_id(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < num_devices; i++) {
        if (devices[i].vendor_id == vendor &&
            devices[i].device_id == device)
            return &devices[i];
    }
    return NULL;
}

/* ── Human-readable class names ────────────────────────────────────────── */

static const char *
class_name(uint8_t class, uint8_t subclass)
{
    switch (class) {
    case 0x00:
        return subclass == 0x00 ? "Legacy device" : "VGA-compatible";
    case 0x01:
        switch (subclass) {
        case 0x00: return "SCSI controller";
        case 0x01: return "IDE controller";
        case 0x05: return "ATA controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "Storage controller";
        }
    case 0x02:
        return subclass == 0x00 ? "Ethernet controller" : "Network controller";
    case 0x03:
        return subclass == 0x00 ? "VGA controller" : "Display controller";
    case 0x04:
        return "Multimedia device";
    case 0x05:
        return "Memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-PCI bridge";
        default:   return "Bridge device";
        }
    case 0x07:
        return "Serial controller";
    case 0x08:
        return "System peripheral";
    case 0x0C:
        switch (subclass) {
        case 0x00: return "FireWire controller";
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "Serial bus controller";
        }
    case 0x0D:
        return "Wireless controller";
    default:
        return "Unknown device";
    }
}

void
pci_list_devices(void)
{
    kprintf("%-7s %-6s %-6s %-5s %s\n",
            "BDF", "VID", "DID", "IRQ", "Description");
    for (int i = 0; i < num_devices; i++) {
        const PciDevice *d = &devices[i];
        kprintf("%d:%2d.%d  %4lx  %4lx  %-5d %s\n",
                (int)d->bus, (int)d->device, (int)d->function,
                (uint64_t)d->vendor_id, (uint64_t)d->device_id,
                (int)d->irq_line,
                class_name(d->class_code, d->subclass));
    }
}
