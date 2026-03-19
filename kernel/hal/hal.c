/*
 * hal.c — Hardware Abstraction Layer dispatch.
 *
 * Stores registered driver ops and delegates calls.  If no driver is
 * registered for a device class, calls are safe no-ops.
 */

#include "hal.h"

/* ── Input ──────────────────────────────────────────────────────────────── */

static const HalInputOps *g_input;

void     hal_input_register(const HalInputOps *ops) { g_input = ops; }
void     hal_input_init(void)       { if (g_input && g_input->init) g_input->init(); }
bool     hal_input_has_key(void)    { return g_input && g_input->has_key && g_input->has_key(); }
uint16_t hal_input_getchar(void)    { return g_input && g_input->getchar ? g_input->getchar() : 0; }
void     hal_input_flush(void)      { if (g_input && g_input->flush) g_input->flush(); }
bool     hal_input_ctrl_held(void)  { return g_input && g_input->ctrl_held && g_input->ctrl_held(); }
bool     hal_input_alt_held(void)   { return g_input && g_input->alt_held && g_input->alt_held(); }
bool     hal_input_shift_held(void) { return g_input && g_input->shift_held && g_input->shift_held(); }

/* ── Pointer ────────────────────────────────────────────────────────────── */

static const HalPointerOps *g_pointer;

void hal_pointer_register(const HalPointerOps *ops) { g_pointer = ops; }
void hal_pointer_init(void)    { if (g_pointer && g_pointer->init) g_pointer->init(); }

void hal_pointer_get_state(HalPointerState *out)
{
    if (g_pointer && g_pointer->get_state)
        g_pointer->get_state(out);
}

void    hal_pointer_hide_cursor(void) { if (g_pointer && g_pointer->hide_cursor) g_pointer->hide_cursor(); }
void    hal_pointer_show_cursor(void) { if (g_pointer && g_pointer->show_cursor) g_pointer->show_cursor(); }
int32_t hal_pointer_x(void)          { return g_pointer && g_pointer->cursor_x ? g_pointer->cursor_x() : 0; }
int32_t hal_pointer_y(void)          { return g_pointer && g_pointer->cursor_y ? g_pointer->cursor_y() : 0; }

/* ── Timer ──────────────────────────────────────────────────────────────── */

static const HalTimerOps *g_timer;

void     hal_timer_register(const HalTimerOps *ops) { g_timer = ops; }
void     hal_timer_init(uint32_t hz)        { if (g_timer && g_timer->init) g_timer->init(hz); }
void     hal_timer_enable_preemption(void)  { if (g_timer && g_timer->enable_preemption) g_timer->enable_preemption(); }
uint64_t hal_timer_ticks(void)              { return g_timer && g_timer->ticks ? g_timer->ticks() : 0; }

/* ── Serial ─────────────────────────────────────────────────────────────── */

static const HalSerialOps *g_serial;

void hal_serial_register(const HalSerialOps *ops) { g_serial = ops; }
void hal_serial_init(void)              { if (g_serial && g_serial->init) g_serial->init(); }
void hal_serial_putchar(char c)         { if (g_serial && g_serial->putchar) g_serial->putchar(c); }
void hal_serial_write(const char *s)    { if (g_serial && g_serial->write) g_serial->write(s); }
