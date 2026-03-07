; kernel_blob.asm — Embeds the kernel flat binary into the bootloader.
;
; Exports _binary_kernel_bin_start and _binary_kernel_bin_end so the
; bootloader C code can locate and copy the kernel to its load address.

bits 64
section .kdata

global _binary_kernel_bin_start
global _binary_kernel_bin_end

_binary_kernel_bin_start:
    incbin "build/kernel.bin"
_binary_kernel_bin_end:
