/*
 * pmm.h — Physical memory manager.
 *
 * Bitmap-based page frame allocator.  Each bit in the bitmap represents
 * one 4 KiB physical page.  The bitmap itself is placed in the first
 * sufficiently large usable memory region reported by the bootloader.
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

void      pmm_init(void *mmap, uint32_t mmap_count,
                   uint64_t kernel_phys, uint64_t kernel_size);
void     *pmm_alloc_page(void);
void      pmm_free_page(void *addr);
uint64_t  pmm_free_pages(void);
uint64_t  pmm_total_pages(void);

#endif /* PMM_H */
