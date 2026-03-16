# micro-os

A 64-bit operating system kernel written from scratch in C and x86-64 assembly. Boots via UEFI, runs on real hardware and QEMU. No external runtime dependencies — every line from bootloader to shell is handwritten.

## Features

### Core
- **UEFI bootloader** — initialises GOP framebuffer, reads memory map, loads kernel to 0x100000
- **GDT/IDT** — 64-bit descriptor tables, 256 interrupt vectors, IST-based stack switching
- **PIC** — IRQ 0-15 remapped to vectors 32-47

### Memory management
- **Physical memory manager** — bitmap allocator with bulk 64-bit operations, initialised from the UEFI memory map
- **Virtual memory manager** — 4-level paging with 4 GiB identity map via 2 MiB huge pages
- **Kernel heap** — first-fit free-list allocator (`kmalloc`/`kfree`)

### Drivers
- **PIT timer** — 100 Hz tick, monotonic counter, preemptive scheduling trigger
- **PS/2 keyboard** — scan code set 1, full US layout, ring buffer, Shift/Caps/Ctrl/Alt
- **PS/2 mouse** — IRQ 12 movement and button tracking
- **Serial** — COM1 at 115200 baud for debug logging

### Scheduling
- **Preemptive multitasking** — priority-based round-robin with 4 priority levels
- **Task lifecycle** — create, sleep, wait, exit, cleanup
- **Context switching** — callee-saved register save/restore via assembly

### Filesystem
- **In-memory VFS (ramfs)** — tree-structured vnode table with directories, files up to 16 KiB
- **Operations** — open, read, write, close, stat, mkdir, readdir, unlink, rename

### Shell
Interactive shell with bash-style line editing and tab completion:

| Command | Description |
|---------|-------------|
| `cd [dir]` | Change working directory |
| `pwd` | Print working directory |
| `ls [path]` | List directory contents |
| `cat <file>` | Print file contents |
| `head [-n N] <file>` | Print first N lines (default 10) |
| `tail [-n N] <file>` | Print last N lines (default 10) |
| `wc <file>` | Count lines, words, bytes |
| `echo [text]` | Print text to console |
| `touch <file>` | Create empty file |
| `write <file> <text>` | Write text to file |
| `cp <src> <dst>` | Copy file |
| `mv <src> <dst>` | Move or rename file |
| `mkdir <dir>` | Create directory |
| `rmdir <dir>` | Remove empty directory |
| `rm <path>` | Remove file |
| `stat <path>` | Show file/directory info |
| `mem` | Show free memory |
| `uptime` | Show system uptime |
| `ps` | List running tasks |
| `whoami` | Print current user |
| `uname [-a]` | Print system information |
| `clear` | Clear the screen |
| `reboot` | Reboot the machine |

**Line editing:** left/right arrows, Home, End, Delete, backspace, Ctrl+L (clear screen).

**Tab completion:** single match completes fully; multiple matches complete the common prefix on first Tab, list all on second Tab. Works for both commands and file paths relative to the current directory.

## TODO

- Add userspace, privilege separation, user-mode tasks
- Error handling and fault recovery
- Hardware abstraction layer
- Cross-compilation support (Linux/macOS host)
- Tests

## Building

### Requirements

- [MSYS2](https://www.msys2.org/) with the MinGW64 toolchain
- Python 3 (for disk image generation)

```
pacman -S nasm mingw-w64-x86_64-gcc mingw-w64-x86_64-qemu python3
```

### Build and run

```bash
make          # build kernel and bootable disk image
make run      # launch in QEMU with UEFI firmware and serial console
make clean    # remove build artifacts
```

## Project structure

```
boot/
  main.c              UEFI bootloader (PE32+ application)
  uefi.h              Minimal UEFI type definitions
  bootinfo.h          Boot handoff structure (framebuffer, memory map)

kernel/
  entry.asm            Entry stub (_start -> kernel_main)
  main.c               Init sequence, shell, command interpreter
  console.c/h          Framebuffer text console (8x16 VGA font)
  serial.c/h           COM1 debug output
  string.c/h           Freestanding string/memory utilities
  kernel.h             Port I/O, cli/sti/hlt macros
  sync.c/h             Spinlock primitives
  syscall.c/h          System call infrastructure
  user.c/h             User-mode support (planned)

  interrupts/
    gdt.c/h            Global Descriptor Table + TSS
    idt.c/h            Interrupt Descriptor Table + PIC setup
    isr_stubs.asm      256 ISR entry stubs (push errno, vector, jump to common)
    gdt_flush.asm      GDT reload helper

  mm/
    pmm.c/h            Physical memory manager (bitmap allocator)
    vmm.c/h            Virtual memory manager (4-level paging)
    heap.c/h           Kernel heap (first-fit free-list)

  drivers/
    timer.c/h          PIT channel 0 (100 Hz)
    keyboard.c/h       PS/2 keyboard (scan code set 1, US layout)
    mouse.c/h          PS/2 mouse (IRQ 12)

  sched/
    task.c/h           Task management, priority scheduler
    switch.asm         Context switch (callee-saved registers)

  fs/
    vfs.c/h            In-memory virtual filesystem (ramfs)

tools/
  mkimg.py             Creates a bootable FAT12 UEFI disk image
```

## Boot sequence

1. UEFI firmware loads `BOOTX64.EFI` from the EFI System Partition
2. Bootloader initialises GOP, reads memory map, copies kernel to `0x100000`
3. Bootloader calls `ExitBootServices` and jumps to `_start`
4. `_start` zeroes BSS, calls `kernel_main`
5. Kernel initialises: serial, console, GDT, IDT, PMM, VMM, heap, PIT, keyboard, mouse, VFS, syscalls, scheduler
6. Scheduler launches demo tasks and the interactive shell
7. Idle loop yields to the scheduler indefinitely

## Memory layout

| Region | Address | Description |
|--------|---------|-------------|
| Low 1 MiB | `0x00000`-`0xFFFFF` | Reserved (BIOS/UEFI, IVT) |
| Kernel | `0x100000`+ | Code, rodata, data, BSS |
| PMM bitmap | After kernel | Physical page frame bitmap |
| Heap | PMM-allocated pages | Dynamic kernel allocations |
| Framebuffer | `0x80000000` (typical) | GOP linear framebuffer |

## License

MIT
