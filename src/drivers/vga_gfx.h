#ifndef VGA_GFX_H
#define VGA_GFX_H

#include <stdint.h>
#include <stdbool.h>

// ─── Размер экрана (динамический) ────────────────────────────────────────────
#define VGA_GFX_WIDTH  320   // mode 13h
#define VGA_GFX_HEIGHT 200

int gfx_get_width();
int gfx_get_height();

// ─── Индексы стандартных цветов VGA (0-15); полная палитра 0-255 ─────────────
#define GFX_BLACK         0
#define GFX_BLUE          1
#define GFX_GREEN         2
#define GFX_CYAN          3
#define GFX_RED           4
#define GFX_MAGENTA       5
#define GFX_BROWN         6
#define GFX_LIGHT_GRAY    7
#define GFX_DARK_GRAY     8
#define GFX_LIGHT_BLUE    9
#define GFX_LIGHT_GREEN   10
#define GFX_LIGHT_CYAN    11
#define GFX_LIGHT_RED     12
#define GFX_LIGHT_MAGENTA 13
#define GFX_YELLOW        14
#define GFX_WHITE         15
// 16-31: серые, 32-247: куб 6×6×6 RGB, 248-255: доп. серые (gfx_init_palette)

bool vga_gfx_active();
bool vga_lfb_available();
void vga_lfb_init(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint8_t bpp);

// ─── Переключение режима ──────────────────────────────────────────────────────
void vga_enter_mode13();       // 320x200x256 (chain-4)
void vga_enter_mode_640x480(); // 640x480x256 (Mode X, plane-based)
void vga_exit_to_text();       // Вернуться в текстовый режим 80x25
void vga_init_text_mode();     // Инициализация текстового режима при старте ядра

// ─── Палитра (r, g, b — значения 0-63; idx 0-255) ────────────────────────────
void gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void gfx_init_palette();   // Загрузить полную 256-цветную палитру VGA

// ─── Примитивы рисования (будущий API для программ) ─────────────────────────
void gfx_put_pixel(int x, int y, uint8_t color);
void gfx_clear(uint8_t color);
uint8_t* gfx_get_backbuffer();   // прямой доступ к backbuffer для быстрого блита
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint8_t color);  // только рамка
void gfx_draw_hline(int x, int y, int len, uint8_t color);
void gfx_draw_vline(int x, int y, int len, uint8_t color);
void gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color); // Bresenham

// Перенести backbuffer → VRAM (вызывать после рисования сцены, до cursor_draw)
void gfx_flip();

// Программный клиппинг — все gfx_put_pixel вне прямоугольника игнорируются
void gfx_set_clip(int x, int y, int w, int h);
void gfx_clear_clip();

// ─── Вывод текста (встроенный 8x8 шрифт) ────────────────────────────────────
void gfx_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);
void gfx_draw_text(int x, int y, const char* str, uint8_t fg, uint8_t bg);

// ─── Курсор мыши ─────────────────────────────────────────────────────────────
// Курсор рисуется автоматически внутри gfx_flip() — отдельных вызовов не нужно.
#define GFX_CURSOR_W 12
#define GFX_CURSOR_H 16
void gfx_cursor_show();
void gfx_cursor_hide();
void gfx_cursor_move(int x, int y);
void gfx_cursor_set_locked(bool locked);
int  gfx_cursor_get_x();
int  gfx_cursor_get_y();
// Быстрое обновление позиции без полного flip (курсор двинулся, сцена нет)
void gfx_cursor_update_pos();
void gfx_cursor_blit_backbuffer(int x, int y);  // курсор в backbuffer (анти‑мерцание)
// Устаревшие — оставлены для совместимости
void gfx_cursor_save   (int x, int y);
void gfx_cursor_restore(int x, int y);
void gfx_cursor_draw   (int x, int y);

#define GFX_TRANSPARENT 255

#endif // VGA_GFX_H
