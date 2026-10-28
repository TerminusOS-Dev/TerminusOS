#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

extern bool shift_pressed;
#define CHAR_ARROW_UP    1
#define CHAR_ARROW_DOWN  2
#define CHAR_ARROW_LEFT  3
#define CHAR_ARROW_RIGHT 4
#define CHAR_TAB         9
#define CHAR_SHIFT_TAB   6
#define CHAR_F3          132
#define CHAR_F4          133
#define CHAR_PGUP        5
#define CHAR_PGDN        7
#define CHAR_SHIFT_RIGHT    16
#define CHAR_CTRL_F         134
#define CHAR_DEL            135
#define CHAR_CTRL_Z         136
#define CHAR_CTRL_SHIFT_Z   137
#define CHAR_CTRL_ESC       138
#define CHAR_SHIFT_ESC      139
#define CHAR_CTRL_C         140
#define CHAR_CTRL_V         141
#define CHAR_SHIFT_UP       142
#define CHAR_SHIFT_DOWN     143
#define CHAR_SHIFT_LEFT     144
// CHAR_SHIFT_RIGHT уже = 16, переопределяем нормально:
#undef  CHAR_SHIFT_RIGHT
#define CHAR_SHIFT_RIGHT    145
#define CHAR_CTRL_A         146
#define CHAR_CTRL_E         147
#define CHAR_HOME           148
#define CHAR_END            149
// ── добавлено для расширенного nano: копирование/вырезание строк,
// прыжки по словам, переход к строке:колонке ──────────────────────────────
#define CHAR_CTRL_K         150   // копировать текущую строку
#define CHAR_CTRL_X         151   // вырезать выделение
#define CHAR_CTRL_SHIFT_A   152   // вырезать текущую строку целиком
#define CHAR_CTRL_SHIFT_X   153   // диалог "перейти к строке:колонке"
#define CHAR_CTRL_LEFT      154   // прыжок на слово влево
#define CHAR_CTRL_RIGHT     155   // прыжок на слово вправо
#define CHAR_CTRL_BACKSPACE 156   // удалить слово влево
#define CHAR_CTRL_DEL       157   // удалить слово вправо
#define CHAR_CTRL_H          158   // справка (список горячих клавиш)
#define CHAR_CTRL_SHIFT_LEFT  159   // выделение по словам — влево
#define CHAR_CTRL_SHIFT_RIGHT 160   // выделение по словам — вправо
#define CHAR_WIN_V             161  // Win+V — панель истории буфера обмена
#define CHAR_CTRL_ENTER         162 // Ctrl+Enter — подтверждение опасного действия

extern "C" uint8_t get_key();
void keyboard_on_irq(); /* IRQ1: drain HW into queue */
uint32_t keyboard_get_state(); /* bitmask of held game keys */
uint8_t keyboard_check_ctrl_c(); /* pop first queued key only if it is CHAR_CTRL_C */

// Кладёт символ в приоритетную однобайтовую очередь — следующий get_key()
// заберёт именно его, раньше чем пойдёт опрашивать порты клавиатуры.
// Нужно для IRQ1-детектора Ctrl+C (interrupts.cpp): если он подсмотрел
// символ через get_key(), а это оказался не Ctrl+C — "кладёт его обратно",
// чтобы обычная доставка (например, sys_read() внутри программы) его
// всё равно увидела и не потеряла нажатие.
void keyboard_inject(uint8_t k);

// Non-destructive: true if software queue has at least one byte.
bool keyboard_has_input(void);

#endif // KEYBOARD_H
