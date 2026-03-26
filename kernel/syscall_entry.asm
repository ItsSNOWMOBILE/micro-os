; syscall_entry.asm — SYSCALL instruction entry point.
;
; When SYSCALL executes:
;   RCX = user RIP (return address)
;   R11 = user RFLAGS
;   RAX = syscall number
;   RDI = arg0, RSI = arg1, RDX = arg2, R10 = arg3
;
; We switch to kernel stack, call the C dispatcher, then SYSRETQ.

bits 64

section .data
global _syscall_kernel_rsp
_syscall_kernel_rsp: dq 0

section .bss
_user_rsp: resq 1

section .text

extern syscall_dispatch

global syscall_entry

syscall_entry:
    ; Disable interrupts immediately to prevent preemption while
    ; the global _user_rsp is in use.  Re-enabled after we push
    ; it onto the per-task kernel stack.
    cli
    mov  [rel _user_rsp], rsp
    mov  rsp, [rel _syscall_kernel_rsp]

    ; Build a save frame on the kernel stack.
    push qword [rel _user_rsp]  ; user RSP
    push r11                     ; user RFLAGS
    push rcx                     ; user RIP

    ; Save callee-saved.
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Call syscall_dispatch(num, a0, a1, a2, a3)
    ; MS x64 ABI: RCX, RDX, R8, R9, [rsp+32]
    ; Syscall ABI: RAX=num, RDI=a0, RSI=a1, RDX=a2, R10=a3
    sub  rsp, 40                ; shadow space + 5th arg
    mov  [rsp+32], r10          ; a3 on stack
    mov  r9, rdx                ; a2 -> R9
    mov  r8, rsi                ; a1 -> R8
    mov  rdx, rdi               ; a0 -> RDX
    mov  rcx, rax               ; num -> RCX
    call syscall_dispatch
    add  rsp, 40

    ; RAX has return value.

    ; Restore callee-saved.
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx

    ; Restore user state.
    pop  rcx                    ; user RIP
    pop  r11                    ; user RFLAGS
    pop  rsp                    ; user RSP

    o64 sysret

; Helper: set kernel RSP for SYSCALL handler.
; void syscall_set_kernel_rsp(uint64_t rsp);  [MS ABI: RCX = rsp]
global syscall_set_kernel_rsp
syscall_set_kernel_rsp:
    mov [rel _syscall_kernel_rsp], rcx
    ret
