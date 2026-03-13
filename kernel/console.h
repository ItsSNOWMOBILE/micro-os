/*
 * console.h — Framebuffer text console.
 */

#ifndef CONSOLE_H
#define CONSOLE_H

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

#endif /* CONSOLE_H */
