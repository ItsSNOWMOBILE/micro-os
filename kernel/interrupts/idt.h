/*
 * idt.h — Interrupt Descriptor Table.
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Total entries: 32 exceptions + 16 IRQs + 1 syscall = 49; round to 256. */
#define IDT_ENTRIES 256

void idt_init(void);

/* Called by assembly stubs — dispatches to the correct handler. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rsi, rdi, rdx, rcx, rbx, rax;
    uint64_t int_no, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} InterruptFrame;

/* Register an ISR for a given vector number. */
typedef void (*isr_handler_t)(InterruptFrame *frame);
void idt_register_handler(uint8_t vector, isr_handler_t handler);

#endif /* IDT_H */
