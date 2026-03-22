/*
 * heap.c — Kernel heap allocator.
 *
 * Uses a simple first-fit free-list.  The heap grows by requesting
 * pages from the PMM.  Each allocation has an 8-byte header storing
 * the block size.  Free blocks are linked into a sorted free list and
 * coalesced on free.
 */

#include "heap.h"
#include "pmm.h"
#include "../string.h"

typedef struct FreeBlock {
    size_t             size;
    struct FreeBlock  *next;
} FreeBlock;

#define HEADER_SIZE   sizeof(size_t)
#define MIN_BLOCK     (sizeof(FreeBlock))
#define HEAP_PAGES    64        /* initial heap: 256 KiB */

static FreeBlock *free_list;

/* ── Initialisation ──────────────────────────────────────────────────────── */

void
heap_init(void)
{
    void *base = pmm_alloc_page();
    if (!base) return;

    /* Allocate remaining pages and verify contiguity. */
    for (int i = 1; i < HEAP_PAGES; i++) {
        void *p = pmm_alloc_page();
        void *expected = (uint8_t *)base + (size_t)i * PAGE_SIZE;
        if (p != expected) {
            /* Non-contiguous — use only what we have so far. */
            if (p) pmm_free_page(p);
            size_t total = (size_t)i * PAGE_SIZE;
            free_list = (FreeBlock *)base;
            free_list->size = total;
            free_list->next = NULL;
            return;
        }
    }

    size_t total = HEAP_PAGES * PAGE_SIZE;

    free_list = (FreeBlock *)base;
    free_list->size = total;
    free_list->next = NULL;
}

/* ── Allocation ──────────────────────────────────────────────────────────── */

void *
kmalloc(size_t size)
{
    if (size == 0) return NULL;

    /* Align to 16 bytes, add header. */
    size = (size + 15) & ~(size_t)15;
    size_t needed = size + HEADER_SIZE;
    if (needed < MIN_BLOCK)
        needed = MIN_BLOCK;

    /* Try twice: once with existing heap, once after growing. */
    for (int attempt = 0; attempt < 2; attempt++) {
        FreeBlock **prev = &free_list;
        FreeBlock  *cur  = free_list;

        while (cur) {
            if (cur->size >= needed) {
                if (cur->size >= needed + MIN_BLOCK + 16) {
                    FreeBlock *remainder = (FreeBlock *)((uint8_t *)cur + needed);
                    remainder->size = cur->size - needed;
                    remainder->next = cur->next;
                    *prev = remainder;
                    cur->size = needed;
                } else {
                    *prev = cur->next;
                }

                *(size_t *)cur = cur->size;
                return (uint8_t *)cur + HEADER_SIZE;
            }
            prev = &cur->next;
            cur  = cur->next;
        }

        if (attempt > 0) break;

        /* Grow heap: allocate contiguous pages from PMM. */
        size_t pages_needed = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages_needed < 4) pages_needed = 4;

        void *base = pmm_alloc_page();
        if (!base) return NULL;

        size_t got = 1;
        for (size_t i = 1; i < pages_needed; i++) {
            void *p = pmm_alloc_page();
            if (!p) break;
            if (p != (uint8_t *)base + i * PAGE_SIZE) {
                pmm_free_page(p);
                break;
            }
            got++;
        }

        /* Add the new region to the free list via kfree. */
        FreeBlock *blk = (FreeBlock *)base;
        *(size_t *)blk = got * PAGE_SIZE;  /* write header for kfree */
        kfree((uint8_t *)blk + HEADER_SIZE);
    }

    return NULL;
}

/* ── Aligned allocation ──────────────────────────────────────────────────── */

void *
kmalloc_aligned(size_t size, size_t align)
{
    if (size == 0 || align == 0) return NULL;
    /* Over-allocate so we can align within the buffer and store
     * the original pointer just before the aligned address. */
    void *raw = kmalloc(size + align + sizeof(void *));
    if (!raw) return NULL;
    uintptr_t addr = ((uintptr_t)raw + sizeof(void *) + align - 1) & ~(align - 1);
    ((void **)addr)[-1] = raw;
    return (void *)addr;
}

/* ── Free ────────────────────────────────────────────────────────────────── */

void
kfree(void *ptr)
{
    if (!ptr) return;

    FreeBlock *blk = (FreeBlock *)((uint8_t *)ptr - HEADER_SIZE);
    size_t block_size = *(size_t *)blk;
    blk->size = block_size;

    /* Insert into the sorted free list. */
    FreeBlock **prev = &free_list;
    FreeBlock  *cur  = free_list;

    while (cur && cur < blk) {
        prev = &cur->next;
        cur  = cur->next;
    }

    blk->next = cur;
    *prev = blk;

    /* Coalesce with next block. */
    if (cur && (uint8_t *)blk + blk->size == (uint8_t *)cur) {
        blk->size += cur->size;
        blk->next  = cur->next;
    }

    /* Coalesce with previous block. */
    if (prev != &free_list) {
        FreeBlock *p = (FreeBlock *)((uint8_t *)prev -
                        __builtin_offsetof(FreeBlock, next));
        if ((uint8_t *)p + p->size == (uint8_t *)blk) {
            p->size += blk->size;
            p->next  = blk->next;
        }
    }
}
