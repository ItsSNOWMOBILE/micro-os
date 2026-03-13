#
# Makefile — micro-os build system.
#
# Targets:
#   make         — build kernel + bootloader, create disk image
#   make run     — build and launch in QEMU
#   make clean   — remove build artefacts
#
# Requirements (MSYS2 MinGW64):
#   pacman -S nasm mingw-w64-x86_64-gcc mingw-w64-x86_64-qemu
#

# ── Toolchain ────────────────────────────────────────────────────────────────

# Use MSYS2 MinGW64 toolchain.
MSYS2    = /c/msys64/mingw64/bin
CC       = $(MSYS2)/gcc
LD       = $(MSYS2)/ld
NASM     = /c/msys64/usr/bin/nasm
OBJCOPY  = $(MSYS2)/objcopy
QEMU     = $(MSYS2)/qemu-system-x86_64

export PATH := $(MSYS2):/c/msys64/usr/bin:$(PATH)

# ── Directories ──────────────────────────────────────────────────────────────

BUILD    = build
BOOT_DIR = boot
KERN_DIR = kernel
INT_DIR  = $(KERN_DIR)/interrupts

# ── OVMF firmware path (MSYS2) ──────────────────────────────────────────────

OVMF     = /c/msys64/mingw64/share/qemu/edk2-x86_64-code.fd

# ── Common flags ─────────────────────────────────────────────────────────────

CFLAGS_COMMON = -Wall -Wextra -Werror -std=c11 \
                -ffreestanding -fno-stack-protector -fno-exceptions \
                -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
                -fno-asynchronous-unwind-tables

# Kernel: freestanding flat binary linked at 0x100000.
KERN_CFLAGS  = $(CFLAGS_COMMON) -mcmodel=large -fno-pic -O2
KERN_LDFLAGS = -T $(KERN_DIR)/linker.ld -nostdlib -static --image-base 0

# Boot: UEFI PE32+ application (MinGW targets PE natively).
BOOT_CFLAGS  = $(CFLAGS_COMMON) -O2 -I$(BOOT_DIR) \
               -Wno-incompatible-pointer-types

# ── Sources ──────────────────────────────────────────────────────────────────

MM_DIR    = $(KERN_DIR)/mm
DRV_DIR   = $(KERN_DIR)/drivers
SCHED_DIR = $(KERN_DIR)/sched
FS_DIR    = $(KERN_DIR)/fs

KERN_C_SRC = $(KERN_DIR)/main.c \
             $(KERN_DIR)/console.c \
             $(KERN_DIR)/serial.c \
             $(KERN_DIR)/string.c \
             $(KERN_DIR)/sync.c \
             $(KERN_DIR)/syscall.c \
             $(KERN_DIR)/user.c \
             $(INT_DIR)/gdt.c \
             $(INT_DIR)/idt.c \
             $(MM_DIR)/pmm.c \
             $(MM_DIR)/vmm.c \
             $(MM_DIR)/heap.c \
             $(DRV_DIR)/timer.c \
             $(DRV_DIR)/keyboard.c \
             $(DRV_DIR)/mouse.c \
             $(SCHED_DIR)/task.c \
             $(FS_DIR)/vfs.c

KERN_ASM_SRC = $(KERN_DIR)/entry.asm \
               $(KERN_DIR)/syscall_entry.asm \
               $(KERN_DIR)/user_entry.asm \
               $(INT_DIR)/gdt_flush.asm \
               $(INT_DIR)/isr_stubs.asm \
               $(SCHED_DIR)/switch.asm

BOOT_C_SRC = $(BOOT_DIR)/main.c

# ── Objects ──────────────────────────────────────────────────────────────────

KERN_C_OBJ   = $(patsubst %.c,$(BUILD)/%.o,$(KERN_C_SRC))
KERN_ASM_OBJ = $(patsubst %.asm,$(BUILD)/%.o,$(KERN_ASM_SRC))
# entry.o must be first so _start is at the beginning of the flat binary.
KERN_ENTRY   = $(BUILD)/$(KERN_DIR)/entry.o
KERN_OBJ     = $(KERN_ENTRY) $(KERN_C_OBJ) $(filter-out $(KERN_ENTRY),$(KERN_ASM_OBJ))

BOOT_OBJ     = $(patsubst %.c,$(BUILD)/%.o,$(BOOT_C_SRC))

# ── Top-level targets ───────────────────────────────────────────────────────

.PHONY: all run clean

all: $(BUILD)/micro-os.img

run: $(BUILD)/micro-os.img
	$(QEMU) \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
		-drive format=raw,file=$(BUILD)/micro-os.img \
		-m 256M \
		-serial stdio \
		-display gtk \
		-usb

clean:
	rm -rf $(BUILD)

# ── Kernel ───────────────────────────────────────────────────────────────────

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f win64 $< -o $@

$(BUILD)/kernel/interrupts/%.o: kernel/interrupts/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f win64 $< -o $@

$(BUILD)/kernel/sched/%.o: kernel/sched/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f win64 $< -o $@

$(BUILD)/kernel/fs/%.o: kernel/fs/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERN_CFLAGS) -c $< -o $@

$(BUILD)/kernel.exe: $(KERN_OBJ)
	$(LD) $(KERN_LDFLAGS) -o $@ $^

$(BUILD)/kernel.bin: $(BUILD)/kernel.exe
	$(OBJCOPY) -O binary $< $@

# ── Bootloader ───────────────────────────────────────────────────────────────

# Embed kernel.bin into the bootloader via NASM incbin.
$(BUILD)/kernel_blob.o: $(BUILD)/kernel.bin boot/kernel_blob.asm
	@mkdir -p $(dir $@)
	$(NASM) -f win64 boot/kernel_blob.asm -o $@

$(BUILD)/boot/%.o: boot/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_CFLAGS) -c $< -o $@

$(BUILD)/BOOTX64.EFI: $(BOOT_OBJ) $(BUILD)/kernel_blob.o
	$(CC) $(BOOT_CFLAGS) -nostdlib -Wl,-dll -shared \
		-Wl,--subsystem,10 \
		-e efi_main \
		-o $@ $^

# ── Disk image ───────────────────────────────────────────────────────────────

$(BUILD)/micro-os.img: $(BUILD)/BOOTX64.EFI
	python3 tools/mkimg.py $(BUILD)/BOOTX64.EFI $@

# ── Dependencies ─────────────────────────────────────────────────────────────

$(BOOT_OBJ): $(BUILD)/kernel.bin
