#ifndef SHELL_H
#define SHELL_H

#include "../commands/cmd_nano.h"
#include <stdint.h>

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

extern uint8_t theme_bg;
extern uint8_t theme_fg;
extern uint8_t theme_bar_bg;
extern uint8_t theme_bar_fg;

void set_theme(const char* name);
uint8_t get_theme_color();

#define BUF_SIZE 256
#define HISTORY_SIZE 10
#define STATUS_BAR_ROW 0  // Верхняя строка для GUI

extern char buffer[BUF_SIZE];
extern int buf_len;
extern int cursor_offset;
extern char current_path[128];
extern char history[HISTORY_SIZE][BUF_SIZE];
extern int history_count;
extern int history_index;
extern int last_cursor_x;
extern int last_cursor_y;

// GUI Functions
void shell_draw_status_bar();  // Рисует верхнюю панель
void shell_update_time();       // Обновляет время справа
void shell_update_command();    // Обновляет команду слева

// Shell Functions
void shell_main();
void shell_print_prompt();
void shell_clear_line();
void shell_redraw();
void shell_insert_char(char c);
void shell_delete_char();
void shell_add_to_history(const char* cmd);
void shell_load_history(int index);
void shell_save_history_file();
void shell_load_history_file();
void process_command();
void shell_init_cursor();
void shell_handle_tab(bool reverse);
void shell_tab_accept();
void shell_tab_accept_one();
bool shell_has_suggestion();
void tab_reset();
void shell_handle_pgup();
void shell_handle_pgdn();
void shell_exit_scrollback();
bool shell_is_in_scrollback();
void shell_redraw_full();
void shell_clear_buffer();
void shell_clipboard_popup();
void shell_word_left();
void shell_word_right();
void shell_delete_word_left();
void shell_delete_forward();

// Выделение / буфер обмена в строке ввода (общая история — lib/clipboard.h)
void shell_sel_clear();
bool shell_sel_collapse(bool to_end);   // стрелка при выделении: true — выделение было
void shell_select_extend(int kind);     // 0=Shift+←  1=Shift+→  2=Ctrl+Shift+←  3=Ctrl+Shift+→
void shell_select_all();
bool shell_copy_selection();            // Ctrl+C (false — выделения нет)
bool shell_cut_selection();             // Ctrl+X (false — выделения нет)
void shell_copy_line();                 // Ctrl+K
void shell_paste_text(const char* txt);
void shell_paste_latest();              // Ctrl+V
int  shell_clipboard_pick();            // окно истории: индекс записи или -1 (для nano)
#endif