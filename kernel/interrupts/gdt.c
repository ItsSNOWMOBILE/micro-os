/*
 * gdt.c — Global Descriptor Table setup.
 *
 * Defines a minimal GDT for 64-bit long mode:
 *   [0] Null descriptor
 *   [1] Kernel code (CS)  — Ring 0, 64-bit
 *   [2] Kernel data (DS)  — Ring 0
 *   [3] User code   (CS)  — Ring 3, 64-bit
 *   [4] User data   (DS)  — Ring 3
 *   [5] TSS descriptor    — 16 bytes (occupies slots 5 and 6)
 *
 * The TSS is needed so the CPU can switch to a known-good stack on
 * interrupts/exceptions (IST entries) and on Ring 3 → Ring 0 transitions.
 */

#include "gdt.h"
#include "../string.h"

/* ── TSS ─────────────────────────────────────────────────────────────────── */

#define IST_STACK_SIZE 4096

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3];          /* RSP for Ring 0, 1, 2 */
    uint64_t reserved1;
    uint64_t ist[7];          /* IST1–IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} TSS;

static TSS tss;
static uint8_t ist1_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

/* ── GDT entries ─────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_hi;   /* flags[7:4] | limit[19:16] */
    uint8_t  base_hi;
} GDTEntry;

typedef struct __attribute__((packed)) {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_hi;
    uint8_t  base_hi;
    uint32_t base_upper;
    uint32_t reserved;
} GDTSystemEntry;  /* 16-byte entry for TSS in long mode */

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} GDTR;

/*
 * 7 slots: null + 4 segment descriptors + 1 TSS (which uses 2 slots).
 * Total = 7 × 8 = 56 bytes.
 */
static uint8_t gdt_raw[7 * 8] __attribute__((aligned(16)));

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void
set_entry(int index, uint8_t access, uint8_t flags)
{
    GDTEntry *e = (GDTEntry *)&gdt_raw[index * 8];
    e->limit_lo      = 0xFFFF;
    e->base_lo       = 0;
    e->base_mid      = 0;
    e->access        = access;
    e->flags_limit_hi = (flags << 4) | 0x0F;
    e->base_hi       = 0;
}

static void
set_tss(int index, uint64_t base, uint32_t limit)
{
    GDTSystemEntry *e = (GDTSystemEntry *)&gdt_raw[index * 8];
    e->limit_lo      = limit & 0xFFFF;
    e->base_lo       = base & 0xFFFF;
    e->base_mid      = (base >> 16) & 0xFF;
    e->access        = 0x89;   /* present, TSS available */
    e->flags_limit_hi = ((limit >> 16) & 0x0F);
    e->base_hi       = (base >> 24) & 0xFF;
    e->base_upper    = (uint32_t)(base >> 32);
    e->reserved      = 0;
}

extern void gdt_flush(uint64_t gdtr_ptr, uint16_t code_sel, uint16_t data_sel);

/* ── Public API ──────────────────────────────────────────────────────────── */

void
gdt_init(void)
{
    memset(gdt_raw, 0, sizeof(gdt_raw));
    memset(&tss, 0, sizeof(tss));

    /* IST1: used for double-fault and NMI handlers. */
    tss.ist[0] = (uint64_t)(ist1_stack + IST_STACK_SIZE);
    tss.iomap_base = sizeof(TSS);

    /* [0] Null */
    /* [1] Kernel code: present, DPL 0, code, readable, 64-bit, granularity 4K */
    set_entry(1, 0x9A, 0xA);   /* 0x9A = P|DPL0|S|code|readable; 0xA = L|G */
    /* [2] Kernel data: present, DPL 0, data, writable */
    set_entry(2, 0x92, 0xC);   /* 0x92 = P|DPL0|S|data|writable; 0xC = Sz|G */
    /* [3] User code: present, DPL 3, code, readable, 64-bit */
    set_entry(3, 0xFA, 0xA);   /* 0xFA = P|DPL3|S|code|readable */
    /* [4] User data: present, DPL 3, data, writable */
    set_entry(4, 0xF2, 0xC);   /* 0xF2 = P|DPL3|S|data|writable */
    /* [5–6] TSS */
    set_tss(5, (uint64_t)&tss, sizeof(TSS) - 1);

    GDTR gdtr = {
        .limit = sizeof(gdt_raw) - 1,
        .base  = (uint64_t)gdt_raw
    };

    gdt_flush((uint64_t)&gdtr, GDT_KERNEL_CODE, GDT_KERNEL_DATA);
}
