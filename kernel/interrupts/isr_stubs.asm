; isr_stubs.asm — Interrupt service routine entry stubs.
;
; Each stub pushes a dummy error code (if the CPU didn't push one),
; then the vector number, saves all general-purpose registers, calls
; the C dispatcher, restores state, and returns via iretq.
;
; The stubs populate an array (isr_stub_table) that idt.c indexes by
; vector number when building the IDT.

bits 64
section .text

extern isr_dispatch

; Macro for exceptions that do NOT push an error code.
%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0               ; dummy error code
    push qword %1               ; vector number
    jmp  isr_common
%endmacro

; Macro for exceptions that push an error code (8, 10–14, 17, 21, 29, 30).
%macro ISR_ERR 1
isr_stub_%1:
    push qword %1               ; vector number (error code already on stack)
    jmp  isr_common
%endmacro

; ── Common stub: save state, call C, restore, iretq ──────────────────────

isr_common:
    ; Save general-purpose registers (matches InterruptFrame layout).
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; First argument = pointer to InterruptFrame (top of stack).
    ; MS x64 ABI: first arg in RCX.
    mov  rcx, rsp
    ; MS ABI requires 32 bytes of shadow space on the stack.
    sub  rsp, 32
    call isr_dispatch
    add  rsp, 32

    ; Restore registers.
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rsi
    pop  rdi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax

    ; Remove vector number and error code.
    add  rsp, 16
    iretq

; ── Generate stubs for all 256 vectors ───────────────────────────────────

; Exceptions 0–31:
ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8       ; Double Fault
ISR_NOERR  9
ISR_ERR   10       ; Invalid TSS
ISR_ERR   11       ; Segment Not Present
ISR_ERR   12       ; Stack-Segment Fault
ISR_ERR   13       ; General Protection Fault
ISR_ERR   14       ; Page Fault
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17       ; Alignment Check
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21       ; Control Protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29       ; VMM Communication
ISR_ERR   30       ; Security Exception
ISR_NOERR 31

; IRQs and software interrupts 32–255:
%assign i 32
%rep 224
    ISR_NOERR i
%assign i i+1
%endrep

; ── Stub table: array of 256 function pointers ───────────────────────────

section .data

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
