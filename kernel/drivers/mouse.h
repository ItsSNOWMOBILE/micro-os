/*
 * mouse.h — PS/2 mouse driver.
 */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t  x, y;        /* current cursor position (pixels) */
    bool     left;
    bool     right;
    bool     middle;
    int8_t   dx, dy;      /* last delta movement */
} MouseState;

void mouse_init(void);
void mouse_get_state(MouseState *out);

/* Hide/show the cursor around framebuffer writes to prevent artifacts. */
void mouse_hide_cursor(void);
void mouse_show_cursor(void);

/* Called by the console to get cursor position for rendering. */
int32_t mouse_x(void);
int32_t mouse_y(void);

#endif /* MOUSE_H */
