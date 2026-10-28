#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include <stdbool.h>

// filename  — путь к ELF
// debug     — подробный лог
// args_tail — аргументы после имени файла (может быть NULL), напр. "hist.dat foo"
//             разбиваются по пробелам в argv[1..]
int elf_run(const char* filename, bool debug, const char* args_tail);
extern bool g_elf_debug; // трассировка сисколов в syscall.cpp читает этот же флаг

extern "C" void __attribute__((noreturn)) elf_request_exit(int code);
extern "C" void elf_set_mem_upper_kb(uint32_t kb);

// true с момента прыжка в программу и до возврата (exit/exit_group).
// Используется в interrupts.cpp (IRQ1) — чтобы жёстко ловить Ctrl+C даже
// если программа зависла в чистом вычислительном цикле без единого
// сискола, и НЕ трогать это вне запущенной программы (обычный ввод в
// шелле/gite не должен меняться).
extern bool g_elf_running;

#endif
