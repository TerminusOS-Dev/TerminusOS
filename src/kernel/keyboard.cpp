#include "keyboard.h"
#include "../lib/screen.h"
#include "../drivers/serial.h"

// Порты контроллера клавиатуры
#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

// Скан-коды
#define SCAN_LSHIFT_PRESS 0x2A
#define SCAN_LSHIFT_RELEASE 0xAA
#define SCAN_RSHIFT_PRESS 0x36
#define SCAN_RSHIFT_RELEASE 0xB6
#define SCAN_CAPSLOCK       0x3A
#define SCAN_LCTRL_PRESS    0x1D
#define SCAN_LCTRL_RELEASE  0x9D

// Глобальные флаги
bool shift_pressed = false;
bool caps_lock     = false;
static bool ctrl_pressed = false;
static bool win_pressed  = false;  // левая клавиша Win (E0 5B / E0 DB)
// E0-префикс расширенных клавиш (стрелки, Del, Home и т.д.)
// Нужен чтобы не спутать "фиктивный" shift (E0+0x2A / E0+0xAA) с настоящим
static bool e0_prefix = false;

// Таблица соответствия скан-кодов ASCII символам (без Shift)
static char scan_code_table[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

// Таблица для Shift (символы верхнего регистра и спецсимволы)
static char scan_code_table_shift[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

// Функция чтения с порта
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

/* Ring buffer — IRQ and poll both feed/drain it */
#define KBD_Q_SIZE 32
static uint8_t kbd_q[KBD_Q_SIZE];
static volatile uint8_t kbd_q_head = 0; /* write */
static volatile uint8_t kbd_q_tail = 0; /* read */

static void kbd_q_push(uint8_t k) {
    if (k == 0) return;
    uint8_t next = (uint8_t)((kbd_q_head + 1) % KBD_Q_SIZE);
    if (next == kbd_q_tail) return; /* full — drop */
    kbd_q[kbd_q_head] = k;
    kbd_q_head = next;
}

static uint8_t kbd_q_pop(void) {
    if (kbd_q_tail == kbd_q_head) return 0;
    uint8_t k = kbd_q[kbd_q_tail];
    kbd_q_tail = (uint8_t)((kbd_q_tail + 1) % KBD_Q_SIZE);
    return k;
}

void keyboard_inject(uint8_t k) { kbd_q_push(k); }

bool keyboard_has_input(void) {
    return kbd_q_tail != kbd_q_head;
}

/* Pressed-key bitmap (set/clear by make/break scancodes) */
static uint8_t key_down[128];

uint32_t keyboard_get_state(void) {
    /* bits: 0=W 1=S 2=A 3=D 4=Q 5=E 6=Left 7=Right 8=Up 9=Down 10=X 11=Esc */
    uint32_t s = 0;
    if (key_down[0x11]) s |= (1u << 0);  /* W */
    if (key_down[0x1F]) s |= (1u << 1);  /* S */
    if (key_down[0x1E]) s |= (1u << 2);  /* A */
    if (key_down[0x20]) s |= (1u << 3);  /* D */
    if (key_down[0x10]) s |= (1u << 4);  /* Q */
    if (key_down[0x12]) s |= (1u << 5);  /* E */
    if (key_down[0x4B]) s |= (1u << 6);  /* Left  */
    if (key_down[0x4D]) s |= (1u << 7);  /* Right */
    if (key_down[0x48]) s |= (1u << 8);  /* Up    */
    if (key_down[0x50]) s |= (1u << 9);  /* Down  */
    if (key_down[0x2D]) s |= (1u << 10); /* X */
    if (key_down[0x01]) s |= (1u << 11); /* Esc */
    return s;
}


/* Read one scancode from hardware and translate → push to queue. Returns char or 0. */

/* Process one scancode (make/break). Returns translated char or 0. */
static uint8_t kbd_process_scancode(uint8_t scancode);

/* Called from IRQ1 — MUST read the data port to EOI the controller. */
void keyboard_on_irq(void) {
    /* Drain ALL pending keyboard bytes. Leaving data in the PS/2 buffer
       keeps IRQ1 asserted → interrupt storm / freeze. */
    for (int n = 0; n < 16; n++) {
        uint8_t status = inb(KEYBOARD_STATUS_PORT);
        if (!(status & 0x01)) break;
        if (status & 0x20) break; /* mouse — IRQ12 owns it */
        uint8_t scancode = inb(KEYBOARD_DATA_PORT);
        uint8_t ch = kbd_process_scancode(scancode);
        if (ch) kbd_q_push(ch);
    }
}

uint8_t get_key() {
    /* Prefer queued keys from IRQ */
    uint8_t k = kbd_q_pop();
    if (k) return k;

    /* Polling fallback (no IRQ / early boot). Read STATUS+DATA under CLI so
       the pending IRQ1 can't grab the scancode between the two reads and
       deliver the same keypress a second time. */
    __asm__ volatile("cli");
    uint8_t status = inb(KEYBOARD_STATUS_PORT);
    if (!(status & 0x01) || (status & 0x20)) {
        __asm__ volatile("sti");
        return 0;
    }
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    __asm__ volatile("sti");
    return kbd_process_scancode(scancode);
}

/* Pop the first queued key only if it is CHAR_CTRL_C — used by IRQ1 to hard-
   kill a running ELF without reading the PS/2 data port from IRQ context.
   Returns the key if it was Ctrl+C (and pops it), 0 otherwise (key left as is). */
uint8_t keyboard_check_ctrl_c(void) {
    if (kbd_q_tail != kbd_q_head) {
        uint8_t k = kbd_q[kbd_q_tail];
        if (k == CHAR_CTRL_C) {
            kbd_q_tail = (uint8_t)((kbd_q_tail + 1) % KBD_Q_SIZE);
            return k;
        }
    }
    return 0;
}

static uint8_t kbd_process_scancode(uint8_t scancode) {
    {

        // ── E0-префикс: расширенные клавиши (стрелки, Del, Home …) ──────────
        // Запоминаем, что следующий байт идёт с E0-префиксом.
        if (scancode == 0xE0) { e0_prefix = true; return 0; }

        // ── Ctrl ─────────────────────────────────────────────────────────────
        if (scancode == SCAN_LCTRL_PRESS)   { ctrl_pressed = true;  e0_prefix = false; return 0; }
        if (scancode == SCAN_LCTRL_RELEASE) { ctrl_pressed = false; e0_prefix = false; return 0; }

        // ── Win (левая) ──────────────────────────────────────────────────────
        // Приходит только с E0-префиксом (расширенная клавиша).
        if (scancode == 0x5B) { win_pressed = true;  e0_prefix = false; return 0; }
        if (scancode == 0xDB) { win_pressed = false; e0_prefix = false; return 0; }

        // ── Shift ─────────────────────────────────────────────────────────────
        // E0+0x2A и E0+0xAA — «фиктивный» shift, который клавиатура посылает
        // вокруг расширенных клавиш (стрелки, PgUp …).  Его НЕЛЬЗЯ трактовать
        // как нажатие/отпускание настоящего Shift, иначе shift_pressed
        // сбросится в false до того как мы обработаем стрелку.
        if (scancode == SCAN_LSHIFT_RELEASE || scancode == SCAN_RSHIFT_RELEASE) {
            if (!e0_prefix) shift_pressed = false;   // настоящий Shift-release
            e0_prefix = false;
            return 0;
        }
        if (scancode == SCAN_LSHIFT_PRESS || scancode == SCAN_RSHIFT_PRESS) {
            if (!e0_prefix) shift_pressed = true;    // настоящий Shift-press
            e0_prefix = false;
            return 0;
        }

        // ── Caps Lock ─────────────────────────────────────────────────────────
        if (scancode == SCAN_CAPSLOCK) {
            caps_lock = !caps_lock;
            e0_prefix = false;
            return 0;
        }

        // ── Отпускание любой клавиши (бит 7) ─────────────────────────────────
        if (scancode & 0x80) {
            key_down[scancode & 0x7F] = 0;
            e0_prefix = false;
            return 0;
        }

        /* mark pressed (make code) */
        key_down[scancode & 0x7F] = 1;

        // ── Ctrl+буква ────────────────────────────────────────────────────────
        if (win_pressed && scancode == 0x2F) { e0_prefix = false; return CHAR_WIN_V; }
        if (ctrl_pressed) {
            e0_prefix = false;
            if (scancode == 0x21) return CHAR_CTRL_F;
            if (scancode == 0x2E) return CHAR_CTRL_C;
            if (scancode == 0x2F) return CHAR_CTRL_V;
            if (scancode == 0x1E) return shift_pressed ? CHAR_CTRL_SHIFT_A : CHAR_CTRL_A;
            if (scancode == 0x12) return CHAR_CTRL_E;
            if (scancode == 0x2C) return shift_pressed ? CHAR_CTRL_SHIFT_Z : CHAR_CTRL_Z;
            if (scancode == 0x25) return CHAR_CTRL_K;
            if (scancode == 0x2D) return shift_pressed ? CHAR_CTRL_SHIFT_X : CHAR_CTRL_X;
            if (scancode == 0x0E) return CHAR_CTRL_BACKSPACE;
            if (scancode == 0x23) return CHAR_CTRL_H;
            if (scancode == 0x1C) return CHAR_CTRL_ENTER;
        }

        // ── ESC ───────────────────────────────────────────────────────────────
        if (scancode == 0x01) {
            e0_prefix = false;
            if (ctrl_pressed)  return CHAR_CTRL_ESC;
            if (shift_pressed) return CHAR_SHIFT_ESC;
            return 27;
        }

        // ── Del / Home / End ──────────────────────────────────────────────────
        if (scancode == 0x53) { e0_prefix = false; return ctrl_pressed ? CHAR_CTRL_DEL : CHAR_DEL; }
        if (scancode == 0x47) { e0_prefix = false; return CHAR_HOME; }
        if (scancode == 0x4F) { e0_prefix = false; return CHAR_END;  }

        // ── Стрелки (с Shift, Ctrl или Ctrl+Shift) ───────────────────────────────
        if (scancode == 0x48) { e0_prefix = false; return shift_pressed ? CHAR_SHIFT_UP    : CHAR_ARROW_UP;    }
        if (scancode == 0x50) { e0_prefix = false; return shift_pressed ? CHAR_SHIFT_DOWN  : CHAR_ARROW_DOWN;  }
        if (scancode == 0x4B) {
            e0_prefix = false;
            if (ctrl_pressed && shift_pressed) return CHAR_CTRL_SHIFT_LEFT;
            if (ctrl_pressed)                  return CHAR_CTRL_LEFT;
            return shift_pressed ? CHAR_SHIFT_LEFT : CHAR_ARROW_LEFT;
        }
        if (scancode == 0x4D) {
            e0_prefix = false;
            if (ctrl_pressed && shift_pressed) return CHAR_CTRL_SHIFT_RIGHT;
            if (ctrl_pressed)                  return CHAR_CTRL_RIGHT;
            return shift_pressed ? CHAR_SHIFT_RIGHT : CHAR_ARROW_RIGHT;
        }

        // ── PgUp / PgDn ───────────────────────────────────────────────────────
        if (scancode == 0x49) { e0_prefix = false; return CHAR_PGUP; }
        if (scancode == 0x51) { e0_prefix = false; return CHAR_PGDN; }

        // ── Tab / Shift+Tab ───────────────────────────────────────────────────
        if (scancode == 0x0F) { e0_prefix = false; return shift_pressed ? (char)CHAR_SHIFT_TAB : (char)CHAR_TAB; }

        // ── Обычные символы ───────────────────────────────────────────────────
        e0_prefix = false;
        if (scancode < sizeof(scan_code_table)) {
            char c = 0;
            if (shift_pressed) {
                c = scan_code_table_shift[scancode];
            } else {
                c = scan_code_table[scancode];
                if (caps_lock && c >= 'a' && c <= 'z') c -= 32;
            }
            return c;
        }
    }
    return 0;
}