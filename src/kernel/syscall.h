#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

void syscall_init();

// Linux i386: до 6 аргументов (ebx..ebp). Возврат в eax (отрицательное = -errno).
extern "C" uint32_t syscall_dispatch(uint32_t num,
                                     uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t a4, uint32_t a5, uint32_t a6);

// ─── Номера = Linux i386 ─────────────────────────────────────────────────────
#define SYS_EXIT            1
#define SYS_FORK            2
#define SYS_READ            3
#define SYS_WRITE           4
#define SYS_OPEN            5
#define SYS_CLOSE           6
#define SYS_WAITPID         7
#define SYS_LINK            9
#define SYS_UNLINK          10
#define SYS_EXECVE          11
#define SYS_CHDIR           12
#define SYS_LSEEK           19
#define SYS_GETPID          20
#define SYS_SETUID          23
#define SYS_GETUID          24
#define SYS_PTRACE          26
#define SYS_ACCESS          33
#define SYS_KILL            37
#define SYS_RENAME          38
#define SYS_MKDIR           39
#define SYS_RMDIR           40
#define SYS_DUP             41
#define SYS_PIPE            42
#define SYS_BRK             45
#define SYS_SETGID          46
#define SYS_GETGID          47
#define SYS_GETEUID         49
#define SYS_GETEGID         50
#define SYS_IOCTL           54
#define SYS_FCNTL           55
#define SYS_DUP2            63
#define SYS_GETPPID         64
#define SYS_SETSID          66
#define SYS_SETREUID        70
#define SYS_SETREGID        71
#define SYS_GETTIMEOFDAY    78
#define SYS_SELECT          142
#define SYS_POLL            168
#define SYS_SETFSUID        138
#define SYS_SETFSGID        139
#define SYS_SETRESUID       164
#define SYS_SETRESGID       170
#define SYS_UMASK           60
/* i386 32-bit uid/gid variants (static busybox often uses these) */
#define SYS_GETUID32        199
#define SYS_GETGID32        200
#define SYS_GETEUID32       201
#define SYS_GETEGID32       202
#define SYS_SETREUID32      203
#define SYS_SETREGID32      204
#define SYS_SETRESUID32     208
#define SYS_SETRESGID32     210
#define SYS_SETUID32        213
#define SYS_SETGID32        214
#define SYS_MUNMAP          91
#define SYS_SOCKETCALL      102
#define SYS_WAIT4           114
#define SYS_CLONE           120
#define SYS_UNAME           122
#define SYS_MPROTECT        125
#define SYS_GETCWD          183
#define SYS_MMAP2           192
#define SYS_STAT64          195
#define SYS_FSTAT64         197
#define SYS_GETDENTS64      220
#define SYS_FCNTL64         221
#define SYS_GETTID          224
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP      252
#define SYS_CLOCK_GETTIME   265
#define SYS_CLOCK_GETTIME64 403   /* musl prefers this; struct timespec64 */
#define SYS_OPENAT          295
#define SYS_WRITEV          146
#define SYS_READV           145
#define SYS_LLSEEK          140
#define SYS_UGETRLIMIT      191
#define SYS_SET_TID_ADDRESS 258
#define SYS_FUTEX           240
#define SYS_NANOSLEEP       162

/* Extra stubs needed by modern musl CRT (otherwise -ENOSYS can hang startup) */
#define SYS_READLINKAT       305
#define SYS_SET_ROBUST_LIST  311
#define SYS_GET_ROBUST_LIST  312
#define SYS_GETRANDOM        355
#define SYS_STATX            383

// ─── Terminus graphics (vga_gfx) — custom, musl-friendly via syscall() ────────
// Numbers start at 1000 to stay clear of Linux i386 range.
#define SYS_GFX_ENTER_MODE13      1000
#define SYS_GFX_ENTER_640x480     1001
#define SYS_GFX_EXIT_TEXT         1002
#define SYS_GFX_GET_WIDTH         1003
#define SYS_GFX_GET_HEIGHT        1004
#define SYS_GFX_SET_PALETTE       1005  /* idx, r, g, b  (0-63) */
#define SYS_GFX_INIT_PALETTE      1006
#define SYS_GFX_PUT_PIXEL         1007  /* x, y, color */
#define SYS_GFX_CLEAR             1008  /* color */
#define SYS_GFX_FILL_RECT         1009  /* x, y, w, h, color */
#define SYS_GFX_DRAW_RECT         1010  /* x, y, w, h, color */
#define SYS_GFX_DRAW_HLINE        1011  /* x, y, len, color */
#define SYS_GFX_DRAW_VLINE        1012  /* x, y, len, color */
#define SYS_GFX_DRAW_LINE         1013  /* x0, y0, x1, y1, color */
#define SYS_GFX_FLIP              1014
#define SYS_GFX_SET_CLIP          1015  /* x, y, w, h */
#define SYS_GFX_CLEAR_CLIP        1016
#define SYS_GFX_DRAW_CHAR         1017  /* x, y, c, fg, bg */
#define SYS_GFX_DRAW_TEXT         1018  /* x, y, str_ptr, fg, bg */
#define SYS_GFX_CURSOR_SHOW       1019
#define SYS_GFX_CURSOR_HIDE       1020
#define SYS_GFX_CURSOR_MOVE       1021  /* x, y */
#define SYS_GFX_CURSOR_GET_X      1022
#define SYS_GFX_CURSOR_GET_Y      1023
#define SYS_GFX_CURSOR_UPDATE_POS  1024
#define SYS_GFX_ACTIVE            1025
#define SYS_GFX_CURSOR_SET_LOCKED 1026  /* locked (bool) */
#define SYS_GFX_LFB_AVAILABLE     1027

// Keyboard (non-blocking) + mouse + backbuffer
#define SYS_KB_GETKEY             1028  /* → char or 0 if none */
#define SYS_KB_GET_STATE           1035  /* → bitmask of held keys */
#define SYS_YIELD                 1036  /* sti; hlt — wait for next IRQ */
#define SYS_GFX_GET_BACKBUFFER    1029  /* → ptr to kernel backbuffer (direct write) */
#define SYS_MOUSE_GET_X           1030
#define SYS_MOUSE_GET_Y           1031
#define SYS_MOUSE_GET_BUTTONS     1032  /* bit0=L bit1=R bit2=M */
#define SYS_MOUSE_GET_DX          1033  /* raw dx since last read, then cleared */
#define SYS_MOUSE_GET_DY          1034  /* raw dy since last read, then cleared */

#define ALLIVE_TEST               1060

// Вызывается из elf_run перед стартом программы: текущий program break
// = конец последнего PT_LOAD (page-aligned).
void syscall_set_program_break(uint32_t brk);
void syscall_reset_fds();
// Верхняя граница ОЗУ, доступной ТЕКУЩЕЙ программе (brk-куча и mmap-арена
// делят всё, что выше ELF_LOAD_BASE = 64 МБ, до этого потолка). Вызывается
// из elf_run() перед каждым запуском — mem_end берётся из данных загрузчика
// (BIOS int 15h), т.е. отражает реальный объём ОЗУ машины/QEMU.
void syscall_set_mem_ceiling(uint32_t mem_end);

#endif