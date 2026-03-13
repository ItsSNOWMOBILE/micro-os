# micro-os

A 64-bit operating system kernel written from scratch, booted via UEFI.

## Features

- **UEFI bootloader** — obtains framebuffer (GOP), memory map, and exits boot services
- **Framebuffer console** — pixel-level text rendering with an embedded 8x16 VGA font
- **Serial output** — COM1 at 115200 baud for debug logging (QEMU `-serial stdio`)
- **GDT/IDT** — full 64-bit descriptor tables with 256 interrupt vectors
- **PIC remapping** — IRQ 0–15 mapped to vectors 32–47
- **Physical memory manager** — bitmap allocator using the UEFI memory map
- **Virtual memory manager** — 4-level paging, 4 GiB identity map via 2 MiB huge pages
- **Kernel heap** — first-fit free-list allocator (`kmalloc` / `kfree`)
- **PIT timer** — 100 Hz tick for preemptive scheduling
- **PS/2 keyboard driver** — scan code set 1, US layout, ring buffer input
- **Preemptive scheduler** — round-robin with sleep support and context switching
- **Interactive shell** — built-in commands: `help`, `mem`, `uptime`, `clear`, `reboot`

## TODO

- Add filesystem
- Add userspace, privilege separation, syscall interface, user-mode tasks...
- Memory management upgrade
- Scheduler rewrite/upgrade
- Error handling and fault recovery
- Allow abstraction layer for hardware
- Add cross-compilation support
- Add tests! Yikes

## Building

### Requirements

[MSYS2](https://www.msys2.org/) MinGW64 environment with:

```
pacman -S nasm mingw-w64-x86_64-gcc mingw-w64-x86_64-qemu
```

Python 3 is needed for the disk image tool.

### Build

```
make
```

### Run in QEMU

```
make run
```

This launches QEMU with UEFI firmware (OVMF), a serial console on stdio, and the micro-os disk image.

### Clean

```
make clean
```

## Architecture

```
boot/
  main.c           UEFI bootloader (PE32+ application)
  uefi.h           Minimal UEFI type definitions
  bootinfo.h       Data structure passed to the kernel

kernel/
  entry.asm         Entry stub (_start → kernel_main)
  main.c            Kernel entry point and init sequence
  console.c         Framebuffer text console (8x16 font)
  serial.c          COM1 debug output
  string.c          memset, memcpy, memmove, strlen

  interrupts/
    gdt.c           Global Descriptor Table + TSS
    idt.c           Interrupt Descriptor Table + PIC
    isr_stubs.asm   256 ISR entry stubs
    gdt_flush.asm   GDT reload helper

  mm/
    pmm.c           Physical memory manager (bitmap)
    vmm.c           Virtual memory manager (4-level paging)
    heap.c          Kernel heap (kmalloc / kfree)

  drivers/
    timer.c         PIT channel 0 (100 Hz)
    keyboard.c      PS/2 keyboard (scan code set 1)

  sched/
    task.c          Task management and round-robin scheduler
    switch.asm      Context switch (register save/restore)

tools/
  mkimg.py          Creates a bootable FAT12 UEFI disk image
```

## Boot sequence

1. UEFI firmware loads `BOOTX64.EFI` from the ESP
2. Bootloader initialises GOP, gets memory map, copies kernel to 0x100000
3. Bootloader exits boot services and jumps to `_start`
4. `_start` zeroes BSS and calls `kernel_main`
5. Kernel initialises serial, console, GDT, IDT, PMM, VMM, heap
6. Timer and keyboard drivers are started
7. Scheduler creates demo tasks and an interactive shell
8. Idle loop yields to the scheduler indefinitely

## Memory layout

| Region | Address | Description |
|--------|---------|-------------|
| Low 1 MiB | `0x00000` – `0xFFFFF` | Reserved (BIOS, IVT) |
| Kernel | `0x100000` + | Code, rodata, data, BSS |
| PMM bitmap | After kernel | Physical page frame bitmap |
| Heap | PMM-allocated | Dynamic kernel allocations |
| Framebuffer | `0x80000000` (typical) | GOP linear framebuffer |

## License

MIT
