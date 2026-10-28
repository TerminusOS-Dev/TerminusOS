#include "screen.h"
#include "../drivers/disk.h"
#include "../lib/string.h"
#include "scrollback.h"

// ==========================================
// Глобальные переменные VGA и Темы
// ==========================================
volatile uint16_t* video_memory = (volatile uint16_t*)0xB8000;
int cursor_pos = 0;

// ─── Буфер редиректа (echo text > file) ─────────────────────────────────────
#define REDIRECT_BUF_SIZE 4096
char  redirect_buf[REDIRECT_BUF_SIZE];
int   redirect_len   = 0;
bool  redirect_active = false;

void redirect_start() {
    redirect_len    = 0;
    redirect_buf[0] = 0;
    redirect_active = true;
}
void redirect_stop() {
    redirect_active = false;
    if (redirect_len < REDIRECT_BUF_SIZE)
        redirect_buf[redirect_len] = 0;
}
const char* redirect_get_buf() { return redirect_buf; }
int         redirect_get_len() { return redirect_len; }

// Цветовые константы
#define VGA_COLOR_BLACK 0
#define VGA_COLOR_BLUE 1
#define VGA_COLOR_GREEN 2
#define VGA_COLOR_CYAN 3
#define VGA_COLOR_RED 4
#define VGA_COLOR_MAGENTA 5
#define VGA_COLOR_BROWN 6
#define VGA_COLOR_LIGHT_GREY 7
#define VGA_COLOR_DARK_GREY 8
#define VGA_COLOR_LIGHT_BLUE 9
#define VGA_COLOR_LIGHT_GREEN 10
#define VGA_COLOR_LIGHT_CYAN 11
#define VGA_COLOR_LIGHT_RED 12
#define VGA_COLOR_LIGHT_MAGENTA 13
#define VGA_COLOR_LIGHT_BROWN 14
#define VGA_COLOR_WHITE 15

// Текущая тема (по умолчанию Classic)
uint8_t theme_bg = VGA_COLOR_BLACK;
uint8_t theme_fg = VGA_COLOR_WHITE;
uint8_t theme_bar_bg = VGA_COLOR_LIGHT_GREY;
uint8_t theme_bar_fg = VGA_COLOR_BLACK;
uint8_t theme_cursor = VGA_COLOR_WHITE; // Цвет курсора / символа под ним
uint8_t theme_cursor_bg   = VGA_COLOR_WHITE;  // Фон под курсором
uint8_t theme_cursor_char = VGA_COLOR_BLACK;  // Цвет символа под курсором
uint8_t theme_dir    = VGA_COLOR_LIGHT_CYAN;  // Цвет директорий в ls
uint8_t theme_file   = VGA_COLOR_WHITE;       // Цвет файлов в ls

// ==========================================
// Управление темой
// ==========================================

uint8_t get_theme_color() {
    return (theme_bg << 4) | theme_fg;
}

void set_theme(const char* name) {
    if (strcmp(name, "matrix") == 0) {
        theme_bg          = VGA_COLOR_BLACK;
        theme_fg          = VGA_COLOR_LIGHT_GREEN;
        theme_bar_bg      = VGA_COLOR_GREEN;
        theme_bar_fg      = VGA_COLOR_BLACK;
        theme_cursor      = VGA_COLOR_LIGHT_GREEN;
        theme_cursor_bg   = VGA_COLOR_LIGHT_GREEN;
        theme_cursor_char = VGA_COLOR_BLACK;
        theme_dir         = VGA_COLOR_LIGHT_CYAN;
        theme_file        = VGA_COLOR_LIGHT_GREEN;
    } else if (strcmp(name, "ocean") == 0) {
        theme_bg          = VGA_COLOR_BLUE;
        theme_fg          = VGA_COLOR_WHITE;
        theme_bar_bg      = VGA_COLOR_CYAN;
        theme_bar_fg      = VGA_COLOR_BLUE;
        theme_cursor      = VGA_COLOR_WHITE;
        theme_cursor_bg   = VGA_COLOR_CYAN;
        theme_cursor_char = VGA_COLOR_BLUE;
        theme_dir         = VGA_COLOR_LIGHT_GREEN;
        theme_file        = VGA_COLOR_WHITE;
    } else if (strcmp(name, "amber") == 0) {
        theme_bg          = VGA_COLOR_BLACK;
        theme_fg          = VGA_COLOR_LIGHT_BROWN;
        theme_bar_bg      = VGA_COLOR_BROWN;
        theme_bar_fg      = VGA_COLOR_BLACK;
        theme_cursor      = VGA_COLOR_LIGHT_BROWN;
        theme_cursor_bg   = VGA_COLOR_LIGHT_BROWN;
        theme_cursor_char = VGA_COLOR_BLACK;
        theme_dir         = VGA_COLOR_LIGHT_GREEN;
        theme_file        = VGA_COLOR_LIGHT_BROWN;
    } else if (strcmp(name, "bsod") == 0) {
        theme_bg          = VGA_COLOR_BLUE;
        theme_fg          = VGA_COLOR_WHITE;
        theme_bar_bg      = VGA_COLOR_WHITE;
        theme_bar_fg      = VGA_COLOR_BLUE;
        theme_cursor      = VGA_COLOR_WHITE;
        theme_cursor_bg   = VGA_COLOR_WHITE;
        theme_cursor_char = VGA_COLOR_BLUE;
        theme_dir         = VGA_COLOR_LIGHT_CYAN;
        theme_file        = VGA_COLOR_WHITE;
    } else if (strcmp(name, "red") == 0) {
        theme_bg          = VGA_COLOR_BLACK;
        theme_fg          = VGA_COLOR_RED;
        theme_bar_bg      = VGA_COLOR_RED;
        theme_bar_fg      = VGA_COLOR_WHITE;
        theme_cursor      = VGA_COLOR_LIGHT_RED;
        theme_cursor_bg   = VGA_COLOR_LIGHT_RED;
        theme_cursor_char = VGA_COLOR_BLACK;
        theme_dir         = VGA_COLOR_LIGHT_MAGENTA;
        theme_file        = VGA_COLOR_RED;
    } else { // default / classic
        theme_bg          = VGA_COLOR_BLACK;
        theme_fg          = VGA_COLOR_WHITE;
        theme_bar_bg      = VGA_COLOR_LIGHT_GREY;
        theme_bar_fg      = VGA_COLOR_BLACK;
        theme_cursor      = VGA_COLOR_WHITE;
        theme_cursor_bg   = VGA_COLOR_WHITE;
        theme_cursor_char = VGA_COLOR_BLACK;
        theme_dir         = VGA_COLOR_LIGHT_CYAN;
        theme_file        = VGA_COLOR_WHITE;
    }
}

// ==========================================
// VGA функции
// ==========================================

void disable_vga_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

void update_vga_cursor(int x, int y) {
    int pos = y * 80 + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void draw_block_cursor(int x, int y) {
    int pos = y * 80 + x;
    uint16_t current = video_memory[pos];
    uint8_t ch = current & 0xFF;

    // Фон = theme_cursor_bg, символ = theme_cursor_char
    // Если под курсором пробел/пусто — рисуем '_'
    uint8_t cursor_attr = (theme_cursor_bg << 4) | (theme_cursor_char & 0x0F);

    if (ch == ' ' || ch == 0) {
        ch = '_';
    }

    video_memory[pos] = ((uint16_t)cursor_attr << 8) | ch;
}

void clear_block_cursor(int x, int y) {
    int pos = y * 80 + x;
    uint16_t current = video_memory[pos];
    uint8_t ch = current & 0xFF;

    // Восстанавливаем обычный цвет
    uint8_t normal_color = get_theme_color();

    if (ch == '_') {
        ch = ' ';
    }

    video_memory[pos] = (normal_color << 8) | ch;
}


// ─── Minimal ANSI/VT100 parser (enough for busybox vi / clear / colors) ───────
static int  ansi_state = 0;   // 0=normal 1=got ESC 2=CSI params
static int  ansi_params[8];
static int  ansi_nparams = 0;
static int  ansi_in_num = 0;
static int  ansi_save_pos = 0;
static uint8_t ansi_fg = 0xFF; // 0xFF = use theme
static uint8_t ansi_bg = 0xFF;

static uint8_t ansi_attr() {
    uint8_t fg = (ansi_fg == 0xFF) ? theme_fg : ansi_fg;
    uint8_t bg = (ansi_bg == 0xFF) ? theme_bg : ansi_bg;
    return (uint8_t)((bg << 4) | (fg & 0x0F));
}

static void ansi_clamp_cursor() {
    if (cursor_pos < 80) cursor_pos = 80; // keep status bar (row 0)
    if (cursor_pos >= 80 * 25) cursor_pos = 80 * 25 - 1;
}

static void ansi_clear_region(int from, int to) {
    uint16_t blank = ((uint16_t)ansi_attr() << 8) | ' ';
    if (from < 80) from = 80;
    if (to > 80 * 25) to = 80 * 25;
    for (int i = from; i < to; i++) video_memory[i] = blank;
}

static void ansi_exec_csi(char final_cmd) {
    int p0 = (ansi_nparams >= 1) ? ansi_params[0] : 0;
    int p1 = (ansi_nparams >= 2) ? ansi_params[1] : 0;
    int row = cursor_pos / 80;
    int col = cursor_pos % 80;

    switch (final_cmd) {
    case 'A': {
        int n = p0 ? p0 : 1;
        row -= n; if (row < 1) row = 1;
        cursor_pos = row * 80 + col;
        break;
    }
    case 'B': {
        int n = p0 ? p0 : 1;
        row += n; if (row > 24) row = 24;
        cursor_pos = row * 80 + col;
        break;
    }
    case 'C': {
        int n = p0 ? p0 : 1;
        col += n; if (col > 79) col = 79;
        cursor_pos = row * 80 + col;
        break;
    }
    case 'D': {
        int n = p0 ? p0 : 1;
        col -= n; if (col < 0) col = 0;
        cursor_pos = row * 80 + col;
        break;
    }
    case 'H': case 'f': {
        int r = p0 ? p0 : 1;
        int c = p1 ? p1 : 1;
        if (r < 1) r = 1; if (r > 25) r = 25;
        if (c < 1) c = 1; if (c > 80) c = 80;
        if (r == 1) cursor_pos = 80 + (c - 1);
        else cursor_pos = (r - 1) * 80 + (c - 1);
        ansi_clamp_cursor();
        break;
    }
    case 'J': {
        if (p0 == 2 || p0 == 3) ansi_clear_region(80, 80 * 25);
        else if (p0 == 1) ansi_clear_region(80, cursor_pos + 1);
        else ansi_clear_region(cursor_pos, 80 * 25);
        break;
    }
    case 'K': {
        int line = (cursor_pos / 80) * 80;
        if (p0 == 2) ansi_clear_region(line, line + 80);
        else if (p0 == 1) ansi_clear_region(line, cursor_pos + 1);
        else ansi_clear_region(cursor_pos, line + 80);
        break;
    }
    case 'm': {
        if (ansi_nparams == 0) { ansi_fg = ansi_bg = 0xFF; break; }
        for (int i = 0; i < ansi_nparams; i++) {
            int p = ansi_params[i];
            if (p == 0) { ansi_fg = ansi_bg = 0xFF; }
            else if (p == 1) {
                if (ansi_fg != 0xFF && ansi_fg < 8) ansi_fg = (uint8_t)(ansi_fg + 8);
            }
            else if (p >= 30 && p <= 37) {
                static const uint8_t map[8] = {
                    VGA_COLOR_BLACK, VGA_COLOR_RED, VGA_COLOR_GREEN, VGA_COLOR_BROWN,
                    VGA_COLOR_BLUE, VGA_COLOR_MAGENTA, VGA_COLOR_CYAN, VGA_COLOR_LIGHT_GREY
                };
                ansi_fg = map[p - 30];
            }
            else if (p >= 40 && p <= 47) {
                static const uint8_t map[8] = {
                    VGA_COLOR_BLACK, VGA_COLOR_RED, VGA_COLOR_GREEN, VGA_COLOR_BROWN,
                    VGA_COLOR_BLUE, VGA_COLOR_MAGENTA, VGA_COLOR_CYAN, VGA_COLOR_LIGHT_GREY
                };
                ansi_bg = map[p - 40];
            }
            else if (p >= 90 && p <= 97) {
                static const uint8_t map[8] = {
                    VGA_COLOR_DARK_GREY, VGA_COLOR_LIGHT_RED, VGA_COLOR_LIGHT_GREEN, VGA_COLOR_LIGHT_BROWN,
                    VGA_COLOR_LIGHT_BLUE, VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_LIGHT_CYAN, VGA_COLOR_WHITE
                };
                ansi_fg = map[p - 90];
            }
            else if (p == 39) ansi_fg = 0xFF;
            else if (p == 49) ansi_bg = 0xFF;
        }
        break;
    }
    case 's': ansi_save_pos = cursor_pos; break;
    case 'u': cursor_pos = ansi_save_pos; ansi_clamp_cursor(); break;
    case 'h': case 'l':
        break;
    default:
        break;
    }
}

static bool ansi_feed(char c) {
    if (ansi_state == 0) {
        if ((uint8_t)c == 0x1B) { ansi_state = 1; return true; }
        return false;
    }
    if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_nparams = 0;
            ansi_in_num = 0;
            for (int i = 0; i < 8; i++) ansi_params[i] = 0;
            return true;
        }
        if (c == '7') { ansi_save_pos = cursor_pos; ansi_state = 0; return true; }
        if (c == '8') { cursor_pos = ansi_save_pos; ansi_clamp_cursor(); ansi_state = 0; return true; }
        ansi_state = 0;
        return true;
    }
    if (c >= '0' && c <= '9') {
        ansi_in_num = 1;
        if (ansi_nparams < 8)
            ansi_params[ansi_nparams] = ansi_params[ansi_nparams] * 10 + (c - '0');
        return true;
    }
    if (c == ';') {
        if (ansi_nparams < 7) ansi_nparams++;
        ansi_in_num = 0;
        if (ansi_nparams < 8) ansi_params[ansi_nparams] = 0;
        return true;
    }
    if (c == '?') return true;
    if (ansi_in_num) { if (ansi_nparams < 8) ansi_nparams++; ansi_in_num = 0; }
    ansi_exec_csi(c);
    ansi_state = 0;
    return true;
}

void print_char(char c) {
    // Если активен редирект — пишем в буфер, не на экран
    if (redirect_active) {
        if (c != '\b' && redirect_len < REDIRECT_BUF_SIZE - 1)
            redirect_buf[redirect_len++] = c;
        return;
    }

    if (ansi_feed(c)) {
        update_vga_cursor(cursor_pos % 80, cursor_pos / 80);
        return;
    }

    uint8_t attr = ansi_attr();
    uint16_t blank = (attr << 8) | ' ';

    if (c == '\n') {
        int row = cursor_pos / 80;
        cursor_pos = (row + 1) * 80;
    } else if (c == '\r') {
        cursor_pos = (cursor_pos / 80) * 80;
    } else if (c == '\t') {
        int col = cursor_pos % 80;
        int next = (col + 8) & ~7;
        if (next > 80) next = 80;
        while ((cursor_pos % 80) < next)
            video_memory[cursor_pos++] = (attr << 8) | ' ';
    } else if (c == '\b') {
        if ((cursor_pos % 80) > 0) cursor_pos--;
    } else {
        video_memory[cursor_pos++] = (attr << 8) | (uint8_t)c;
    }
    
    // ✅ ПРАВИЛЬНАЯ ПРОКРУТКА с учетом цвета
    while (cursor_pos >= 80 * 25) { 
        // Сохраняем статус-бар (строка 0)
        uint16_t statusbar_backup[80];
        for (int i = 0; i < 80; i++) {
            statusbar_backup[i] = video_memory[i];
        }

        // Сохраняем строку 24 в scrollback перед прокруткой (она уходит с экрана)
        scrollback_push((const uint16_t*)(video_memory + 24 * 80));

        // Прокручиваем строки 1-24 вверх
        for (int row = 1; row < 24; row++) {
            for (int col = 0; col < 80; col++) {
                video_memory[row * 80 + col] = video_memory[(row + 1) * 80 + col];
            }
        }
        
        // Очищаем последнюю строку (строка 24) цветом темы
        for (int i = 80 * 24; i < 80 * 25; i++) {
            video_memory[i] = blank;
        }

        // Восстанавливаем статус-бар
        for (int i = 0; i < 80; i++) {
            video_memory[i] = statusbar_backup[i];
        }
        
        cursor_pos -= 80; 
    }
    
    update_vga_cursor(cursor_pos % 80, cursor_pos / 80);
}

void print(const char* str) {
    int i=0;
    while(str[i]) print_char(str[i++]);
}

void println(const char* str) {
    print(str);
    print_char('\n');
}

char to_hex(uint8_t val) {
    if (val < 10) return val + '0';
    return val - 10 + 'A';
}

void print_hex_byte(uint8_t byte) {
    print_char(to_hex((byte >> 4) & 0x0F));
    print_char(to_hex(byte & 0x0F));
}

void set_custom_theme(uint8_t bg, uint8_t fg, uint8_t bar_bg, uint8_t bar_fg,
                      uint8_t cursor, uint8_t cursor_bg, uint8_t cursor_char,
                      uint8_t dir, uint8_t file) {
    theme_bg          = bg          & 0x0F;
    theme_fg          = fg          & 0x0F;
    theme_bar_bg      = bar_bg      & 0x0F;
    theme_bar_fg      = bar_fg      & 0x0F;
    theme_cursor      = cursor      & 0x0F;
    theme_cursor_bg   = cursor_bg   & 0x0F;
    theme_cursor_char = cursor_char & 0x0F;
    theme_dir         = dir         & 0x0F;
    theme_file        = file        & 0x0F;
}

void print_char_colored(char c, uint8_t color) {
    // Печатает символ с произвольным атрибутом (не меняет cursor_pos логику)
    if (redirect_active) { print_char(c); return; }
    if (c == '\n') { print_char(c); return; }
    uint8_t saved_fg = theme_fg;
    // Временно меняем fg чтобы переиспользовать прокрутку из print_char
    // Проще: напрямую пишем в видеопамять с нужным цветом
    if (cursor_pos < 80 * 25) {
        video_memory[cursor_pos++] = ((uint16_t)color << 8) | (uint8_t)c;
        update_vga_cursor(cursor_pos % 80, cursor_pos / 80);
    }
}

void draw_mouse_cursor(int x, int y, uint8_t color) {
    // Импортируем графические функции если они доступны
    extern bool vga_gfx_active();
    extern void gfx_fill_rect(int, int, int, int, uint8_t);

    if (vga_gfx_active()) {
        // В графическом режиме рисуем квадрат 16x16
        gfx_fill_rect(x, y, 16, 16, color);
    } else {
        // Статические переменные для сохранения предыдущей позиции и символа
        static int      last_cx = -1, last_cy = -1;
        static uint16_t saved_char = 0;

        // Восстанавливаем символ на старой позиции курсора
        if (last_cx >= 0 && last_cx < 80 && last_cy >= 1 && last_cy < 25) {
            video_memory[last_cy * 80 + last_cx] = saved_char;
        }

        // Рисуем курсор на новой позиции
        if (x >= 0 && x < 80 && y >= 1 && y < 25) {
            int pos = y * 80 + x;
            saved_char = video_memory[pos];         // сохраняем то что было
            uint8_t attr = (theme_bg << 4) | color;
            video_memory[pos] = (attr << 8) | 219; // 219 = ███
            last_cx = x;
            last_cy = y;
        } else {
            last_cx = -1;
            last_cy = -1;
        }
    }
}