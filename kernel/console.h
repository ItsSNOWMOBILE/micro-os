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

#endif /* CONSOLE_H */
