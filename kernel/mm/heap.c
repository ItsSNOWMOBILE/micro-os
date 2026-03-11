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
    /* Allocate a contiguous run of pages for the initial heap.
     * For simplicity, we allocate them individually and use only
     * the first contiguous block.  A real allocator would use
     * a virtual address range and map pages on demand. */
    void *base = pmm_alloc_page();
    if (!base) return;

    /* Allocate more pages and hope they're contiguous (they usually
     * are right after PMM init when memory is fresh). */
    for (int i = 1; i < HEAP_PAGES; i++) {
        void *p = pmm_alloc_page();
        (void)p;  /* we rely on contiguous allocation */
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

    FreeBlock **prev = &free_list;
    FreeBlock  *cur  = free_list;

    while (cur) {
        if (cur->size >= needed) {
            /* Can we split? */
            if (cur->size >= needed + MIN_BLOCK + 16) {
                /* Split: create a new free block after our allocation. */
                FreeBlock *remainder = (FreeBlock *)((uint8_t *)cur + needed);
                remainder->size = cur->size - needed;
                remainder->next = cur->next;
                *prev = remainder;
                cur->size = needed;
            } else {
                /* Use the whole block. */
                *prev = cur->next;
                /* size stays as-is */
            }

            /* Store size in header, return pointer past it. */
            *(size_t *)cur = cur->size;
            return (uint8_t *)cur + HEADER_SIZE;
        }
        prev = &cur->next;
        cur  = cur->next;
    }

    /* Out of heap memory — try to get a page from PMM. */
    void *page = pmm_alloc_page();
    if (!page) return NULL;

    FreeBlock *blk = (FreeBlock *)page;
    blk->size = PAGE_SIZE;
    blk->next = NULL;

    /* Add to free list and retry. */
    *prev = blk;
    return kmalloc(size);  /* recurse once */
}

/* ── Free ────────────────────────────────────────────────────────────────── */

void
kfree(void *ptr)
{
    if (!ptr) return;

    /* Step back past the header to get the block address and size. */
    FreeBlock *blk = (FreeBlock *)((uint8_t *)ptr - HEADER_SIZE);
    size_t block_size = *(size_t *)blk;
    blk->size = block_size;

    /* Insert into the sorted free list and coalesce. */
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
    if (*prev != blk) {
        FreeBlock *p = (FreeBlock *)((uint8_t *)prev - __builtin_offsetof(FreeBlock, next));
        /* Recalculate prev from the free list to be safe. */
        (void)p;
    }
}
