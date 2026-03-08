/*
 * linker.ld — Kernel linker script.
 *
 * The kernel is loaded as a flat binary at physical address 0x100000
 * (1 MiB).  kernel_main is the entry point.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text ALIGN(4K) : {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    .bss ALIGN(4K) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
        *(.pdata)
        *(.xdata)
    }
}
