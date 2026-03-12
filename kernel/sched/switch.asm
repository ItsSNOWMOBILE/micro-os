; switch.asm — Context switch between two tasks.
;
; void context_switch(TaskContext *old, TaskContext *new);
;
; MS x64 ABI: RCX = old context, RDX = new context.
;
; Saves callee-saved registers (rbx, rbp, r12–r15, rflags) and RSP into
; the old context, then restores them from the new context.  The "return"
; from this function lands at whatever RIP the new task had saved — either
; the point where it last called context_switch, or (for a new task) the
; entry function pushed onto its stack during task_create.
;
; TaskContext layout (offsets):
;   0x00  rsp
;   0x08  rip       (unused here — stack-based)
;   0x10  rbx
;   0x18  rbp
;   0x20  r12
;   0x28  r13
;   0x30  r14
;   0x38  r15
;   0x40  rflags

bits 64
section .text

global context_switch

context_switch:
    ; ── Save old context ─────────────────────────────────────────────
    ; Push callee-saved regs so they're on the stack when we return.
    pushfq
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save RSP into old->rsp (offset 0x00).
    mov [rcx], rsp

    ; ── Restore new context ──────────────────────────────────────────
    ; Load RSP from new->rsp (offset 0x00).
    mov rsp, [rdx]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq

    ; ret pops the return address — for a new task this is the entry function.
    ret
