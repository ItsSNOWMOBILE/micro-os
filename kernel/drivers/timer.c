/*
 * timer.c — PIT channel 0 driver.
 *
 * The PIT runs at a base frequency of 1,193,182 Hz.  We program the
 * reload counter to generate interrupts at the desired frequency (e.g.
 * 100 Hz for a 10 ms tick).  IRQ 0 is wired to vector 32 via the
 * remapped PIC.
 */

#include "timer.h"
#include "../kernel.h"
#include "../interrupts/idt.h"
#include "../sched/task.h"
#include <stdbool.h>

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
#define PIT_BASE_HZ  1193182
#define IRQ0_VECTOR  32

static volatile uint64_t g_ticks;
static bool sched_active;
static volatile bool in_schedule;

static void
timer_handler(InterruptFrame *frame)
{
    (void)frame;
    g_ticks++;

    /* Send EOI to master PIC. */
    outb(0x20, 0x20);

    /* Preemptive scheduling. */
    if (sched_active && !in_schedule) {
        in_schedule = true;
        sched_schedule();
        in_schedule = false;
    }
}

void timer_enable_preemption(void) { sched_active = true; }

void
timer_init(uint32_t frequency_hz)
{
    g_ticks = 0;
    sched_active = false;
    in_schedule = false;

    uint16_t divisor = (uint16_t)(PIT_BASE_HZ / frequency_hz);
    if (divisor == 0) divisor = 1;

    /* Channel 0, access mode lobyte/hibyte, mode 2 (rate generator). */
    outb(PIT_CMD, 0x34);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    idt_register_handler(IRQ0_VECTOR, timer_handler);

    /* Unmask IRQ 0 on the master PIC. */
    uint8_t mask = inb(0x21);
    mask &= ~(1 << 0);
    outb(0x21, mask);
}

uint64_t
timer_ticks(void)
{
    return g_ticks;
}
