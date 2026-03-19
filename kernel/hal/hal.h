/*
 * hal.h — Hardware Abstraction Layer.
 *
 * Defines ops-struct interfaces for each device class.  Concrete drivers
 * (PS/2, PIT, COM1, etc.) register themselves at init time.  The rest
 * of the kernel calls through the HAL so drivers can be swapped without
 * touching consumer code.
 */

#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stdbool.h>

/* ── Input (keyboard) ───────────────────────────────────────────────────── */

typedef struct {
    void     (*init)(void);
    bool     (*has_key)(void);
    uint16_t (*getchar)(void);
    void     (*flush)(void);
    bool     (*ctrl_held)(void);
    bool     (*alt_held)(void);
    bool     (*shift_held)(void);
} HalInputOps;

void     hal_input_register(const HalInputOps *ops);
void     hal_input_init(void);
bool     hal_input_has_key(void);
uint16_t hal_input_getchar(void);
void     hal_input_flush(void);
bool     hal_input_ctrl_held(void);
bool     hal_input_alt_held(void);
bool     hal_input_shift_held(void);

/* ── Pointing device (mouse) ────────────────────────────────────────────── */

typedef struct {
    int32_t x, y;
    bool    left, right, middle;
    int8_t  dx, dy;
} HalPointerState;

typedef struct {
    void    (*init)(void);
    void    (*get_state)(HalPointerState *out);
    void    (*hide_cursor)(void);
    void    (*show_cursor)(void);
    int32_t (*cursor_x)(void);
    int32_t (*cursor_y)(void);
} HalPointerOps;

void    hal_pointer_register(const HalPointerOps *ops);
void    hal_pointer_init(void);
void    hal_pointer_get_state(HalPointerState *out);
void    hal_pointer_hide_cursor(void);
void    hal_pointer_show_cursor(void);
int32_t hal_pointer_x(void);
int32_t hal_pointer_y(void);

/* ── Timer ──────────────────────────────────────────────────────────────── */

typedef struct {
    void     (*init)(uint32_t frequency_hz);
    void     (*enable_preemption)(void);
    uint64_t (*ticks)(void);
} HalTimerOps;

void     hal_timer_register(const HalTimerOps *ops);
void     hal_timer_init(uint32_t frequency_hz);
void     hal_timer_enable_preemption(void);
uint64_t hal_timer_ticks(void);

/* ── Serial / debug output ──────────────────────────────────────────────── */

typedef struct {
    void (*init)(void);
    void (*putchar)(char c);
    void (*write)(const char *s);
} HalSerialOps;

void hal_serial_register(const HalSerialOps *ops);
void hal_serial_init(void);
void hal_serial_putchar(char c);
void hal_serial_write(const char *s);

#endif /* HAL_H */
