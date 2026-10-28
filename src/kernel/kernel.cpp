#include <stdint.h>
#include <stdbool.h>
#include "../lib/screen.h"
#include "keyboard.h"
#include "../shell/shell.h"
#include "../drivers/fat32.h"
#include "../drivers/atapi.h"
#include "../drivers/iso9660.h"
#include "../drivers/disk.h"
#include "../drivers/rtc.h"
#include "../lib/config.h"
#include "../commands/cmd_uptime.h"
#include "../drivers/vga_gfx.h"
#include "../drivers/serial.h"
#include "interrupts.h"
#include "gdt.h"
#include "mouse.h"
#include "../lib/clipboard.h"
#include "../lib/heap.h"
#include "syscall.h"
#include "elf_loader.h"
#include "version.h"
extern "C" {
    void panic(const char* message);
}

void sleep_seconds(int seconds) {
    for (int i = 0; i < seconds; i++) {
        uint8_t start_sec = rtc_read_time().second;
        while (rtc_read_time().second == start_sec) {
            asm volatile("pause");
        }
    }
}

// ─── Multiboot Info Structure (только нужные поля) ───────────────────────────
struct __attribute__((packed)) MultibootInfo {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
};

extern "C" void kmain(MultibootInfo* mbi) {
    // ─── DEBUG: serial-консоль (COM1, 0x3F8) — видна и в текстовом, и в GUI режиме ───
    serial_init();
    serial_write("\n[BOOT] serial up\n");

    gdt_init();
    serial_write("[BOOT] gdt up (extended w/ TLS slots)\n");

    // ─── ВАЖНО: init_idt() раньше нигде не вызывался -> IDT/PIC не настроены,
    // sti не выполнялся, IRQ12 (мышь) был мёртвым кодом. Вызываем ДО mouse_init().
    init_idt();
    serial_write("[BOOT] IDT+PIC done\n");

    // PIT @ TIMER_HZ — нужен для clock()/gettimeofday/clock_gettime (linpack и др.)
    timer_init();
    serial_write("[BOOT] timer up\n");

    heap_init();
    serial_write("[BOOT] heap up\n");

    syscall_init();
    serial_write("[BOOT] syscalls (int 0x80) up\n");

    if (mbi && (mbi->flags & 0x1)) {
        elf_set_mem_upper_kb(mbi->mem_upper);
        serial_write("[BOOT] mem_upper set\n");
    }

    // Инициализация FAT
    // ─── Инициализация LFB из Multiboot ──────────────────────────────────────
    // flags bit 12 = framebuffer info присутствует
    if (mbi && (mbi->flags & (1 << 12)) && mbi->framebuffer_type == 1) {
        vga_lfb_init(
            (uint32_t)mbi->framebuffer_addr,
            mbi->framebuffer_width,
            mbi->framebuffer_height,
            mbi->framebuffer_pitch,
            mbi->framebuffer_bpp
        );
    }

    // GRUB мог оставить железо в VESA/LFB-режиме — явно переключаем в текстовый
    vga_init_text_mode();
    ata_init();
    fat32_init();
    if (atapi_detect()) {
        if (atapi_check_disk()) {
            // Монтируем ISO 9660 — создаём виртуальную папку /cdrom
            if (iso_mount()) {
                println("[CDROM] Disk detected and mounted as /cdrom");
            } else {
                println("[CDROM] Drive found, but no readable ISO 9660 disk");
            }
        } else {
            println("[CDROM] Drive found, no disk inserted");
        }
    }
    rtc_init();
    uptime_init();
    mouse_init();

    config_load();
    shell_load_history_file();
    clipboard_load();
    // Очищаем экран
    for(int i=0;i<80*25;i++) video_memory[i]=0x0F00;

    uint8_t attr = get_theme_color();
    uint16_t blank = (attr << 8) | ' ';
    for(int i=0; i<80*25; i++) video_memory[i]=blank;

    // Рисуем статус-бар
    shell_update_time();
    shell_draw_status_bar();
    // Выводим приветствие
    cursor_pos = 80;
    println(OS_WELCOME_MSG);
    println("Type 'help' for commands.");
    
    shell_print_prompt();
    shell_init_cursor();

    static int tick_counter = 0;
    
    while(1){
        uint8_t c = get_key();
        if(++tick_counter > 50) {
            shell_update_time();
            tick_counter = 0;
        }
        
        if(c==0) continue;
        switch(c) {
            case CHAR_PGUP:
                shell_handle_pgup();
                break;
            case CHAR_PGDN:
                shell_handle_pgdn();
                break;
            case CHAR_WIN_V:
                shell_clipboard_popup();
                break;
            case '\n':
                if (shell_is_in_scrollback()) { shell_exit_scrollback(); break; }
                shell_sel_collapse(true);      // убрать подсветку выделения перед выполнением
                tab_reset();
                history_index = -1;
                print_char('\n');
                process_command();
                shell_print_prompt();
                shell_init_cursor();
                break;

            case CHAR_TAB:
                shell_handle_tab(false);
                break;

            case CHAR_SHIFT_TAB:
                shell_handle_tab(true);
                break;

            case CHAR_DEL:
                shell_delete_forward();
                break;

            case CHAR_CTRL_C:
                // Есть выделение — копируем его в буфер обмена, строку не трогаем.
                // Нет выделения — как раньше: отмена текущей строки.
                if (shell_copy_selection()) break;
                shell_sel_clear();
                buf_len = 0;
                cursor_offset = 0;
                buffer[0] = 0;
                tab_reset();
                print_char('\n');
                shell_print_prompt();
                shell_init_cursor();
                break;

            case CHAR_CTRL_A:
            case CHAR_HOME:
                shell_sel_clear();
                cursor_offset = 0;
                shell_redraw();
                break;

            case CHAR_CTRL_E:
            case CHAR_END:
                shell_sel_clear();
                cursor_offset = buf_len;
                shell_redraw();
                break;

                
            case CHAR_CTRL_X:
                shell_cut_selection();
                break;

            case CHAR_CTRL_V:
                if (shell_is_in_scrollback()) shell_exit_scrollback();
                shell_paste_latest();
                break;

            case CHAR_CTRL_K:
                shell_copy_line();
                break;

            case CHAR_CTRL_SHIFT_A:
                shell_select_all();
                break;

            case CHAR_SHIFT_LEFT:
                shell_select_extend(0);
                break;

            case CHAR_CTRL_SHIFT_LEFT:
                shell_select_extend(2);
                break;

            case CHAR_CTRL_SHIFT_RIGHT:
                shell_select_extend(3);
                break;

            case '\b':
                shell_delete_char();
                break;
                
            case CHAR_ARROW_LEFT:
                if (shell_sel_collapse(false)) break;
                if (cursor_offset > 0) cursor_offset--;
                shell_redraw();
                break;

            case CHAR_CTRL_LEFT:
                shell_word_left();
                break;

            case CHAR_CTRL_RIGHT:
                shell_word_right();
                break;

            case CHAR_CTRL_BACKSPACE:
                shell_delete_word_left();
                break;
                
            case CHAR_ARROW_RIGHT:
                if (shell_sel_collapse(true)) break;
                if (cursor_offset < buf_len) {
                    cursor_offset++;
                    shell_redraw();
                } else {
                    shell_tab_accept();
                }
                break;

            case CHAR_SHIFT_RIGHT:
                if (shell_has_suggestion()) {
                    shell_tab_accept_one();
                } else {
                    shell_select_extend(1);
                }
                break;
                
            case CHAR_ARROW_UP:
                if (history_index == -1) {
                    if (history_count > 0) {
                        shell_load_history(history_count - 1);
                    }
                } else if (history_index > 0) {
                    // Идем к более старым командам
                    shell_load_history(history_index - 1);
                }
                break;
                
            case CHAR_ARROW_DOWN:
                if (history_index != -1 && history_index < history_count - 1) {
                    shell_load_history(history_index + 1);
                } else if (history_index == history_count - 1) {
                    shell_load_history(-1);
                }
                break;
                
            default:
                if (c >= 32 && c < 128) {
                    if (shell_is_in_scrollback()) shell_exit_scrollback();
                    shell_insert_char(c);
                }
                break;
        }
    }
}