/*
 * keyboard.h — PS/2 keyboard driver.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

void keyboard_init(void);
bool keyboard_has_key(void);
char keyboard_getchar(void);

#endif /* KEYBOARD_H */
