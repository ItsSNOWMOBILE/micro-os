/*
 * UEFI bootloader for micro-os.
 *
 * Responsibilities:
 *   1. Obtain a linear framebuffer via GOP.
 *   2. Retrieve the UEFI memory map.
 *   3. Load the kernel from the EFI system partition.
 *   4. Exit boot services.
 *   5. Jump to the kernel entry point, passing a BootInfo struct.
 *
 * The kernel binary is expected at \EFI\BOOT\KERNEL.BIN on the ESP,
 * loaded as a flat binary at a fixed physical address.
 */

#include "uefi.h"
#include "bootinfo.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void
print(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, const char *s)
{
    CHAR16 buf[2] = { 0, 0 };
    while (*s) {
        if (*s == '\n') {
            buf[0] = L'\r';
            out->OutputString(out, buf);
        }
        buf[0] = (CHAR16)*s++;
        out->OutputString(out, buf);
    }
}

static void
print_hex(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, uint64_t val)
{
    CHAR16 buf[19]; /* "0x" + 16 hex + null */
    buf[0] = L'0';
    buf[1] = L'x';
    for (int i = 15; i >= 0; i--) {
        uint8_t nib = (val >> (i * 4)) & 0xF;
        buf[2 + (15 - i)] = nib < 10 ? L'0' + nib : L'A' + nib - 10;
    }
    buf[18] = 0;
    out->OutputString(out, buf);
}

static void *
memset_boot(void *dst, int c, uint64_t n)
{
    uint8_t *p = (uint8_t *)dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}

static void *
memcpy_boot(void *dst, const void *src, uint64_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ── GOP: obtain framebuffer ─────────────────────────────────────────────── */

static EFI_STATUS
init_gop(EFI_BOOT_SERVICES *bs, Framebuffer *fb)
{
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;

    EFI_STATUS status = bs->LocateProtocol(&gop_guid, NULL, (void **)&gop);
    if (status != EFI_SUCCESS || !gop)
        return status;

    fb->base   = gop->Mode->FrameBufferBase;
    fb->width  = gop->Mode->Info->HorizontalResolution;
    fb->height = gop->Mode->Info->VerticalResolution;
    fb->pitch  = gop->Mode->Info->PixelsPerScanLine * 4;

    switch (gop->Mode->Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
        fb->format = PIXEL_FORMAT_RGBX;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:
        fb->format = PIXEL_FORMAT_BGRX;
        break;
    default:
        fb->format = PIXEL_FORMAT_BGRX;
        break;
    }

    return EFI_SUCCESS;
}

/* ── Memory map translation ──────────────────────────────────────────────── */

static MmapEntryType
translate_efi_memory_type(uint32_t type)
{
    switch (type) {
    case EfiConventionalMemory:
    case EfiBootServicesCode:
    case EfiBootServicesData:
        return MMAP_USABLE;
    case EfiLoaderCode:
    case EfiLoaderData:
        return MMAP_BOOTLOADER_RECLAIMABLE;
    case EfiACPIReclaimMemory:
        return MMAP_ACPI_RECLAIMABLE;
    case EfiACPIMemoryNVS:
        return MMAP_ACPI_NVS;
    case EfiUnusableMemory:
        return MMAP_BAD_MEMORY;
    default:
        return MMAP_RESERVED;
    }
}

static void
build_mmap(BootInfo *info, uint8_t *efi_mmap, UINTN mmap_size,
           UINTN desc_size)
{
    info->mmap_count = 0;

    for (UINTN off = 0; off < mmap_size && info->mmap_count < BOOT_MMAP_MAX_ENTRIES;
         off += desc_size)
    {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(efi_mmap + off);
        MmapEntry *e = &info->mmap[info->mmap_count++];
        e->base   = desc->PhysicalStart;
        e->length = desc->NumberOfPages * 4096;
        e->type   = translate_efi_memory_type(desc->Type);
    }
}

/* ── Kernel loading ──────────────────────────────────────────────────────── */

/*
 * For now, the kernel is embedded at compile time using objcopy.
 * A production bootloader would read it from the filesystem via the
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.  We'll evolve to that later.
 *
 * The kernel binary is linked to extern symbols injected by the linker:
 */
extern uint8_t _binary_kernel_bin_start[];
extern uint8_t _binary_kernel_bin_end[];

#define KERNEL_LOAD_ADDR 0x100000  /* 1 MiB */

typedef void (EFIAPI *KernelEntry)(BootInfo *info);

/* ── Entry point ─────────────────────────────────────────────────────────── */

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st)
{
    EFI_BOOT_SERVICES              *bs  = st->BootServices;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = st->ConOut;

    out->ClearScreen(out);
    print(out, "micro-os bootloader v0.1\n");

    /* Disable watchdog timer so UEFI doesn't reboot us after 5 min. */
    bs->SetWatchdogTimer(0, 0, 0, NULL);

    /* ── Framebuffer ─────────────────────────────────────────────────── */

    static BootInfo boot_info;
    memset_boot(&boot_info, 0, sizeof(boot_info));

    EFI_STATUS status = init_gop(bs, &boot_info.fb);
    if (status != EFI_SUCCESS) {
        print(out, "ERROR: failed to initialise GOP\n");
        goto halt;
    }

    print(out, "GOP: ");
    print_hex(out, boot_info.fb.width);
    print(out, " x ");
    print_hex(out, boot_info.fb.height);
    print(out, " @ ");
    print_hex(out, boot_info.fb.base);
    print(out, "\n");

    /* ── Copy kernel to its load address ─────────────────────────────── */

    uint64_t kernel_size = (uint64_t)(_binary_kernel_bin_end -
                                      _binary_kernel_bin_start);
    /*
     * The flat binary does not include BSS, but the kernel needs BSS
     * pages to be allocated and zeroed.  Over-allocate: the kernel is
     * unlikely to exceed 256 KiB total (code + data + BSS).
     */
    uint64_t alloc_size = kernel_size < 0x40000 ? 0x40000 : kernel_size * 2;
    UINTN pages = (alloc_size + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS kernel_addr = KERNEL_LOAD_ADDR;

    status = bs->AllocatePages(AllocateAddress, EfiLoaderData,
                               pages, &kernel_addr);
    if (status != EFI_SUCCESS) {
        print(out, "ERROR: failed to allocate pages for kernel\n");
        goto halt;
    }

    memcpy_boot((void *)kernel_addr, _binary_kernel_bin_start, kernel_size);

    boot_info.kernel_phys_base = KERNEL_LOAD_ADDR;
    boot_info.kernel_size      = kernel_size;

    print(out, "Kernel loaded at ");
    print_hex(out, KERNEL_LOAD_ADDR);
    print(out, " (");
    print_hex(out, kernel_size);
    print(out, " bytes)\n");

    /* ── Get memory map and exit boot services ───────────────────────── */

    UINTN    mmap_size = 0;
    UINTN    map_key   = 0;
    UINTN    desc_size = 0;
    uint32_t desc_ver  = 0;
    uint8_t *mmap_buf  = NULL;

    /* First call to find required buffer size. */
    bs->GetMemoryMap(&mmap_size, NULL, &map_key, &desc_size, &desc_ver);
    mmap_size += 2 * desc_size; /* headroom for the AllocatePool itself */

    status = bs->AllocatePool(EfiLoaderData, mmap_size, (void **)&mmap_buf);
    if (status != EFI_SUCCESS) {
        print(out, "ERROR: failed to allocate memory map buffer\n");
        goto halt;
    }

    status = bs->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                              &map_key, &desc_size, &desc_ver);
    if (status != EFI_SUCCESS) {
        print(out, "ERROR: failed to get memory map\n");
        goto halt;
    }

    build_mmap(&boot_info, mmap_buf, mmap_size, desc_size);

    print(out, "Memory map: ");
    print_hex(out, boot_info.mmap_count);
    print(out, " entries\n");

    print(out, "Exiting boot services...\n");

    /*
     * ExitBootServices must be called with the current map key.
     * If it fails (because the map changed between GetMemoryMap and
     * ExitBootServices), re-fetch and retry once.
     */
    status = bs->ExitBootServices(image_handle, map_key);
    if (status != EFI_SUCCESS) {
        mmap_size = 0;
        bs->GetMemoryMap(&mmap_size, NULL, &map_key, &desc_size, &desc_ver);
        mmap_size += 2 * desc_size;
        bs->GetMemoryMap(&mmap_size, (EFI_MEMORY_DESCRIPTOR *)mmap_buf,
                         &map_key, &desc_size, &desc_ver);
        build_mmap(&boot_info, mmap_buf, mmap_size, desc_size);
        status = bs->ExitBootServices(image_handle, map_key);
        if (status != EFI_SUCCESS)
            goto halt;
    }

    /* ── Jump to kernel ──────────────────────────────────────────────── */

    KernelEntry entry = (KernelEntry)KERNEL_LOAD_ADDR;
    entry(&boot_info);

    /* Should never reach here. */
halt:
    for (;;)
        __asm__ volatile("hlt");

    return EFI_SUCCESS;
}
