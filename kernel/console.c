/*
 * console.c — Framebuffer text console.
 *
 * Renders text using a built-in 8x16 bitmap font directly to the linear
 * framebuffer obtained from UEFI GOP.  Supports scrolling and a minimal
 * kprintf for kernel diagnostics.
 */

#include "console.h"
#include "string.h"
#include "font8x16.h"
#include "drivers/mouse.h"
#include "kernel.h"
#include <stdarg.h>

/* Set once mouse_init has been called. */
static bool mouse_ready;
/* Nesting counter: >0 means cursor is already hidden by an outer call. */
static volatile int cursor_hidden_depth;

static Framebuffer g_fb;
static uint32_t   *g_pixels;

static uint32_t g_cols;      /* text columns  */
static uint32_t g_rows;      /* text rows     */
static uint32_t g_cx;        /* cursor column */
static uint32_t g_cy;        /* cursor row    */

#define FG_COLOR 0x00CCCCCC  /* light grey */
#define BG_COLOR 0x00000000  /* black      */

/* Cached pitch in uint32_t units (pixels per row). */
static uint32_t g_stride;

/* ── Font rendering ──────────────────────────────────────────────────────── */

static void
draw_char(uint32_t col, uint32_t row, char c)
{
    uint32_t x0 = col * 8;
    uint32_t y0 = row * 16;
    const uint8_t *glyph = font8x16[(uint8_t)c];
    static const uint32_t colors[2] = { BG_COLOR, FG_COLOR };

    uint32_t *row_ptr = g_pixels + y0 * g_stride + x0;

    for (uint32_t y = 0; y < 16; y++) {
        uint8_t bits = glyph[y];
        row_ptr[0] = colors[(bits >> 7) & 1];
        row_ptr[1] = colors[(bits >> 6) & 1];
        row_ptr[2] = colors[(bits >> 5) & 1];
        row_ptr[3] = colors[(bits >> 4) & 1];
        row_ptr[4] = colors[(bits >> 3) & 1];
        row_ptr[5] = colors[(bits >> 2) & 1];
        row_ptr[6] = colors[(bits >> 1) & 1];
        row_ptr[7] = colors[bits & 1];
        row_ptr += g_stride;
    }
}

/* ── Scrolling ───────────────────────────────────────────────────────────── */

static void
scroll_up(void)
{
    uint32_t line_bytes = g_fb.pitch;
    uint32_t text_height = 16;
    uint32_t copy_bytes = (g_fb.height - text_height) * line_bytes;

    /* Source is always above destination — regions don't overlap upward,
     * so memcpy is safe and faster than memmove. */
    memcpy(g_pixels,
           (uint8_t *)g_pixels + text_height * line_bytes,
           copy_bytes);

    /* Clear the last row. */
    memset((uint8_t *)g_pixels + copy_bytes, 0, text_height * line_bytes);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void
console_init(Framebuffer *fb)
{
    g_fb     = *fb;
    g_pixels = (uint32_t *)fb->base;
    g_stride = fb->pitch / 4;
    g_cols   = fb->width / 8;
    g_rows   = fb->height / 16;
    g_cx     = 0;
    g_cy     = 0;
    console_clear();
}

/*
 * Hide the mouse cursor before a full-screen operation (clear, scroll).
 * For individual character draws, we check overlap instead.
 */
static inline void
cursor_guard_enter(void)
{
    if (!mouse_ready) return;
    cli();
    if (cursor_hidden_depth++ == 0)
        mouse_hide_cursor();
    sti();
}

static inline void
cursor_guard_leave(void)
{
    if (!mouse_ready) return;
    cli();
    if (--cursor_hidden_depth == 0)
        mouse_show_cursor();
    sti();
}

/*
 * Check if a character cell overlaps the mouse cursor.
 * If so, hide cursor before drawing and show after.
 */
static inline bool
char_overlaps_cursor(uint32_t col, uint32_t row)
{
    if (!mouse_ready) return false;
    int32_t cx = mouse_x();
    int32_t cy = mouse_y();
    int32_t x0 = (int32_t)(col * 8);
    int32_t y0 = (int32_t)(row * 16);
    /* CURSOR_W=8, CURSOR_H=12 from mouse.c */
    return !(x0 + 8 <= cx || cx + 8 <= x0 ||
             y0 + 16 <= cy || cy + 12 <= y0);
}

void
console_clear(void)
{
    cursor_guard_enter();
    memset(g_pixels, 0, g_fb.height * g_fb.pitch);
    g_cx = 0;
    g_cy = 0;
    cursor_guard_leave();
}

void
console_putchar(char c)
{
    bool need_hide = false;

    if (c == '\n') {
        g_cx = 0;
        g_cy++;
    } else if (c == '\r') {
        g_cx = 0;
    } else if (c == '\b') {
        if (g_cx > 0) {
            g_cx--;
        } else if (g_cy > 0) {
            g_cy--;
            g_cx = g_cols - 1;
        }
        need_hide = char_overlaps_cursor(g_cx, g_cy);
        if (need_hide) cursor_guard_enter();
        draw_char(g_cx, g_cy, ' ');
        if (need_hide) cursor_guard_leave();
    } else if (c == '\t') {
        g_cx = (g_cx + 4) & ~3u;
    } else {
        need_hide = char_overlaps_cursor(g_cx, g_cy);
        if (need_hide) cursor_guard_enter();
        draw_char(g_cx, g_cy, c);
        if (need_hide) cursor_guard_leave();
        g_cx++;
    }

    if (g_cx >= g_cols) {
        g_cx = 0;
        g_cy++;
    }

    if (g_cy >= g_rows) {
        /* Scroll affects the entire framebuffer — always guard. */
        cursor_guard_enter();
        scroll_up();
        cursor_guard_leave();
        g_cy = g_rows - 1;
    }
}

uint32_t *console_get_pixels(void) { return g_pixels; }
uint32_t  console_get_pitch(void)  { return g_fb.pitch; }
void console_get_screen_size(uint32_t *w, uint32_t *h)
{
    *w = g_fb.width;
    *h = g_fb.height;
}

void console_set_mouse_ready(void) { mouse_ready = true; }
bool console_is_writing(void) { return cursor_hidden_depth > 0; }
void console_begin_batch(void) { cursor_guard_enter(); }
void console_end_batch(void)   { cursor_guard_leave(); }

void
console_write(const char *s)
{
    while (*s)
        console_putchar(*s++);
}

/* ── kprintf ─────────────────────────────────────────────────────────────── */

static void
print_uint(uint64_t val, int base)
{
    char buf[20];
    int  i = 0;

    if (val == 0) {
        console_putchar('0');
        return;
    }

    while (val > 0) {
        uint64_t digit = val % base;
        buf[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        val /= base;
    }

    while (--i >= 0)
        console_putchar(buf[i]);
}

static void
print_int(int64_t val)
{
    if (val < 0) {
        console_putchar('-');
        print_uint((uint64_t)(-val), 10);
    } else {
        print_uint((uint64_t)val, 10);
    }
}

void
kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            console_putchar(*fmt++);
            continue;
        }
        fmt++;  /* skip '%' */

        /* Handle 'l' length modifier. */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            console_write(s ? s : "(null)");
            break;
        }
        case 'd': {
            int64_t val = is_long ? va_arg(ap, int64_t) : va_arg(ap, int);
            print_int(val);
            break;
        }
        case 'u': {
            uint64_t val = is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            print_uint(val, 10);
            break;
        }
        case 'x': {
            uint64_t val = is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            print_uint(val, 16);
            break;
        }
        case 'p': {
            console_write("0x");
            print_uint(va_arg(ap, uint64_t), 16);
            break;
        }
        case 'c':
            console_putchar((char)va_arg(ap, int));
            break;
        case '%':
            console_putchar('%');
            break;
        default:
            console_putchar('%');
            console_putchar(*fmt);
            break;
        }
        fmt++;
    }

    va_end(ap);
}
