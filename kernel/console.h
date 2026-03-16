/*
 * console.h — Framebuffer text console.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdbool.h>
#include "../boot/bootinfo.h"

void console_init(Framebuffer *fb);
void console_putchar(char c);
void console_write(const char *s);
void console_clear(void);

/* Formatted print (subset: %s, %d, %u, %x, %p, %c, %%). */
void kprintf(const char *fmt, ...);

/* Framebuffer accessors (used by mouse driver). */
uint32_t *console_get_pixels(void);
uint32_t  console_get_pitch(void);
void      console_get_screen_size(uint32_t *w, uint32_t *h);

/* Tell the console that the mouse cursor is active so framebuffer
 * writes will hide/show it to prevent artifacts. */
void      console_set_mouse_ready(void);

/* Returns true if the console is mid-write (cursor should not be drawn). */
bool      console_is_writing(void);

/* Batch cursor guard: hide cursor before a burst of console_putchar calls,
 * show it after.  Nesting-safe. */
void      console_begin_batch(void);
void      console_end_batch(void);

#endif /* CONSOLE_H */
