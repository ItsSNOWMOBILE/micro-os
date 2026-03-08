/*
 * kernel.h — Core kernel definitions.
 */

#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../boot/bootinfo.h"

/* ── Inline port I/O ─────────────────────────────────────────────────────── */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* ── Misc ────────────────────────────────────────────────────────────────── */

static inline void hlt(void) {
    __asm__ volatile("hlt");
}

static inline void cli(void) {
    __asm__ volatile("cli");
}

static inline void sti(void) {
    __asm__ volatile("sti");
}

void panic(const char *msg);

#endif /* KERNEL_H */
