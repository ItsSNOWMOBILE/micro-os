/*
 * keyboard.h — PS/2 keyboard driver.
 *
 * Supports full US keyboard layout including extended keys (arrows,
 * Home, End, Delete, Insert, PgUp, PgDn), Ctrl combinator, and
 * proper Caps Lock (letters only).
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* Special key codes returned by keyboard_getchar().
 * Normal ASCII characters are returned as-is (0x01-0x7F).
 * Special keys use values >= 0x80 that don't collide with ASCII. */
#define KEY_UP       0x80
#define KEY_DOWN     0x81
#define KEY_LEFT     0x82
#define KEY_RIGHT    0x83
#define KEY_HOME     0x84
#define KEY_END      0x85
#define KEY_INSERT   0x86
#define KEY_DELETE   0x87
#define KEY_PGUP     0x88
#define KEY_PGDN     0x89
#define KEY_F1       0x8A
#define KEY_F2       0x8B
#define KEY_F3       0x8C
#define KEY_F4       0x8D
#define KEY_F5       0x8E
#define KEY_F6       0x8F
#define KEY_F7       0x90
#define KEY_F8       0x91
#define KEY_F9       0x92
#define KEY_F10      0x93
#define KEY_F11      0x94
#define KEY_F12      0x95

void keyboard_init(void);
bool keyboard_has_key(void);

/* Returns a char (ASCII) or a KEY_* special code.
 * Use uint16_t to accommodate both. */
uint16_t keyboard_getchar(void);

/* Modifier state queries. */
bool keyboard_ctrl_held(void);
bool keyboard_alt_held(void);
bool keyboard_shift_held(void);

/* Discard any buffered keystrokes. */
void keyboard_flush(void);

/* Register the PS/2 keyboard with the HAL. */
void keyboard_register_hal(void);

#endif /* KEYBOARD_H */
