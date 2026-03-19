/*
 * timer.h — PIT (Programmable Interval Timer) driver.
 *
 * Configures PIT channel 0 to fire IRQ 0 at ~100 Hz for preemptive
 * scheduling.  A monotonic tick counter is exposed for timekeeping.
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void     timer_init(uint32_t frequency_hz);
void     timer_enable_preemption(void);
uint64_t timer_ticks(void);

/* Register the PIT with the HAL. */
void timer_register_hal(void);

#endif /* TIMER_H */
