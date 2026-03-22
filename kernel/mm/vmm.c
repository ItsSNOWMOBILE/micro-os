/*
 * vmm.c — Virtual memory manager.
 *
 * Creates a new PML4 and identity-maps the first 4 GiB (using 2 MiB
 * huge pages) plus the framebuffer region.  Then switches CR3.
 *
 * The initial page tables are statically allocated in the kernel BSS
 * to avoid a chicken-and-egg problem with the PMM: the PMM returns
 * physical addresses that may not be identity-mapped in the UEFI
 * firmware's page tables.
 *
 * After init, vmm_map_page / vmm_unmap_page use the PMM to allocate
 * page tables dynamically (since our identity mapping is now active).
 */

#include "vmm.h"
#include "pmm.h"
#include "../string.h"
#include "../console.h"
#include "../serial.h"

/* Page table entry type. */
typedef uint64_t pte_t;

/*
 * Static page tables for the initial identity mapping.
 * We need: 1 PML4 + 1 PDPT + 4 PDs = 6 pages = 24 KiB.
 * Each table is 512 entries × 8 bytes = 4096 bytes.
 */
static pte_t init_pml4[512] __attribute__((aligned(4096)));
static pte_t init_pdpt[512] __attribute__((aligned(4096)));
static pte_t init_pd[4][512] __attribute__((aligned(4096)));

static pte_t *pml4;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static pte_t *
alloc_table(void)
{
    pte_t *table = (pte_t *)pmm_alloc_page();
    return table;
}

static inline uint64_t pml4_index(uint64_t virt) { return (virt >> 39) & 0x1FF; }
static inline uint64_t pdpt_index(uint64_t virt) { return (virt >> 30) & 0x1FF; }
static inline uint64_t pd_index(uint64_t virt)   { return (virt >> 21) & 0x1FF; }
static inline uint64_t pt_index(uint64_t virt)   { return (virt >> 12) & 0x1FF; }

static pte_t *
walk(uint64_t virt, bool create, uint64_t flags)
{
    pte_t *table = pml4;
    /* Intermediate entries must be at least as permissive as the leaf.
     * Propagate PTE_USER so user-mode pages are reachable. */
    uint64_t inter = PTE_PRESENT | PTE_WRITABLE;
    if (flags & PTE_USER) inter |= PTE_USER;

    /* PML4 → PDPT */
    uint64_t idx = pml4_index(virt);
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | inter;
    } else if (create) {
        table[idx] |= inter;  /* widen permissions if needed */
    }
    table = (pte_t *)(table[idx] & PTE_ADDR_MASK);

    /* PDPT → PD */
    idx = pdpt_index(virt);
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | inter;
    } else if (create) {
        table[idx] |= inter;
    }
    table = (pte_t *)(table[idx] & PTE_ADDR_MASK);

    /* PD → PT */
    idx = pd_index(virt);
    if (table[idx] & PTE_HUGE)
        return NULL;  /* 2 MiB page — can't drill down further */
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | inter;
    } else if (create) {
        table[idx] |= inter;
    }
    table = (pte_t *)(table[idx] & PTE_ADDR_MASK);

    return &table[pt_index(virt)];
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void
vmm_init(void)
{
    pml4 = init_pml4;

    memset(init_pml4, 0, sizeof(init_pml4));
    memset(init_pdpt, 0, sizeof(init_pdpt));

    /* PTE_USER on intermediates is permissive — leaf entries still control
     * actual access.  Setting it here allows user-mode pages to exist
     * anywhere under PML4[0] without needing to retroactively fix parents. */
    init_pml4[0] = (uint64_t)init_pdpt | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /*
     * Identity-map the first 4 GiB using 2 MiB huge pages.
     * PML4[0] → PDPT[0..3] → PD[0..511] each with PTE_HUGE.
     */
    for (int i = 0; i < 4; i++) {
        memset(init_pd[i], 0, sizeof(init_pd[i]));
        init_pdpt[i] = (uint64_t)init_pd[i] | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

        for (int j = 0; j < 512; j++) {
            uint64_t phys = (uint64_t)i * (512ULL * 0x200000)
                          + (uint64_t)j * 0x200000;
            init_pd[i][j] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
        }
    }

    /* Load the new PML4 into CR3. */
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");
}

void
vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    pte_t *entry = walk(virt, true, flags);
    if (entry)
        *entry = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void
vmm_unmap_page(uint64_t virt)
{
    pte_t *entry = walk(virt, false, 0);
    if (entry)
        *entry = 0;

    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void
vmm_map_mmio(uint64_t phys, uint64_t size)
{
    /* Round down to 2 MiB boundary, round up size. */
    uint64_t start = phys & ~(0x200000ULL - 1);
    uint64_t end   = (phys + size + 0x200000ULL - 1) & ~(0x200000ULL - 1);

    uint64_t flags = PTE_PRESENT | PTE_WRITABLE;

    for (uint64_t addr = start; addr < end; addr += 0x200000) {
        uint64_t i4 = pml4_index(addr);
        uint64_t i3 = pdpt_index(addr);
        uint64_t i2 = pd_index(addr);

        /* Ensure PML4 entry exists. */
        if (!(pml4[i4] & PTE_PRESENT)) {
            pte_t *pdpt = alloc_table();
            if (!pdpt) return;
            memset(pdpt, 0, 4096);
            pml4[i4] = (uint64_t)pdpt | flags;
        }

        /* Ensure PDPT entry exists. */
        pte_t *pdpt = (pte_t *)(pml4[i4] & PTE_ADDR_MASK);
        if (!(pdpt[i3] & PTE_PRESENT)) {
            pte_t *pd = alloc_table();
            if (!pd) return;
            memset(pd, 0, 4096);
            pdpt[i3] = (uint64_t)pd | flags;
        }

        /* Set 2 MiB huge page in PD. */
        pte_t *pd = (pte_t *)(pdpt[i3] & PTE_ADDR_MASK);
        pd[i2] = addr | PTE_PRESENT | PTE_WRITABLE | PTE_HUGE
                | PTE_NOCACHE | PTE_WRITETHROUGH;

        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
    }
}

void
vmm_mark_huge_user(uint64_t virt)
{
    /* Mark a 2 MiB huge page entry as user-accessible.
     * Used to allow Ring 3 code to execute from identity-mapped kernel pages. */
    uint64_t idx_pdpt = pdpt_index(virt);
    uint64_t idx_pd   = pd_index(virt);

    /* For addresses in the first 4 GiB, we use the static init tables. */
    pte_t *pdpt = (pte_t *)(pml4[pml4_index(virt)] & PTE_ADDR_MASK);
    pte_t *pd   = (pte_t *)(pdpt[idx_pdpt] & PTE_ADDR_MASK);

    if (pd[idx_pd] & PTE_HUGE)
        pd[idx_pd] |= PTE_USER;
}

uint64_t *
vmm_get_kernel_pml4(void)
{
    return pml4;
}

uint64_t *
vmm_create_address_space(void)
{
    /* Allocate a fresh PML4 page. */
    pte_t *new_pml4 = alloc_table();
    if (!new_pml4) return NULL;

    /* Clone the kernel's PML4 entries.  Since all kernel mappings live
     * in PML4[0] (identity-mapped first 4 GiB), we copy all 512 entries
     * so the new address space has full access to kernel memory. User
     * pages will be mapped into the lower half at unique addresses. */
    for (int i = 0; i < 512; i++)
        new_pml4[i] = pml4[i];

    return new_pml4;
}

void
vmm_switch_address_space(uint64_t *target_pml4)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint64_t)target_pml4) : "memory");
}

/* Walk page tables in a specific PML4 (not necessarily the active one). */
static pte_t *
walk_in(pte_t *root, uint64_t virt, bool create, uint64_t flags)
{
    pte_t *table = root;
    uint64_t inter = PTE_PRESENT | PTE_WRITABLE;
    if (flags & PTE_USER) inter |= PTE_USER;

    uint64_t idx = pml4_index(virt);
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | inter;
    } else if (create) {
        table[idx] |= inter;
    }
    table = (pte_t *)(table[idx] & PTE_ADDR_MASK);

    idx = pdpt_index(virt);
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | inter;
    } else if (create) {
        table[idx] |= inter;
    }
    table = (pte_t *)(table[idx] & PTE_ADDR_MASK);

    idx = pd_index(virt);
    if (table[idx] & PTE_HUGE)
        return NULL;
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | inter;
    } else if (create) {
        table[idx] |= inter;
    }
    table = (pte_t *)(table[idx] & PTE_ADDR_MASK);

    return &table[pt_index(virt)];
}

void
vmm_map_page_in(uint64_t *target_pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    pte_t *entry = walk_in((pte_t *)target_pml4, virt, true, flags);
    if (entry)
        *entry = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
}
