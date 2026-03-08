; gdt_flush.asm — Load new GDT and reload segment registers.
;
; Calling convention (MS x64 ABI — MinGW default):
;   rcx = pointer to GDTR
;   rdx = kernel code selector (e.g. 0x08)
;   r8  = kernel data selector (e.g. 0x10)

bits 64
section .text

global gdt_flush

gdt_flush:
    lgdt [rcx]

    ; Reload data segments.
    mov  ax, r8w
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    ; Far return to reload CS.  Push the new CS and the return address,
    ; then retfq pops both.
    pop  rax            ; return address
    push rdx            ; new CS
    push rax            ; return address
    retfq
