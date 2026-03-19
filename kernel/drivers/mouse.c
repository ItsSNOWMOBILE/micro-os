/*
 * mouse.c — PS/2 mouse driver.
 *
 * Handles IRQ 12 (vector 44).  The PS/2 mouse sends 3-byte packets:
 *   byte 0: flags (buttons, sign, overflow)
 *   byte 1: X delta
 *   byte 2: Y delta
 *
 * The driver accumulates position, clamps to screen bounds, and
 * renders a small cursor on the framebuffer.
 */

#include "mouse.h"
#include "../kernel.h"
#include "../interrupts/idt.h"
#include "../hal/hal.h"
#include "../console.h"

#define MOUSE_DATA_PORT   0x60
#define MOUSE_STATUS_PORT 0x64
#define MOUSE_CMD_PORT    0x64
#define IRQ12_VECTOR      44

static volatile int32_t  mouse_px, mouse_py;
static volatile bool     btn_left, btn_right, btn_middle;
static volatile int8_t   last_dx, last_dy;

static uint32_t screen_w, screen_h;

/* Packet assembly. */
static uint8_t packet[3];
static int     packet_idx;

/* ── Cursor rendering ────────────────────────────────────────────────────── */

/*
 * Simple 8x12 arrow cursor bitmap.
 * 1 = white (cursor), 2 = black (outline), 0 = transparent.
 */
#define CURSOR_W 8
#define CURSOR_H 12

static const uint8_t cursor_bitmap[CURSOR_H][CURSOR_W] = {
    {2,0,0,0,0,0,0,0},
    {2,2,0,0,0,0,0,0},
    {2,1,2,0,0,0,0,0},
    {2,1,1,2,0,0,0,0},
    {2,1,1,1,2,0,0,0},
    {2,1,1,1,1,2,0,0},
    {2,1,1,1,1,1,2,0},
    {2,1,1,1,1,1,1,2},
    {2,1,1,1,1,2,2,0},
    {2,1,2,2,1,2,0,0},
    {2,2,0,0,2,1,2,0},
    {0,0,0,0,0,2,0,0},
};

/* Saved pixels under the cursor so we can restore them. */
static uint32_t saved_under[CURSOR_H][CURSOR_W];
static int32_t  saved_cx = -1, saved_cy = -1;
static bool     cursor_visible;

/* Access the framebuffer directly for cursor drawing. */
extern uint32_t *console_get_pixels(void);
extern uint32_t console_get_pitch(void);

static inline void
draw_cursor(int32_t cx, int32_t cy)
{
    uint32_t *pixels = console_get_pixels();
    uint32_t pitch = console_get_pitch() / 4;

    /* Save pixels under cursor and draw it. */
    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t px = cx + x;
            int32_t py = cy + y;
            if (px < 0 || px >= (int32_t)screen_w ||
                py < 0 || py >= (int32_t)screen_h) {
                saved_under[y][x] = 0;
                continue;
            }
            saved_under[y][x] = pixels[py * pitch + px];
            uint8_t v = cursor_bitmap[y][x];
            if (v == 1)
                pixels[py * pitch + px] = 0x00FFFFFF;  /* white */
            else if (v == 2)
                pixels[py * pitch + px] = 0x00000000;  /* black */
        }
    }
    saved_cx = cx;
    saved_cy = cy;
    cursor_visible = true;
}

static inline void
erase_cursor(void)
{
    if (!cursor_visible) return;

    uint32_t *pixels = console_get_pixels();
    uint32_t pitch = console_get_pitch() / 4;

    for (int y = 0; y < CURSOR_H; y++) {
        for (int x = 0; x < CURSOR_W; x++) {
            int32_t px = saved_cx + x;
            int32_t py = saved_cy + y;
            if (px < 0 || px >= (int32_t)screen_w ||
                py < 0 || py >= (int32_t)screen_h)
                continue;
            if (cursor_bitmap[y][x] != 0)
                pixels[py * pitch + px] = saved_under[y][x];
        }
    }
    cursor_visible = false;
}

/* ── PS/2 helpers ────────────────────────────────────────────────────────── */

static void
mouse_wait_input(void)
{
    int timeout = 100000;
    while (timeout--) {
        if (inb(MOUSE_STATUS_PORT) & 0x01)
            return;
    }
}

static void
mouse_wait_output(void)
{
    int timeout = 100000;
    while (timeout--) {
        if (!(inb(MOUSE_STATUS_PORT) & 0x02))
            return;
    }
}

static void
mouse_write(uint8_t data)
{
    mouse_wait_output();
    outb(MOUSE_CMD_PORT, 0xD4);  /* tell controller: next byte goes to mouse */
    mouse_wait_output();
    outb(MOUSE_DATA_PORT, data);
}

static uint8_t
mouse_read(void)
{
    mouse_wait_input();
    return inb(MOUSE_DATA_PORT);
}

/* ── IRQ handler ─────────────────────────────────────────────────────────── */

static void
mouse_handler(InterruptFrame *frame)
{
    (void)frame;

    uint8_t status = inb(MOUSE_STATUS_PORT);
    if (!(status & 0x20)) {
        /* Not a mouse byte — read and discard. */
        inb(MOUSE_DATA_PORT);
        goto eoi;
    }

    uint8_t data = inb(MOUSE_DATA_PORT);
    packet[packet_idx++] = data;

    if (packet_idx < 3)
        goto eoi;

    /* Full 3-byte packet received. */
    packet_idx = 0;

    uint8_t flags = packet[0];

    /* Check for overflow or invalid packet. */
    if (flags & 0xC0)
        goto eoi;

    /* Bit 3 must always be set in a valid packet. */
    if (!(flags & 0x08))
        goto eoi;

    btn_left   = (flags & 0x01) != 0;
    btn_right  = (flags & 0x02) != 0;
    btn_middle = (flags & 0x04) != 0;

    int16_t dx = (int16_t)packet[1] - ((flags & 0x10) ? 256 : 0);
    int16_t dy = (int16_t)packet[2] - ((flags & 0x20) ? 256 : 0);

    last_dx = (int8_t)dx;
    last_dy = (int8_t)dy;

    /* Update position (Y is inverted: PS/2 positive = up, screen positive = down). */
    mouse_px += dx;
    mouse_py -= dy;

    /* Clamp to screen bounds. */
    if (mouse_px < 0) mouse_px = 0;
    if (mouse_py < 0) mouse_py = 0;
    if (mouse_px >= (int32_t)screen_w) mouse_px = screen_w - 1;
    if (mouse_py >= (int32_t)screen_h) mouse_py = screen_h - 1;

    /* Only update the cursor if the console isn't mid-write.
     * The console's cursor_guard_leave will redraw us when it's done. */
    if (!console_is_writing()) {
        erase_cursor();
        draw_cursor(mouse_px, mouse_py);
    }

eoi:
    /* IRQ 12 is on the slave PIC — send EOI to both. */
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void
mouse_init(void)
{
    console_get_screen_size(&screen_w, &screen_h);
    mouse_px = screen_w / 2;
    mouse_py = screen_h / 2;
    btn_left = btn_right = btn_middle = false;
    last_dx = last_dy = 0;
    packet_idx = 0;
    cursor_visible = false;

    /* Enable the auxiliary (mouse) device on the PS/2 controller. */
    mouse_wait_output();
    outb(MOUSE_CMD_PORT, 0xA8);  /* enable auxiliary device */

    /* Enable IRQ 12 in the controller's configuration byte. */
    mouse_wait_output();
    outb(MOUSE_CMD_PORT, 0x20);  /* read config byte */
    mouse_wait_input();
    uint8_t config = inb(MOUSE_DATA_PORT);
    config |= 0x02;  /* enable IRQ 12 */
    config &= ~0x20;  /* clear disable-mouse bit */
    mouse_wait_output();
    outb(MOUSE_CMD_PORT, 0x60);  /* write config byte */
    mouse_wait_output();
    outb(MOUSE_DATA_PORT, config);

    /* Reset the mouse. */
    mouse_write(0xFF);
    mouse_read();  /* ACK */
    mouse_read();  /* self-test result (0xAA) */
    mouse_read();  /* mouse ID (0x00) */

    /* Set default settings. */
    mouse_write(0xF6);
    mouse_read();  /* ACK */

    /* Enable data reporting. */
    mouse_write(0xF4);
    mouse_read();  /* ACK */

    /* Register IRQ handler. */
    idt_register_handler(IRQ12_VECTOR, mouse_handler);

    /* Unmask IRQ 12 on the slave PIC (IRQ 12 = slave bit 4). */
    uint8_t mask = inb(0xA1);
    mask &= ~(1 << 4);
    outb(0xA1, mask);

    /* Also unmask IRQ 2 on the master PIC (cascade). */
    mask = inb(0x21);
    mask &= ~(1 << 2);
    outb(0x21, mask);

    /* Draw initial cursor and tell the console to coordinate with us. */
    draw_cursor(mouse_px, mouse_py);
    console_set_mouse_ready();
}

void
mouse_get_state(MouseState *out)
{
    out->x = mouse_px;
    out->y = mouse_py;
    out->left = btn_left;
    out->right = btn_right;
    out->middle = btn_middle;
    out->dx = last_dx;
    out->dy = last_dy;
}

int32_t mouse_x(void) { return mouse_px; }
int32_t mouse_y(void) { return mouse_py; }

void
mouse_hide_cursor(void)
{
    erase_cursor();
}

void
mouse_show_cursor(void)
{
    if (!cursor_visible)
        draw_cursor(mouse_px, mouse_py);
}

/* ── HAL registration ───────────────────────────────────────────────────── */

static void
mouse_hal_get_state(HalPointerState *out)
{
    out->x      = mouse_px;
    out->y      = mouse_py;
    out->left   = btn_left;
    out->right  = btn_right;
    out->middle = btn_middle;
    out->dx     = last_dx;
    out->dy     = last_dy;
}

static const HalPointerOps ps2_mouse_ops = {
    .init        = mouse_init,
    .get_state   = mouse_hal_get_state,
    .hide_cursor = mouse_hide_cursor,
    .show_cursor = mouse_show_cursor,
    .cursor_x    = mouse_x,
    .cursor_y    = mouse_y,
};

void mouse_register_hal(void) { hal_pointer_register(&ps2_mouse_ops); }
