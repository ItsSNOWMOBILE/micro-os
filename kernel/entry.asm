; entry.asm — Kernel entry stub.
;
; This must be the first object linked so it sits at the very start of
; the flat binary (address 0x100000).  The UEFI bootloader jumps here
; with RCX = BootInfo* (MS ABI).

bits 64
section .text

extern kernel_main

global _start
_start:
    ; RCX holds BootInfo* from the UEFI bootloader (MS ABI).
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
