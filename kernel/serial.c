/*
 * serial.c — COM1 (0x3F8) serial port driver.
 *
 * Initialises COM1 at 115200 baud, 8N1.  Used for debug logging via
 * QEMU's -serial stdio redirection so kernel messages appear in the
 * host terminal alongside the graphical framebuffer.
 */

#include "serial.h"
#include "kernel.h"

#define COM1 0x3F8

void
serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* disable interrupts           */
    outb(COM1 + 3, 0x80);   /* enable DLAB (set baud rate)  */
    outb(COM1 + 0, 0x01);   /* divisor lo: 115200 baud      */
    outb(COM1 + 1, 0x00);   /* divisor hi                   */
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, 1 stop    */
    outb(COM1 + 2, 0xC7);   /* enable FIFO, clear, 14-byte  */
    outb(COM1 + 4, 0x0B);   /* IRQs enabled, RTS/DSR set    */
}

static inline void
serial_wait_ready(void)
{
    while (!(inb(COM1 + 5) & 0x20))
        ;
}

void
serial_putchar(char c)
{
    serial_wait_ready();
    outb(COM1, (uint8_t)c);
}

void
serial_write(const char *s)
{
    while (*s) {
        serial_wait_ready();
        if (*s == '\n') {
            outb(COM1, '\r');
            serial_wait_ready();
        }
        outb(COM1, (uint8_t)*s++);
    }
}
