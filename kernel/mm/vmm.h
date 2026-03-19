/*
 * vmm.h — Virtual memory manager.
 *
 * Sets up a new 4-level page table hierarchy (PML4 → PDPT → PD → PT)
 * and provides functions to map/unmap virtual addresses to physical
 * page frames.
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

/* Page table entry flags (Intel SDM Vol. 3A, Table 4-19). */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_WRITETHROUGH (1ULL << 3)
#define PTE_NOCACHE   (1ULL << 4)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_HUGE      (1ULL << 7)   /* 2 MiB page in PD, 1 GiB in PDPT */
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

void vmm_init(void);
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t virt);

/* Mark a 2 MiB huge page as user-accessible (for Ring 3 code execution). */
void vmm_mark_huge_user(uint64_t virt);

/* Create a new PML4 with the kernel half (upper entries) cloned from
 * the current kernel page tables.  Returns the PML4's physical address
 * (identity-mapped, so also usable as a pointer). */
uint64_t *vmm_create_address_space(void);

/* Switch to a different address space (load CR3). */
void vmm_switch_address_space(uint64_t *pml4);

/* Map a page in a specific address space's PML4. */
void vmm_map_page_in(uint64_t *target_pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/* Get the kernel PML4 pointer. */
uint64_t *vmm_get_kernel_pml4(void);

/* Read the current CR3. */
static inline uint64_t vmm_get_cr3(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

#endif /* VMM_H */
