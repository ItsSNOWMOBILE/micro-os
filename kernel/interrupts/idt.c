/*
 * idt.c — Interrupt Descriptor Table setup.
 *
 * Installs 256 IDT entries.  The first 32 are CPU exceptions; vectors
 * 32–47 are remapped PIC IRQs; vector 0x80 is reserved for syscalls.
 * Each entry points to an assembly stub (isr_stub_N) that pushes the
 * vector number and a uniform error code, then calls isr_dispatch.
 */

#include "idt.h"
#include "gdt.h"
#include "../string.h"
#include "../console.h"
#include "../kernel.h"

/* ── IDT gate descriptor ─────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t  ist;           /* bits [2:0] = IST index, rest zero */
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
} IDTEntry;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} IDTR;

static IDTEntry idt[IDT_ENTRIES];
static isr_handler_t handlers[IDT_ENTRIES];

/* ── Assembly stubs (defined in isr_stubs.asm) ───────────────────────────── */

extern void *isr_stub_table[IDT_ENTRIES];

/* ── Gate setup ──────────────────────────────────────────────────────────── */

static void
set_gate(int vector, uint64_t handler, uint8_t ist, uint8_t type_attr)
{
    idt[vector].offset_lo  = handler & 0xFFFF;
    idt[vector].selector   = GDT_KERNEL_CODE;
    idt[vector].ist        = ist & 0x7;
    idt[vector].type_attr  = type_attr;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].reserved   = 0;
}

/* ── PIC remapping ───────────────────────────────────────────────────────── */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static void
pic_remap(void)
{
    /* ICW1: begin init, expect ICW4 */
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();
    /* ICW2: vector offsets */
    outb(PIC1_DATA, 0x20); io_wait();  /* IRQ 0–7  → vectors 32–39 */
    outb(PIC2_DATA, 0x28); io_wait();  /* IRQ 8–15 → vectors 40–47 */
    /* ICW3: wiring */
    outb(PIC1_DATA, 0x04); io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    /* Mask all IRQs initially; drivers unmask what they need. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

static const char *exception_names[32] = {
    "Division by Zero",        "Debug",
    "NMI",                     "Breakpoint",
    "Overflow",                "Bound Range Exceeded",
    "Invalid Opcode",          "Device Not Available",
    "Double Fault",            "Coprocessor Segment Overrun",
    "Invalid TSS",             "Segment Not Present",
    "Stack-Segment Fault",     "General Protection Fault",
    "Page Fault",              "Reserved",
    "x87 FP Exception",       "Alignment Check",
    "Machine Check",           "SIMD FP Exception",
    "Virtualisation",          "Control Protection",
    "Reserved",                "Reserved",
    "Reserved",                "Reserved",
    "Reserved",                "Reserved",
    "Hypervisor Injection",    "VMM Communication",
    "Security Exception",      "Reserved",
};

void
isr_dispatch(InterruptFrame *frame)
{
    uint64_t vector = frame->int_no;

    if (vector < IDT_ENTRIES && handlers[vector]) {
        handlers[vector](frame);
        return;
    }

    if (vector < 32) {
        kprintf("\n*** EXCEPTION %u: %s ***\n", (unsigned)vector,
                exception_names[vector]);
        kprintf("  error_code = 0x%lx\n", frame->error_code);
        kprintf("  RIP = 0x%lx  RSP = 0x%lx\n", frame->rip, frame->rsp);
        kprintf("  CS  = 0x%lx  SS  = 0x%lx\n", frame->cs, frame->ss);
        kprintf("  RAX = 0x%lx  RBX = 0x%lx\n", frame->rax, frame->rbx);
        kprintf("  RCX = 0x%lx  RDX = 0x%lx\n", frame->rcx, frame->rdx);
        panic("unhandled exception");
    }

    /* IRQ: send EOI to PIC. */
    if (vector >= 40)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void
idt_register_handler(uint8_t vector, isr_handler_t handler)
{
    handlers[vector] = handler;
}

void
idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    memset(handlers, 0, sizeof(handlers));

    pic_remap();

    /* 0x8E = present, DPL 0, interrupt gate (64-bit). */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        uint8_t ist = 0;
        if (i == 2 || i == 8)   /* NMI and Double Fault use IST1 */
            ist = 1;
        set_gate(i, (uint64_t)isr_stub_table[i], ist, 0x8E);
    }

    /* Syscall gate: DPL 3 so user-mode can trigger it via INT 0x80. */
    set_gate(0x80, (uint64_t)isr_stub_table[0x80], 0, 0xEE);

    IDTR idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt
    };

    __asm__ volatile("lidt %0" : : "m"(idtr));
}
