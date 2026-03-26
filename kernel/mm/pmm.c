/*
 * pmm.c — Physical memory manager (bitmap allocator).
 *
 * Scans the memory map provided by the bootloader and builds a bitmap
 * where bit N corresponds to physical page N (address N * 4096).
 *
 * A set bit means the page is FREE; a clear bit means USED/RESERVED.
 * This convention lets us use FFS-style scanning to find free pages.
 *
 * The bitmap is placed at the start of the first usable region large
 * enough to hold it (after the kernel).
 */

#include "pmm.h"
#include "../../boot/bootinfo.h"
#include "../string.h"
#include "../sync.h"

static Spinlock  pmm_lock = SPINLOCK_INIT;
static uint64_t *bitmap;
static uint64_t  bitmap_size;    /* bytes */
static uint64_t  total_pages;
static uint64_t  free_count;
static uint64_t  highest_page;

/* ── Bit manipulation ────────────────────────────────────────────────────── */

static inline void
bitmap_set(uint64_t page)
{
    bitmap[page / 64] |= (1ULL << (page % 64));
}

static inline void
bitmap_clear(uint64_t page)
{
    bitmap[page / 64] &= ~(1ULL << (page % 64));
}

static inline int
bitmap_test(uint64_t page)
{
    return (bitmap[page / 64] >> (page % 64)) & 1;
}

static inline uint64_t
popcount64(uint64_t x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

/* Bulk set a range of bits (mark pages FREE). Returns count of newly set bits. */
static uint64_t
bitmap_set_range(uint64_t start, uint64_t end)
{
    uint64_t count = 0;
    while (start < end && (start % 64) != 0) {
        if (!bitmap_test(start)) { bitmap_set(start); count++; }
        start++;
    }
    while (start + 64 <= end) {
        uint64_t idx = start / 64;
        count += 64 - popcount64(bitmap[idx]);
        bitmap[idx] = ~0ULL;
        start += 64;
    }
    while (start < end) {
        if (!bitmap_test(start)) { bitmap_set(start); count++; }
        start++;
    }
    return count;
}

/* Bulk clear a range of bits (mark pages USED). Returns count of newly cleared bits. */
static uint64_t
bitmap_clear_range(uint64_t start, uint64_t end)
{
    uint64_t count = 0;
    while (start < end && (start % 64) != 0) {
        if (bitmap_test(start)) { bitmap_clear(start); count++; }
        start++;
    }
    while (start + 64 <= end) {
        uint64_t idx = start / 64;
        count += popcount64(bitmap[idx]);
        bitmap[idx] = 0;
        start += 64;
    }
    while (start < end) {
        if (bitmap_test(start)) { bitmap_clear(start); count++; }
        start++;
    }
    return count;
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

void
pmm_init(void *mmap, uint32_t mmap_count,
         uint64_t kernel_phys, uint64_t kernel_size)
{
    MmapEntry *entries = (MmapEntry *)mmap;

    /* Pass 1: find the highest usable address to size the bitmap. */
    highest_page = 0;
    for (uint32_t i = 0; i < mmap_count; i++) {
        uint64_t end = entries[i].base + entries[i].length;
        uint64_t end_page = end / PAGE_SIZE;
        if (end_page > highest_page)
            highest_page = end_page;
    }

    total_pages = highest_page;
    bitmap_size = (total_pages + 63) / 64 * 8;  /* round up to 8-byte units */

    /*
     * Pass 2: find a usable region large enough to hold the bitmap.
     * Skip the first 1 MiB (legacy BIOS area) and the kernel region.
     */
    bitmap = NULL;
    for (uint32_t i = 0; i < mmap_count; i++) {
        if (entries[i].type != MMAP_USABLE)
            continue;

        uint64_t base = entries[i].base;
        uint64_t len  = entries[i].length;

        /* Avoid the low 1 MiB. */
        if (base < 0x100000) {
            uint64_t skip = 0x100000 - base;
            if (skip >= len)
                continue;
            base += skip;
            len  -= skip;
        }

        /* Avoid the kernel region. */
        uint64_t kend = kernel_phys + kernel_size;
        if (base < kend && base + len > kernel_phys) {
            if (base < kend) {
                uint64_t skip = kend - base;
                if (skip >= len)
                    continue;
                base += skip;
                len  -= skip;
            }
        }

        /* Align base up to page boundary. */
        uint64_t aligned = (base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t diff = aligned - base;
        if (diff >= len)
            continue;
        len -= diff;
        base = aligned;

        if (len >= bitmap_size) {
            bitmap = (uint64_t *)base;
            break;
        }
    }

    if (!bitmap)
        return;  /* caller should panic */

    /* Mark everything as used initially. */
    memset(bitmap, 0, bitmap_size);
    free_count = 0;

    /* Pass 3: mark usable pages as free (bulk). */
    for (uint32_t i = 0; i < mmap_count; i++) {
        if (entries[i].type != MMAP_USABLE)
            continue;

        uint64_t base = entries[i].base;
        uint64_t end  = base + entries[i].length;

        /* Page-align inward. */
        base = (base + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        end  = end & ~(uint64_t)(PAGE_SIZE - 1);

        uint64_t start_page = base / PAGE_SIZE;
        uint64_t end_page   = end / PAGE_SIZE;
        if (end_page > total_pages) end_page = total_pages;
        free_count += bitmap_set_range(start_page, end_page);
    }

    /* Reserve the low 1 MiB, kernel, and bitmap (bulk). */
    free_count -= bitmap_clear_range(0, 0x100000 / PAGE_SIZE);

    uint64_t kstart_page = kernel_phys / PAGE_SIZE;
    uint64_t kend_page   = (kernel_phys + kernel_size + PAGE_SIZE - 1) / PAGE_SIZE;
    free_count -= bitmap_clear_range(kstart_page, kend_page);

    uint64_t bm_start = (uint64_t)bitmap / PAGE_SIZE;
    uint64_t bm_end   = ((uint64_t)bitmap + bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    free_count -= bitmap_clear_range(bm_start, bm_end);
}

/* ── Allocation / Free ───────────────────────────────────────────────────── */

/* Only hand out pages below 4 GiB — our identity map doesn't cover above. */
#define PMM_MAX_PAGE  (0x100000000ULL / PAGE_SIZE)

void *
pmm_alloc_page(void)
{
    spin_lock(&pmm_lock);
    uint64_t max_page = total_pages < PMM_MAX_PAGE ? total_pages : PMM_MAX_PAGE;
    uint64_t qwords = (max_page + 63) / 64;

    for (uint64_t i = 0; i < qwords; i++) {
        if (bitmap[i] == 0)
            continue;

        /* __builtin_ctzll: count trailing zeros — finds the lowest set bit. */
        int bit = __builtin_ctzll(bitmap[i]);
        uint64_t page = i * 64 + bit;
        if (page >= max_page) {
            spin_unlock(&pmm_lock);
            return NULL;
        }

        bitmap_clear(page);
        free_count--;
        spin_unlock(&pmm_lock);

        return (void *)(page * PAGE_SIZE);
    }

    spin_unlock(&pmm_lock);
    return NULL;
}

void
pmm_free_page(void *addr)
{
    uint64_t page = (uint64_t)addr / PAGE_SIZE;
    if (page >= total_pages)
        return;
    spin_lock(&pmm_lock);
    if (!bitmap_test(page)) {
        bitmap_set(page);
        free_count++;
    }
    spin_unlock(&pmm_lock);
}

uint64_t
pmm_free_pages(void)
{
    return free_count;
}

uint64_t
pmm_total_pages(void)
{
    return total_pages;
}
