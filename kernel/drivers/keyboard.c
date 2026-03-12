/*
 * keyboard.c — PS/2 keyboard driver (scan code set 1).
 *
 * Handles IRQ 1 (vector 33).  Translates scan codes to ASCII using a
 * simple lookup table and pushes characters into a ring buffer that
 * keyboard_getchar reads from.
 */

#include "keyboard.h"
#include "../kernel.h"
#include "../interrupts/idt.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define IRQ1_VECTOR     33

#define KBD_BUFFER_SIZE 256

/* Ring buffer for key input. */
static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_read;
static volatile uint32_t kbd_write;

static bool shift_held;

/* US keyboard layout scan code → ASCII (lowercase). */
static const char scancode_map[128] = {
    0,   27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',  0,
    '*',  0,  ' ',  0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* F1–F10 */
    0, 0,                            /* Num Lock, Scroll Lock */
    0, 0, 0, '-',                    /* Home, Up, PgUp, - */
    0, 0, 0, '+',                    /* Left, 5, Right, + */
    0, 0, 0, 0, 0,                   /* End, Down, PgDn, Ins, Del */
    0, 0, 0,                         /* unused */
    0, 0,                            /* F11, F12 */
};

static const char scancode_map_shift[128] = {
    0,   27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',  0,
    '*',  0,  ' ',  0,
};

static void
kbd_push(char c)
{
    uint32_t next = (kbd_write + 1) % KBD_BUFFER_SIZE;
    if (next == kbd_read)
        return;  /* buffer full */
    kbd_buffer[kbd_write] = c;
    kbd_write = next;
}

static void
keyboard_handler(InterruptFrame *frame)
{
    (void)frame;

    uint8_t sc = inb(KBD_DATA_PORT);

    /* Left/right shift press. */
    if (sc == 0x2A || sc == 0x36) {
        shift_held = true;
        goto eoi;
    }
    /* Left/right shift release. */
    if (sc == 0xAA || sc == 0xB6) {
        shift_held = false;
        goto eoi;
    }

    /* Ignore key releases (bit 7 set). */
    if (sc & 0x80)
        goto eoi;

    char c = shift_held ? scancode_map_shift[sc] : scancode_map[sc];
    if (c)
        kbd_push(c);

eoi:
    outb(0x20, 0x20);
}

void
keyboard_init(void)
{
    kbd_read  = 0;
    kbd_write = 0;
    shift_held = false;

    idt_register_handler(IRQ1_VECTOR, keyboard_handler);

    /* Unmask IRQ 1 on the master PIC. */
    uint8_t mask = inb(0x21);
    mask &= ~(1 << 1);
    outb(0x21, mask);
}

bool
keyboard_has_key(void)
{
    return kbd_read != kbd_write;
}

char
keyboard_getchar(void)
{
    while (kbd_read == kbd_write)
        hlt();  /* sleep until interrupt */

    char c = kbd_buffer[kbd_read];
    kbd_read = (kbd_read + 1) % KBD_BUFFER_SIZE;
    return c;
}
