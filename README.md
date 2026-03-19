# micro-os

A 64-bit operating system kernel written from scratch in C and x86-64 assembly. Boots via UEFI, runs on real hardware and QEMU. No external runtime dependencies — every line from bootloader to shell is handwritten.

## Features

### Core
- **UEFI bootloader** — initialises GOP framebuffer, reads memory map, loads kernel to 0x100000
- **GDT/IDT** — 64-bit descriptor tables, 256 interrupt vectors, IST-based stack switching
- **PIC** — IRQ 0-15 remapped to vectors 32-47
- **Exception handling** — dedicated handlers for all CPU faults (page fault, GPF, divide-by-zero, invalid opcode, stack fault, etc.) with register dumps; user-mode faults kill the offending task instead of panicking
- **System calls** — INT 0x80 dispatcher with 14 syscalls: exit, read, write, open, close, stat, yield, sleep, getpid, getppid, waitpid, kill, sigreturn
- **Hardware abstraction layer** — ops-struct interfaces for input, pointer, timer, and serial devices; drivers register at init, kernel calls through the HAL
- **ELF loader** — parses ELF64 executables from the VFS, maps PT_LOAD segments into per-process address spaces

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
- **User-mode tasks** — Ring 3 tasks with separate user stacks, IRETQ entry, and INT 0x80 syscalls; TSS RSP0 updated on context switch for proper privilege transitions
- **Per-process address spaces** — each user task gets its own PML4, CR3 switched on context switch; kernel mappings shared, user pages isolated
- **Signals** — POSIX-style signal infrastructure with pending bitmask, per-task handler table, SIGKILL/SIGSTOP special handling

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
| `test` | Run kernel self-tests |
| `usertest` | Spawn a Ring 3 demo task |
| `kill <id>` | Send SIGKILL to a task |
| `exec <path>` | Load and run an ELF binary from the VFS |
| `reboot` | Reboot the machine |

**Line editing:** left/right arrows, Home, End, Delete, backspace, Ctrl+L (clear screen).

**Tab completion:** single match completes fully; multiple matches complete the common prefix on first Tab, list all on second Tab. Works for both commands and file paths relative to the current directory.

## TODO

- PCI bus enumeration
- USB host controller driver (xHCI)
- Networking stack (virtio-net or e1000)
- On-disk filesystem (FAT32 or ext2)

## Building

### Requirements

The build system auto-detects the host platform.

**Windows (MSYS2 MinGW64):**
```
pacman -S nasm mingw-w64-x86_64-gcc mingw-w64-x86_64-qemu python3
```

**Linux (Debian/Ubuntu):**
```
apt install nasm gcc-mingw-w64-x86-64 qemu-system-x86 ovmf python3
```

**macOS (Homebrew):**
```
brew install nasm mingw-w64 qemu python3
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
  syscall.c/h          System call dispatcher (INT 0x80)
  syscall_entry.asm    SYSCALL instruction entry point
  user.c/h             User-mode task creation (Ring 3 via IRETQ)
  user_entry.asm       IRETQ trampoline to enter user mode
  elf.c/h              ELF64 binary loader
  test.c/h             Kernel self-test framework

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

  hal/
    hal.c/h            Hardware abstraction layer (ops-struct interfaces)

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
| User stacks | `0x7FFFFFFFE000` ↓ | Per-task Ring 3 stacks (16 KiB each) |

## License

MIT
