/*
 * keyboard.c — PS/2 keyboard driver (scan code set 1).
 *
 * Handles IRQ 1 (vector 33).  Full US keyboard layout with:
 *   - Shift, Caps Lock, Ctrl, Alt modifiers
 *   - Extended scancodes (0xE0 prefix) for arrows, Home, End, etc.
 *   - Numpad keys
 *   - All printable characters and symbols
 */

#include "keyboard.h"
#include "../kernel.h"
#include "../interrupts/idt.h"
#include "../hal/hal.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define IRQ1_VECTOR     33

#define KBD_BUFFER_SIZE 256

/* Ring buffer stores uint16_t to hold both ASCII and special keys. */
static volatile uint16_t kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_read;
static volatile uint32_t kbd_write;

static volatile bool shift_held;
static volatile bool caps_on;
static volatile bool ctrl_held;
static volatile bool alt_held;
static volatile bool numlock_on;
static volatile bool extended;  /* saw 0xE0 prefix */

/* US keyboard layout: scan code set 1 -> ASCII (lowercase). */
static const char scancode_map[128] = {
/*  0x00 */ 0,    27,  '1', '2', '3', '4', '5', '6',
/*  0x08 */ '7',  '8', '9', '0', '-', '=', '\b', '\t',
/*  0x10 */ 'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',
/*  0x18 */ 'o',  'p', '[', ']', '\n', 0,   'a', 's',
/*  0x20 */ 'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',
/*  0x28 */ '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
/*  0x30 */ 'b',  'n', 'm', ',', '.', '/', 0,   '*',
/*  0x38 */ 0,    ' ', 0,    0,   0,   0,   0,   0,
/*  0x40 */ 0,     0,   0,   0,   0,   0,   0,   '7',
/*  0x48 */ '8',  '9', '-', '4', '5', '6', '+', '1',
/*  0x50 */ '2',  '3', '0', '.', 0,   0,   0,   0,
/*  0x58 */ 0,     0,   0,   0,   0,   0,   0,   0,
/*  0x60 */ 0,     0,   0,   0,   0,   0,   0,   0,
/*  0x68 */ 0,     0,   0,   0,   0,   0,   0,   0,
/*  0x70 */ 0,     0,   0,   0,   0,   0,   0,   0,
/*  0x78 */ 0,     0,   0,   0,   0,   0,   0,   0,
};

/* Shifted layout. */
static const char scancode_map_shift[128] = {
/*  0x00 */ 0,    27,  '!', '@', '#', '$', '%', '^',
/*  0x08 */ '&',  '*', '(', ')', '_', '+', '\b', '\t',
/*  0x10 */ 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
/*  0x18 */ 'O',  'P', '{', '}', '\n', 0,   'A', 'S',
/*  0x20 */ 'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
/*  0x28 */ '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
/*  0x30 */ 'B',  'N', 'M', '<', '>', '?', 0,   '*',
/*  0x38 */ 0,    ' ', 0,    0,   0,   0,   0,   0,
/*  0x40 */ 0,     0,   0,   0,   0,   0,   0,   '7',
/*  0x48 */ '8',  '9', '-', '4', '5', '6', '+', '1',
/*  0x50 */ '2',  '3', '0', '.', 0,   0,   0,   0,
};

/* F-key scancode to KEY_F* mapping. F1=0x3B .. F10=0x44, F11=0x57, F12=0x58 */
static uint16_t
fkey_map(uint8_t sc)
{
    if (sc >= 0x3B && sc <= 0x44)
        return KEY_F1 + (sc - 0x3B);
    if (sc == 0x57) return KEY_F11;
    if (sc == 0x58) return KEY_F12;
    return 0;
}

static void
kbd_push(uint16_t c)
{
    uint32_t next = (kbd_write + 1) % KBD_BUFFER_SIZE;
    if (next == kbd_read)
        return;
    kbd_buffer[kbd_write] = c;
    kbd_write = next;
}

static bool
is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char
to_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static char
to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static void
keyboard_handler(InterruptFrame *frame)
{
    (void)frame;

    uint8_t sc = inb(KBD_DATA_PORT);

    /* Extended scancode prefix. */
    if (sc == 0xE0) {
        extended = true;
        goto eoi;
    }

    if (extended) {
        extended = false;
        bool release = (sc & 0x80) != 0;
        uint8_t code = sc & 0x7F;

        /* Extended key releases: Ctrl, Alt. */
        if (release) {
            if (code == 0x1D) ctrl_held = false;   /* right Ctrl release */
            if (code == 0x38) alt_held = false;     /* right Alt release */
            goto eoi;
        }

        /* Extended key presses. */
        switch (code) {
        case 0x1D: ctrl_held = true;  goto eoi;   /* right Ctrl */
        case 0x38: alt_held = true;   goto eoi;   /* right Alt */
        case 0x48: kbd_push(KEY_UP);     goto eoi;
        case 0x50: kbd_push(KEY_DOWN);   goto eoi;
        case 0x4B: kbd_push(KEY_LEFT);   goto eoi;
        case 0x4D: kbd_push(KEY_RIGHT);  goto eoi;
        case 0x47: kbd_push(KEY_HOME);   goto eoi;
        case 0x4F: kbd_push(KEY_END);    goto eoi;
        case 0x49: kbd_push(KEY_PGUP);   goto eoi;
        case 0x51: kbd_push(KEY_PGDN);   goto eoi;
        case 0x52: kbd_push(KEY_INSERT); goto eoi;
        case 0x53: kbd_push(KEY_DELETE); goto eoi;
        case 0x1C: kbd_push('\n');       goto eoi;  /* numpad Enter */
        case 0x35: kbd_push('/');        goto eoi;  /* numpad / */
        default: goto eoi;
        }
    }

    /* Non-extended keys. */
    bool release = (sc & 0x80) != 0;
    uint8_t code = sc & 0x7F;

    /* Modifier key handling. */
    if (code == 0x2A || code == 0x36) {   /* left/right Shift */
        shift_held = !release;
        goto eoi;
    }
    if (code == 0x1D) {                    /* left Ctrl */
        ctrl_held = !release;
        goto eoi;
    }
    if (code == 0x38) {                    /* left Alt */
        alt_held = !release;
        goto eoi;
    }

    if (release)
        goto eoi;

    /* Toggle keys (on press only). */
    if (code == 0x3A) { caps_on = !caps_on;       goto eoi; }
    if (code == 0x45) { numlock_on = !numlock_on;  goto eoi; }

    /* F-keys. */
    uint16_t fk = fkey_map(code);
    if (fk) {
        kbd_push(fk);
        goto eoi;
    }

    /* Numpad keys (0x47-0x53) — when NumLock is off, act as navigation. */
    if (code >= 0x47 && code <= 0x53 && !numlock_on) {
        switch (code) {
        case 0x47: kbd_push(KEY_HOME);   goto eoi;
        case 0x48: kbd_push(KEY_UP);     goto eoi;
        case 0x49: kbd_push(KEY_PGUP);   goto eoi;
        case 0x4B: kbd_push(KEY_LEFT);   goto eoi;
        case 0x4D: kbd_push(KEY_RIGHT);  goto eoi;
        case 0x4F: kbd_push(KEY_END);    goto eoi;
        case 0x50: kbd_push(KEY_DOWN);   goto eoi;
        case 0x51: kbd_push(KEY_PGDN);   goto eoi;
        case 0x52: kbd_push(KEY_INSERT); goto eoi;
        case 0x53: kbd_push(KEY_DELETE); goto eoi;
        default: break;
        }
    }

    /* Regular character lookup. */
    char c;
    if (shift_held)
        c = (code < 128) ? scancode_map_shift[code] : 0;
    else
        c = (code < 128) ? scancode_map[code] : 0;

    if (c == 0)
        goto eoi;

    /* Caps Lock affects only letters. */
    if (caps_on && is_letter(c)) {
        if (shift_held)
            c = to_lower(c);  /* Shift+Caps = lowercase */
        else
            c = to_upper(c);
    }

    /* Ctrl+letter produces control codes (Ctrl+C = 0x03, etc.) */
    if (ctrl_held && c >= 'a' && c <= 'z') {
        kbd_push((uint16_t)(c - 'a' + 1));
        goto eoi;
    }
    if (ctrl_held && c >= 'A' && c <= 'Z') {
        kbd_push((uint16_t)(c - 'A' + 1));
        goto eoi;
    }
    kbd_push((uint16_t)c);

eoi:
    outb(0x20, 0x20);
}

void
keyboard_init(void)
{
    kbd_read   = 0;
    kbd_write  = 0;
    shift_held = false;
    caps_on    = false;
    ctrl_held  = false;
    alt_held   = false;
    numlock_on = true;
    extended   = false;

    while (inb(KBD_STATUS_PORT) & 0x01)
        inb(KBD_DATA_PORT);

    idt_register_handler(IRQ1_VECTOR, keyboard_handler);

    uint8_t mask = inb(0x21);
    mask &= ~(1 << 1);
    outb(0x21, mask);
}

bool
keyboard_has_key(void)
{
    return kbd_read != kbd_write;
}

uint16_t
keyboard_getchar(void)
{
    while (kbd_read == kbd_write)
        __asm__ volatile("sti; hlt");

    uint16_t c = kbd_buffer[kbd_read];
    kbd_read = (kbd_read + 1) % KBD_BUFFER_SIZE;
    return c;
}

void
keyboard_flush(void)
{
    kbd_read = kbd_write;
}

bool keyboard_ctrl_held(void)  { return ctrl_held; }
bool keyboard_alt_held(void)   { return alt_held; }
bool keyboard_shift_held(void) { return shift_held; }

/* ── HAL registration ───────────────────────────────────────────────────── */

static const HalInputOps ps2_keyboard_ops = {
    .init       = keyboard_init,
    .has_key    = keyboard_has_key,
    .getchar    = keyboard_getchar,
    .flush      = keyboard_flush,
    .ctrl_held  = keyboard_ctrl_held,
    .alt_held   = keyboard_alt_held,
    .shift_held = keyboard_shift_held,
};

void keyboard_register_hal(void) { hal_input_register(&ps2_keyboard_ops); }
