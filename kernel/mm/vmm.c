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
walk(uint64_t virt, bool create)
{
    pte_t *table = pml4;

    /* PML4 → PDPT */
    uint64_t idx = pml4_index(virt);
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | PTE_PRESENT | PTE_WRITABLE;
    }
    table = (pte_t *)(table[idx] & PTE_ADDR_MASK);

    /* PDPT → PD */
    idx = pdpt_index(virt);
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create) return NULL;
        pte_t *next = alloc_table();
        if (!next) return NULL;
        table[idx] = (uint64_t)next | PTE_PRESENT | PTE_WRITABLE;
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
        table[idx] = (uint64_t)next | PTE_PRESENT | PTE_WRITABLE;
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

    init_pml4[0] = (uint64_t)init_pdpt | PTE_PRESENT | PTE_WRITABLE;

    /*
     * Identity-map the first 4 GiB using 2 MiB huge pages.
     * PML4[0] → PDPT[0..3] → PD[0..511] each with PTE_HUGE.
     */
    for (int i = 0; i < 4; i++) {
        memset(init_pd[i], 0, sizeof(init_pd[i]));
        init_pdpt[i] = (uint64_t)init_pd[i] | PTE_PRESENT | PTE_WRITABLE;

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
    pte_t *entry = walk(virt, true);
    if (entry)
        *entry = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

void
vmm_unmap_page(uint64_t virt)
{
    pte_t *entry = walk(virt, false);
    if (entry)
        *entry = 0;

    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}
