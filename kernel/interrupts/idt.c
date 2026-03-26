/*
 * idt.c — Interrupt Descriptor Table setup.
 *
 * Installs 256 IDT entries.  The first 32 are CPU exceptions; vectors
 * 32-47 are remapped PIC IRQs; vector 0x80 is reserved for syscalls.
 * Each entry points to an assembly stub (isr_stub_N) that pushes the
 * vector number and a uniform error code, then calls isr_dispatch.
 */

#include "idt.h"
#include "gdt.h"
#include "../string.h"
#include "../console.h"
#include "../kernel.h"
#include "../serial.h"
#include "../sched/task.h"

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
    outb(PIC1_CMD,  0x11); io_wait();
    outb(PIC2_CMD,  0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* ── Exception handlers ──────────────────────────────────────────────────── */

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

static void
dump_registers(InterruptFrame *frame)
{
    kprintf("  RIP = 0x%lx  RSP = 0x%lx\n", frame->rip, frame->rsp);
    kprintf("  CS  = 0x%lx  SS  = 0x%lx\n", frame->cs, frame->ss);
    kprintf("  RAX = 0x%lx  RBX = 0x%lx\n", frame->rax, frame->rbx);
    kprintf("  RCX = 0x%lx  RDX = 0x%lx\n", frame->rcx, frame->rdx);
    kprintf("  RSI = 0x%lx  RDI = 0x%lx\n", frame->rsi, frame->rdi);
    kprintf("  RBP = 0x%lx  R8  = 0x%lx\n", frame->rbp, frame->r8);
    kprintf("  R9  = 0x%lx  R10 = 0x%lx\n", frame->r9,  frame->r10);
    kprintf("  R11 = 0x%lx  R12 = 0x%lx\n", frame->r11, frame->r12);
    kprintf("  R13 = 0x%lx  R14 = 0x%lx\n", frame->r13, frame->r14);
    kprintf("  R15 = 0x%lx  RFLAGS = 0x%lx\n", frame->r15, frame->rflags);
    kprintf("  Error code = 0x%lx\n", frame->error_code);
}

static void
page_fault_handler(InterruptFrame *frame)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    kprintf("\n*** PAGE FAULT ***\n");
    kprintf("  Faulting address: 0x%lx\n", cr2);
    kprintf("  Error: %s %s %s%s\n",
            (frame->error_code & 1) ? "protection-violation" : "not-present",
            (frame->error_code & 2) ? "write" : "read",
            (frame->error_code & 4) ? "user" : "kernel",
            (frame->error_code & 16) ? " instruction-fetch" : "");
    dump_registers(frame);

    /* If the fault is from a user-mode task (CS RPL == 3), kill the task. */
    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("kernel page fault");
}

static void
gpf_handler(InterruptFrame *frame)
{
    kprintf("\n*** GENERAL PROTECTION FAULT ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("kernel GPF");
}

static void
double_fault_handler(InterruptFrame *frame)
{
    kprintf("\n*** DOUBLE FAULT ***\n");
    dump_registers(frame);
    panic("double fault");
}

static void
invalid_opcode_handler(InterruptFrame *frame)
{
    kprintf("\n*** INVALID OPCODE ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("invalid opcode");
}

static void
division_handler(InterruptFrame *frame)
{
    kprintf("\n*** DIVISION BY ZERO ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("division by zero");
}

static void
breakpoint_handler(InterruptFrame *frame)
{
    if (frame->cs & 3) {
        /* User-mode breakpoint: dump and kill task. */
        kprintf("\n*** BREAKPOINT ***\n");
        dump_registers(frame);
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    /* Kernel breakpoints: silently continue.  PCI config-space probes
     * on some UEFI firmware trigger INT 3 in firmware code; these are
     * harmless and must not flood the console. */
}

static void
overflow_handler(InterruptFrame *frame)
{
    kprintf("\n*** OVERFLOW ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("overflow exception");
}

static void
bound_range_handler(InterruptFrame *frame)
{
    kprintf("\n*** BOUND RANGE EXCEEDED ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("bound range exceeded");
}

static void
invalid_tss_handler(InterruptFrame *frame)
{
    kprintf("\n*** INVALID TSS ***\n");
    dump_registers(frame);
    panic("invalid TSS");
}

static void
segment_np_handler(InterruptFrame *frame)
{
    kprintf("\n*** SEGMENT NOT PRESENT ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("segment not present");
}

static void
stack_fault_handler(InterruptFrame *frame)
{
    kprintf("\n*** STACK-SEGMENT FAULT ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("stack-segment fault");
}

static void
alignment_check_handler(InterruptFrame *frame)
{
    kprintf("\n*** ALIGNMENT CHECK ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("alignment check");
}

static void
simd_handler(InterruptFrame *frame)
{
    kprintf("\n*** SIMD FLOATING-POINT EXCEPTION ***\n");
    dump_registers(frame);

    if (frame->cs & 3) {
        kprintf("  Killing task: %s\n", sched_current()->name);
        sched_current()->state = TASK_DEAD;
        sched_yield();
        return;
    }

    panic("SIMD floating-point exception");
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

void
isr_dispatch(InterruptFrame *frame)
{
    uint64_t vector = frame->int_no;

    if (vector < IDT_ENTRIES && handlers[vector]) {
        handlers[vector](frame);
        /* Send EOI for hardware IRQs (vectors 32-47) centrally,
         * so individual handlers don't need to remember. */
        if (vector >= 32 && vector < 48) {
            if (vector >= 40)
                outb(PIC2_CMD, 0x20);
            outb(PIC1_CMD, 0x20);
        }
        return;
    }

    if (vector < 32) {
        kprintf("\n*** EXCEPTION %u: %s ***\n", (unsigned)vector,
                exception_names[vector]);
        dump_registers(frame);

        /* User-mode faults: kill the offending task. */
        if (frame->cs & 3) {
            kprintf("  Killing task: %s\n", sched_current()->name);
            sched_current()->state = TASK_DEAD;
            sched_yield();
            return;
        }

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
        if (i == 2 || i == 8)
            ist = 1;
        set_gate(i, (uint64_t)isr_stub_table[i], ist, 0x8E);
    }

    /* Syscall gate: DPL 3 so user-mode can trigger it via INT 0x80. */
    set_gate(0x80, (uint64_t)isr_stub_table[0x80], 0, 0xEE);

    /* Register built-in exception handlers. */
    handlers[0]  = division_handler;
    handlers[3]  = breakpoint_handler;
    handlers[4]  = overflow_handler;
    handlers[5]  = bound_range_handler;
    handlers[6]  = invalid_opcode_handler;
    handlers[8]  = double_fault_handler;
    handlers[10] = invalid_tss_handler;
    handlers[11] = segment_np_handler;
    handlers[12] = stack_fault_handler;
    handlers[13] = gpf_handler;
    handlers[14] = page_fault_handler;
    handlers[17] = alignment_check_handler;
    handlers[19] = simd_handler;

    IDTR idtr = {
        .limit = sizeof(idt) - 1,
        .base  = (uint64_t)idt
    };

    __asm__ volatile("lidt %0" : : "m"(idtr));
}
