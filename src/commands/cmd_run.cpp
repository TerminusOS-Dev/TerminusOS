#include "cmd_run.h"
#include "../kernel/elf_loader.h"
#include "../lib/screen.h"

// run [-d] <elf-file> [args...]
void cmd_run(char* args) {
    if (!args) args = (char*)"";

    bool debug = false;
    const char* p = args;
    while (*p == ' ') p++;

    if (p[0] == '-' && p[1] == 'd' && (p[2] == '\0' || p[2] == ' ')) {
        debug = true;
        p += 2;
        while (*p == ' ') p++;
    }

    if (*p == '\0') {
        println("Usage: run [-d] <elf-file> [args...]");
        return;
    }

    // Имя ELF — до пробела
    char filename[128];
    int n = 0;
    while (p[n] && p[n] != ' ' && n < 127) {
        filename[n] = p[n];
        n++;
    }
    filename[n] = '\0';

    // Остаток строки — аргументы программы
    const char* tail = p + n;
    while (*tail == ' ') tail++;
    if (!*tail) tail = nullptr;

    int code = elf_run(filename, debug, tail);
    print_char('\n');
    if (debug) {
        print("Program exited with code ");
        int c = code;
        if (c < 0) { print_char('-'); c = -c; }
        if (c == 0) print_char('0');
        else {
            char buf[12]; int i = 0;
            while (c > 0) { buf[i++] = (char)('0' + c % 10); c /= 10; }
            while (i--) print_char(buf[i]);
        }
        println("");
    }
}
