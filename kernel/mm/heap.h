/*
 * heap.h — Kernel heap allocator.
 *
 * Simple first-fit free-list allocator backed by physical pages from
 * the PMM.  Suitable for small kernel-internal allocations (task
 * structs, buffers, etc.).
 */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

void  heap_init(void);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void  kfree(void *ptr);

#endif /* HEAP_H */
