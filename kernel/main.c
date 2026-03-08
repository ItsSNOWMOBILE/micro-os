/*
 * main.c — Kernel entry point.
 *
 * Called by the UEFI bootloader after ExitBootServices.  At this point
 * we own the machine: no UEFI runtime, no interrupts, just a flat
 * physical address space and a framebuffer.
 */

#include "kernel.h"
#include "console.h"
#include "serial.h"
#include "string.h"
#include "interrupts/gdt.h"
#include "interrupts/idt.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "drivers/timer.h"
#include "drivers/keyboard.h"
#include "sched/task.h"

static const char *mmap_type_str[] = {
    [MMAP_USABLE]                  = "usable",
    [MMAP_RESERVED]                = "reserved",
    [MMAP_ACPI_RECLAIMABLE]        = "ACPI reclaimable",
    [MMAP_ACPI_NVS]                = "ACPI NVS",
    [MMAP_BAD_MEMORY]              = "bad",
    [MMAP_BOOTLOADER_RECLAIMABLE]  = "bootloader",
    [MMAP_KERNEL]                  = "kernel",
    [MMAP_FRAMEBUFFER]             = "framebuffer",
};

void
panic(const char *msg)
{
    kprintf("\n!!! KERNEL PANIC: %s !!!\n", msg);
    serial_write("\n!!! KERNEL PANIC: ");
    serial_write(msg);
    serial_write(" !!!\n");
    cli();
    for (;;)
        hlt();
}

/* ── Demo tasks ──────────────────────────────────────────────────────────── */

static void
task_a(void)
{
    for (int i = 0; i < 5; i++) {
        kprintf("[task A] iteration %d\n", i);
        sched_sleep(100);  /* ~1 second at 100 Hz */
    }
    kprintf("[task A] done\n");
}

static void
task_b(void)
{
    for (int i = 0; i < 5; i++) {
        kprintf("[task B] iteration %d\n", i);
        sched_sleep(150);
    }
    kprintf("[task B] done\n");
}

static void
shell_task(void)
{
    kprintf("\nmicro-os shell (type 'help' for commands)\n> ");

    char line[128];
    int pos = 0;

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n') {
            console_putchar('\n');
            line[pos] = '\0';

            if (pos == 0) {
                /* empty */
            } else if (memcmp(line, "help", 4) == 0) {
                kprintf("Commands:\n");
                kprintf("  help     show this message\n");
                kprintf("  mem      show memory stats\n");
                kprintf("  uptime   show ticks since boot\n");
                kprintf("  clear    clear screen\n");
                kprintf("  reboot   reboot the machine\n");
            } else if (memcmp(line, "mem", 3) == 0) {
                kprintf("Free pages: %lu (%lu MiB)\n",
                        pmm_free_pages(),
                        pmm_free_pages() * PAGE_SIZE / (1024 * 1024));
            } else if (memcmp(line, "uptime", 6) == 0) {
                uint64_t ticks = timer_ticks();
                kprintf("Uptime: %lu ticks (%lu seconds)\n",
                        ticks, ticks / 100);
            } else if (memcmp(line, "clear", 5) == 0) {
                console_clear();
            } else if (memcmp(line, "reboot", 6) == 0) {
                /* Triple fault: load a null IDT and trigger an interrupt. */
                kprintf("Rebooting...\n");
                struct { uint16_t limit; uint64_t base; } __attribute__((packed))
                    null_idt = { 0, 0 };
                __asm__ volatile("lidt %0" : : "m"(null_idt));
                __asm__ volatile("int $0x03");
            } else {
                kprintf("Unknown command: %s\n", line);
            }

            pos = 0;
            kprintf("> ");
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                kprintf("\b \b");
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = c;
            console_putchar(c);
        }
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

/* BSS boundaries from the linker script. */
extern uint8_t __bss_start[];
extern uint8_t __bss_end[];

static void
zero_bss(void)
{
    uint8_t *p = __bss_start;
    while (p < __bss_end)
        *p++ = 0;
}

void
kernel_main(BootInfo *info)
{
    /* Zero BSS — objcopy doesn't include it in the flat binary. */
    zero_bss();

    /* ── Serial ──────────────────────────────────────────────────────── */
    serial_init();
    serial_write("micro-os kernel booting...\n");

    /* ── Console ─────────────────────────────────────────────────────── */
    console_init(&info->fb);
    kprintf("micro-os v0.1\n");
    kprintf("Framebuffer: %ux%u @ 0x%lx\n",
            info->fb.width, info->fb.height, info->fb.base);

    /* ── Memory map ──────────────────────────────────────────────────── */
    uint64_t total_usable = 0;
    kprintf("\nMemory map (%u entries):\n", info->mmap_count);
    for (uint32_t i = 0; i < info->mmap_count; i++) {
        MmapEntry *e = &info->mmap[i];
        const char *type = (e->type <= MMAP_FRAMEBUFFER)
                            ? mmap_type_str[e->type] : "unknown";
        kprintf("  0x%lx - 0x%lx  %s\n",
                e->base, e->base + e->length, type);

        if (e->type == MMAP_USABLE)
            total_usable += e->length;
    }
    kprintf("Total usable: %lu MiB\n\n", total_usable / (1024 * 1024));

    /* ── GDT ─────────────────────────────────────────────────────────── */
    gdt_init();
    kprintf("[ok] GDT\n");

    /* ── IDT ─────────────────────────────────────────────────────────── */
    idt_init();
    sti();
    kprintf("[ok] IDT\n");

    /* ── PMM ─────────────────────────────────────────────────────────── */
    pmm_init(info->mmap, info->mmap_count,
             info->kernel_phys_base, info->kernel_size);
    kprintf("[ok] PMM (%lu MiB free)\n",
            pmm_free_pages() * PAGE_SIZE / (1024 * 1024));

    /* ── VMM ─────────────────────────────────────────────────────────── */
    vmm_init();
    kprintf("[ok] VMM\n");

    /* ── Heap ────────────────────────────────────────────────────────── */
    heap_init();
    kprintf("[ok] Heap\n");

    /* ── Timer ───────────────────────────────────────────────────────── */
    timer_init(100);  /* 100 Hz = 10 ms tick */
    kprintf("[ok] PIT (100 Hz)\n");

    /* ── Keyboard ────────────────────────────────────────────────────── */
    keyboard_init();
    kprintf("[ok] Keyboard\n");

    /* ── Scheduler ───────────────────────────────────────────────────── */
    sched_init();
    task_create("task_a", task_a);
    task_create("task_b", task_b);
    task_create("shell",  shell_task);
    timer_enable_preemption();
    kprintf("[ok] Scheduler (3 tasks, preemptive)\n");

    kprintf("\nBoot complete.\n");
    serial_write("Boot complete.\n");

    /* The idle loop: yield to other tasks forever. */
    for (;;)
        sched_yield();
}
