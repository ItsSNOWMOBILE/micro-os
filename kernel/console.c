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
#include <stdarg.h>

static Framebuffer g_fb;
static uint32_t   *g_pixels;

static uint32_t g_cols;      /* text columns  */
static uint32_t g_rows;      /* text rows     */
static uint32_t g_cx;        /* cursor column */
static uint32_t g_cy;        /* cursor row    */

#define FG_COLOR 0x00CCCCCC  /* light grey */
#define BG_COLOR 0x00000000  /* black      */

/* ── Pixel helpers ───────────────────────────────────────────────────────── */

static inline void
put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x < g_fb.width && y < g_fb.height)
        g_pixels[y * (g_fb.pitch / 4) + x] = color;
}

/* ── Font rendering ──────────────────────────────────────────────────────── */

static void
draw_char(uint32_t col, uint32_t row, char c)
{
    uint32_t x0 = col * 8;
    uint32_t y0 = row * 16;
    const uint8_t *glyph = font8x16[(uint8_t)c];

    for (uint32_t y = 0; y < 16; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < 8; x++) {
            uint32_t color = (bits & (0x80 >> x)) ? FG_COLOR : BG_COLOR;
            put_pixel(x0 + x, y0 + y, color);
        }
    }
}

/* ── Scrolling ───────────────────────────────────────────────────────────── */

static void
scroll_up(void)
{
    uint32_t line_bytes = g_fb.pitch;
    uint32_t text_height = 16;

    /* Move all rows up by one text row (16 pixel rows). */
    memmove(g_pixels,
            (uint8_t *)g_pixels + text_height * line_bytes,
            (g_fb.height - text_height) * line_bytes);

    /* Clear the last row. */
    memset((uint8_t *)g_pixels + (g_fb.height - text_height) * line_bytes,
           0, text_height * line_bytes);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void
console_init(Framebuffer *fb)
{
    g_fb     = *fb;
    g_pixels = (uint32_t *)fb->base;
    g_cols   = fb->width / 8;
    g_rows   = fb->height / 16;
    g_cx     = 0;
    g_cy     = 0;
    console_clear();
}

void
console_clear(void)
{
    memset(g_pixels, 0, g_fb.height * g_fb.pitch);
    g_cx = 0;
    g_cy = 0;
}

void
console_putchar(char c)
{
    if (c == '\n') {
        g_cx = 0;
        g_cy++;
    } else if (c == '\r') {
        g_cx = 0;
    } else if (c == '\t') {
        g_cx = (g_cx + 4) & ~3u;
    } else {
        draw_char(g_cx, g_cy, c);
        g_cx++;
    }

    if (g_cx >= g_cols) {
        g_cx = 0;
        g_cy++;
    }

    if (g_cy >= g_rows) {
        scroll_up();
        g_cy = g_rows - 1;
    }
}

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
