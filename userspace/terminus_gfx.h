/*
 * terminus_gfx.h — userspace bindings for Terminus OS VGA graphics syscalls.
 *
 * Compatible with musl (i386) and any libc that provides syscall().
 * Compile example:
 *   i686-linux-musl-gcc -static -o demo.elf demo.c
 *   (or your cross-compiler that produces i386 ELF)
 *
 * Usage:
 *   #include "terminus_gfx.h"
 *   gfx_enter_mode13();
 *   gfx_clear(0);
 *   gfx_draw_text(10, 10, "Hello", 15, 0);
 *   gfx_flip();
 *   ...
 *   gfx_exit_text();
 */

#ifndef TERMINUS_GFX_H
#define TERMINUS_GFX_H

#include <stdint.h>
#include <unistd.h>   /* syscall() — musl / glibc */
#include <sys/syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Syscall numbers (must match kernel/syscall.h) ─────────────────────────── */
#define SYS_GFX_ENTER_MODE13      1000
#define SYS_GFX_ENTER_640x480     1001
#define SYS_GFX_EXIT_TEXT         1002
#define SYS_GFX_GET_WIDTH         1003
#define SYS_GFX_GET_HEIGHT        1004
#define SYS_GFX_SET_PALETTE       1005
#define SYS_GFX_INIT_PALETTE      1006
#define SYS_GFX_PUT_PIXEL         1007
#define SYS_GFX_CLEAR             1008
#define SYS_GFX_FILL_RECT         1009
#define SYS_GFX_DRAW_RECT         1010
#define SYS_GFX_DRAW_HLINE        1011
#define SYS_GFX_DRAW_VLINE        1012
#define SYS_GFX_DRAW_LINE         1013
#define SYS_GFX_FLIP              1014
#define SYS_GFX_SET_CLIP          1015
#define SYS_GFX_CLEAR_CLIP        1016
#define SYS_GFX_DRAW_CHAR         1017
#define SYS_GFX_DRAW_TEXT         1018
#define SYS_GFX_CURSOR_SHOW       1019
#define SYS_GFX_CURSOR_HIDE       1020
#define SYS_GFX_CURSOR_MOVE       1021
#define SYS_GFX_CURSOR_GET_X      1022
#define SYS_GFX_CURSOR_GET_Y      1023
#define SYS_GFX_CURSOR_UPDATE_POS  1024
#define SYS_GFX_ACTIVE            1025
#define SYS_GFX_CURSOR_SET_LOCKED 1026
#define SYS_GFX_LFB_AVAILABLE     1027

#define SYS_KB_GETKEY             1028
#define SYS_KB_GET_STATE           1035
#define SYS_YIELD                 1036
#define SYS_GFX_GET_BACKBUFFER    1029
#define SYS_MOUSE_GET_X           1030
#define SYS_MOUSE_GET_Y           1031
#define SYS_MOUSE_GET_BUTTONS     1032
#define SYS_MOUSE_GET_DX          1033
#define SYS_MOUSE_GET_DY          1034

/* ── Standard 16 VGA colour indices ────────────────────────────────────────── */
#define GFX_BLACK          0
#define GFX_BLUE           1
#define GFX_GREEN          2
#define GFX_CYAN           3
#define GFX_RED            4
#define GFX_MAGENTA        5
#define GFX_BROWN          6
#define GFX_LIGHT_GRAY     7
#define GFX_DARK_GRAY      8
#define GFX_LIGHT_BLUE     9
#define GFX_LIGHT_GREEN   10
#define GFX_LIGHT_CYAN    11
#define GFX_LIGHT_RED     12
#define GFX_LIGHT_MAGENTA 13
#define GFX_YELLOW        14
#define GFX_WHITE         15
#define GFX_TRANSPARENT  255

/* ── Inline wrappers (zero overhead, musl-friendly) ────────────────────────── */

static inline long gfx_enter_mode13(void) {
    return syscall(SYS_GFX_ENTER_MODE13);
}
static inline long gfx_enter_640x480(void) {
    return syscall(SYS_GFX_ENTER_640x480);
}
static inline long gfx_exit_text(void) {
    return syscall(SYS_GFX_EXIT_TEXT);
}
static inline int gfx_get_width(void) {
    return (int)syscall(SYS_GFX_GET_WIDTH);
}
static inline int gfx_get_height(void) {
    return (int)syscall(SYS_GFX_GET_HEIGHT);
}
static inline long gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    return syscall(SYS_GFX_SET_PALETTE, idx, r, g, b);
}
static inline long gfx_init_palette(void) {
    return syscall(SYS_GFX_INIT_PALETTE);
}
static inline long gfx_put_pixel(int x, int y, uint8_t color) {
    return syscall(SYS_GFX_PUT_PIXEL, x, y, color);
}
static inline long gfx_clear(uint8_t color) {
    return syscall(SYS_GFX_CLEAR, color);
}
static inline long gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    return syscall(SYS_GFX_FILL_RECT, x, y, w, h, color);
}
static inline long gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    return syscall(SYS_GFX_DRAW_RECT, x, y, w, h, color);
}
static inline long gfx_draw_hline(int x, int y, int len, uint8_t color) {
    return syscall(SYS_GFX_DRAW_HLINE, x, y, len, color);
}
static inline long gfx_draw_vline(int x, int y, int len, uint8_t color) {
    return syscall(SYS_GFX_DRAW_VLINE, x, y, len, color);
}
static inline long gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color) {
    return syscall(SYS_GFX_DRAW_LINE, x0, y0, x1, y1, color);
}
static inline long gfx_flip(void) {
    return syscall(SYS_GFX_FLIP);
}
static inline long gfx_set_clip(int x, int y, int w, int h) {
    return syscall(SYS_GFX_SET_CLIP, x, y, w, h);
}
static inline long gfx_clear_clip(void) {
    return syscall(SYS_GFX_CLEAR_CLIP);
}
static inline long gfx_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    return syscall(SYS_GFX_DRAW_CHAR, x, y, (long)(unsigned char)c, fg, bg);
}
static inline long gfx_draw_text(int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    return syscall(SYS_GFX_DRAW_TEXT, x, y, (long)str, fg, bg);
}
static inline long gfx_cursor_show(void) {
    return syscall(SYS_GFX_CURSOR_SHOW);
}
static inline long gfx_cursor_hide(void) {
    return syscall(SYS_GFX_CURSOR_HIDE);
}
static inline long gfx_cursor_move(int x, int y) {
    return syscall(SYS_GFX_CURSOR_MOVE, x, y);
}
static inline int gfx_cursor_get_x(void) {
    return (int)syscall(SYS_GFX_CURSOR_GET_X);
}
static inline int gfx_cursor_get_y(void) {
    return (int)syscall(SYS_GFX_CURSOR_GET_Y);
}
static inline long gfx_cursor_update_pos(void) {
    return syscall(SYS_GFX_CURSOR_UPDATE_POS);
}
static inline int gfx_active(void) {
    return (int)syscall(SYS_GFX_ACTIVE);
}
static inline long gfx_cursor_set_locked(int locked) {
    return syscall(SYS_GFX_CURSOR_SET_LOCKED, locked);
}
static inline int gfx_lfb_available(void) {
    return (int)syscall(SYS_GFX_LFB_AVAILABLE);
}

/* ── Keyboard (non-blocking) + Mouse ───────────────────────────────────────── */
/*
 * musl's syscall() treats returns in [-4095,-1] as -errno.
 * Mouse deltas are often negative → they become -1 (left turn "broken").
 * Use raw int $0x80 so the value is returned unchanged.
 */
static inline long __terminus_syscall0(long n) {
    long ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static inline int kb_getkey(void) {
    return (int)__terminus_syscall0(SYS_KB_GETKEY);   /* 0 if no key */
}
/* bits: 0=W 1=S 2=A 3=D 4=Q 5=E 6=Left 7=Right 8=Up 9=Down 10=X 11=Esc */
static inline unsigned kb_get_state(void) {
    return (unsigned)__terminus_syscall0(SYS_KB_GET_STATE);
}
static inline void sys_yield(void) {
    (void)__terminus_syscall0(SYS_YIELD);
}
#define KB_W       (1u << 0)
#define KB_S       (1u << 1)
#define KB_A       (1u << 2)
#define KB_D       (1u << 3)
#define KB_Q       (1u << 4)
#define KB_E       (1u << 5)
#define KB_LEFT    (1u << 6)
#define KB_RIGHT   (1u << 7)
#define KB_UP      (1u << 8)
#define KB_DOWN    (1u << 9)
#define KB_X       (1u << 10)
#define KB_ESC     (1u << 11)

static inline int mouse_get_x(void) {
    return (int)__terminus_syscall0(SYS_MOUSE_GET_X);
}
static inline int mouse_get_y(void) {
    return (int)__terminus_syscall0(SYS_MOUSE_GET_Y);
}
static inline int mouse_get_buttons(void) {
    return (int)__terminus_syscall0(SYS_MOUSE_GET_BUTTONS);
}
static inline int mouse_get_dx(void) {
    return (int)__terminus_syscall0(SYS_MOUSE_GET_DX); /* consumes delta */
}
static inline int mouse_get_dy(void) {
    return (int)__terminus_syscall0(SYS_MOUSE_GET_DY); /* consumes delta */
}
static inline uint8_t *gfx_get_backbuffer(void) {
    return (uint8_t *)(long)__terminus_syscall0(SYS_GFX_GET_BACKBUFFER);
}

#ifdef __cplusplus
}
#endif

#endif /* TERMINUS_GFX_H */
