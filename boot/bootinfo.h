/*
 * bootinfo.h — Data passed from the UEFI bootloader to the kernel.
 *
 * The bootloader fills a BootInfo struct before exiting boot services
 * and jumping to the kernel entry point.  The kernel must treat the
 * memory map as read-only: it is the authoritative source for which
 * physical pages are free.
 */

#ifndef BOOTINFO_H
#define BOOTINFO_H

#include <stdint.h>

#define BOOT_MMAP_MAX_ENTRIES 256

/* Framebuffer pixel format (matches EFI_GRAPHICS_PIXEL_FORMAT). */
typedef enum {
    PIXEL_FORMAT_RGBX,  /* PixelRedGreenBlueReserved8BitPerColor  */
    PIXEL_FORMAT_BGRX,  /* PixelBlueGreenRedReserved8BitPerColor  */
} PixelFormat;

typedef struct {
    uint64_t    base;
    uint32_t    width;
    uint32_t    height;
    uint32_t    pitch;          /* bytes per scan line */
    PixelFormat format;
} Framebuffer;

/* Simplified memory region descriptor. */
typedef enum {
    MMAP_USABLE,
    MMAP_RESERVED,
    MMAP_ACPI_RECLAIMABLE,
    MMAP_ACPI_NVS,
    MMAP_BAD_MEMORY,
    MMAP_BOOTLOADER_RECLAIMABLE,
    MMAP_KERNEL,
    MMAP_FRAMEBUFFER,
} MmapEntryType;

typedef struct {
    uint64_t      base;
    uint64_t      length;
    MmapEntryType type;
} MmapEntry;

typedef struct {
    Framebuffer fb;

    uint32_t    mmap_count;
    MmapEntry   mmap[BOOT_MMAP_MAX_ENTRIES];

    uint64_t    kernel_phys_base;
    uint64_t    kernel_size;

    /* RSDP physical address (for ACPI). */
    uint64_t    rsdp_address;
} BootInfo;

#endif /* BOOTINFO_H */
