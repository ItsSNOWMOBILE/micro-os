; user_entry.asm — Jump to user mode via IRETQ.
;
; void jump_to_user(uint64_t entry, uint64_t user_rsp,
;                   uint16_t user_cs, uint16_t user_ds);
;
; MS x64 ABI: RCX=entry, RDX=user_rsp, R8=user_cs, R9=user_ds
;
; Builds an IRETQ frame on the stack:
;   SS, RSP, RFLAGS, CS, RIP
; Then executes IRETQ to drop to Ring 3.

bits 64
section .text

global jump_to_user

jump_to_user:
    ; Load user data segment into DS, ES, FS, GS.
    mov  ax, r9w
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    ; Build IRETQ frame.
    push r9             ; SS (user data selector with RPL 3)
    push rdx            ; RSP (user stack pointer)
    pushfq              ; RFLAGS
    ; Ensure IF is set in the pushed RFLAGS.
    or   qword [rsp], 0x200
    push r8             ; CS (user code selector with RPL 3)
    push rcx            ; RIP (user entry point)

    iretq
