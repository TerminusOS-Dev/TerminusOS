#include "elf_loader.h"
#include "syscall.h"
#include "elf.h"
#include "../lib/screen.h"
#include "../lib/heap.h"
#include "../drivers/fat32.h"

extern "C" uint8_t _kernel_end[];

// 64 МБ — граница "ОС / программы". Всё ниже принадлежит ядру (код, данные,
// 16-МБ статическая куча ядра из heap.cpp). Всё, что выше и до реального
// потолка ОЗУ (mem_end, см. syscall_set_mem_ceiling ниже), отдаётся текущей
// программе целиком — под сам ELF-образ, под её brk-кучу и под mmap.
#define ELF_LOAD_BASE      0x4000000u
#define ELF_SAFETY_MARGIN  0x100000u
#define ELF_MAX_FILE_SIZE  (32u * 1024 * 1024)
#define ELF_STACK_SIZE     (4u * 1024 * 1024) // 4 МБ — подняли с 1 МБ при разборе glibc-совместимости
                                               // (glibc-шный статический старт заметно тяжелее musl:
                                               //  свой TCB/TLS-блок, больше служебных данных на стеке)

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_PLATFORM 15
#define AT_SECURE   23
#define AT_RANDOM   25

static uint32_t g_mem_upper_kb = 0;
extern "C" void elf_set_mem_upper_kb(uint32_t kb) { g_mem_upper_kb = kb; }

static uint32_t mem_end_bytes() {
    if (g_mem_upper_kb == 0) return 0;
    return 0x100000u + g_mem_upper_kb * 1024u;
}

// ─── setjmp/longjmp для возврата из SYS_EXIT ─────────────────────────────────
// __builtin_setjmp сохраняет ESP/EBP/EIP и callee-saved — в отличие от ручного
// jmp по &&label, который с -O2 и из interrupt-контекста давал петлю.
typedef void* elf_jmp_buf[8];
static elf_jmp_buf g_jmp;
static int         g_exit_code = 0;
static uint32_t    g_saved_cwd_cluster = 0; // cwd шелла до запуска программы — см. elf_run()
static uint8_t     g_stack_storage[ELF_STACK_SIZE];

extern "C" void __attribute__((noreturn)) elf_request_exit(int code) {
    g_exit_code = code;
    g_elf_running = false; // so IRQ1 Ctrl+C logic won't fire during unwind
    __asm__ volatile ("cli");
    __builtin_longjmp(g_jmp, 1);
}

static uint32_t g_jump_entry = 0;
static uint32_t g_jump_esp   = 0;

static void __attribute__((noreturn, noinline)) jump_to_entry(uint32_t new_esp, uint32_t entry) {
    g_jump_esp   = new_esp;
    g_jump_entry = entry;
    __asm__ __volatile__(
        "cli\n\t"
        "movl %0, %%esp\n\t"
        "movl %1, %%eax\n\t"
        "sti\n\t"          /* user programs need IRQs (mouse, keyboard) */
        "jmp *%%eax\n\t"
        :
        : "m"(g_jump_esp), "m"(g_jump_entry)
        : "eax", "memory"
    );
    __builtin_unreachable();
}


// Печать только в debug-режиме (ошибки грузятся всегда через println напрямую)
bool g_elf_debug = false; // не static — syscall.cpp тоже трассирует под -d (см. elf_loader.h)
bool g_elf_running = false; // не static — interrupts.cpp (IRQ1) проверяет для жёсткого Ctrl+C
static void dprint(const char* s) { if (g_elf_debug) print(s); }
static void dprintln(const char* s) { if (g_elf_debug) println(s); }
static void print_hex_always(uint32_t v) {
    print("0x");
    for (int i = 7; i >= 0; i--) {
        uint8_t n = (v >> (i * 4)) & 0xF;
        print_char(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}

static void dprint_hex(uint32_t v) {
    if (!g_elf_debug) return;
    print("0x");
    for (int i = 7; i >= 0; i--) {
        uint8_t n = (v >> (i * 4)) & 0xF;
        print_char(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}

int elf_run(const char* filename, bool debug, const char* args_tail) {
    g_elf_debug = debug;
    dprintln("elf: looking up file...");
    FAT32_FindResult res = fat32_find_entry(filename, 0x00);
    if (!res.found) { println("elf: file not found"); return -1; }

    uint32_t size = res.entry.file_size;
    if (size < sizeof(Elf32_Ehdr) || size > ELF_MAX_FILE_SIZE) {
        println("elf: bad file size"); return -1;
    }

    uint8_t* filebuf = (uint8_t*)kmalloc(size);
    if (!filebuf) { println("elf: out of memory reading file"); return -1; }

    uint32_t cluster = FAT32_GET_CLUSTER(&res.entry);
    uint32_t got = fat32_read_file_to_buffer(cluster, size, filebuf, size);
    if (got < sizeof(Elf32_Ehdr)) {
        println("elf: short read from disk");
        kfree(filebuf);
        return -1;
    }

    Elf32_Ehdr* eh = (Elf32_Ehdr*)filebuf;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F')) {
        println("elf: not an ELF file"); kfree(filebuf); return -1;
    }
    if (eh->e_ident[4] != 1) { println("elf: not 32-bit"); kfree(filebuf); return -1; }
    if (eh->e_type != ET_EXEC) {
        println("elf: not ET_EXEC (rebuild without -fPIE)");
        kfree(filebuf); return -1;
    }
    if (eh->e_machine != EM_386) { println("elf: need i386"); kfree(filebuf); return -1; }

    uint32_t kern_end = (uint32_t)_kernel_end;
    if (g_elf_debug) { print("elf: _kernel_end="); dprint_hex(kern_end); println(""); }
    if (g_elf_debug) { print("elf: LOAD_BASE  ="); dprint_hex(ELF_LOAD_BASE); println(""); }

    if (ELF_LOAD_BASE < kern_end + ELF_SAFETY_MARGIN) {
        println("elf: INTERNAL: LOAD_BASE overlaps kernel");
        kfree(filebuf); return -1;
    }

    uint32_t mend = mem_end_bytes();
    if (mend != 0) {
        if (g_elf_debug) { print("elf: mem_end    ="); dprint_hex(mend); println(""); }
        if (ELF_LOAD_BASE + 0x200000u > mend) {
            println("elf: not enough RAM (need qemu -m 64+)");
            kfree(filebuf); return -1;
        }
    }
    // Отдаём всё выше 64 МБ (ELF_LOAD_BASE) текущей программе: brk-куча и
    // mmap-арена в syscall.cpp сами поделят этот диапазон динамически,
    // вплоть до mend (реальный потолок ОЗУ; 0, если загрузчик его не
    // определил — тогда используется безопасный запасной вариант).
    syscall_set_mem_ceiling(mend);

    if (eh->e_phoff + (uint32_t)eh->e_phnum * sizeof(Elf32_Phdr) > size) {
        println("elf: bad phdr table"); kfree(filebuf); return -1;
    }

    Elf32_Phdr* ph = (Elf32_Phdr*)(filebuf + eh->e_phoff);
    uint32_t entry = eh->e_entry;
    int loaded = 0;
    uint32_t max_seg_end = 0;

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;

        if (ph[i].p_vaddr < ELF_LOAD_BASE) {
            print("elf: segment VA "); print_hex_always(ph[i].p_vaddr);
            println(" below LOAD_BASE — rebuild with user.ld");
            kfree(filebuf); return -1;
        }
        if (ph[i].p_offset + ph[i].p_filesz > size) {
            println("elf: segment past end of file"); kfree(filebuf); return -1;
        }
        if (mend && ph[i].p_vaddr + ph[i].p_memsz > mend) {
            println("elf: segment past end of RAM"); kfree(filebuf); return -1;
        }

        if (g_elf_debug) { print("elf: map VA="); dprint_hex(ph[i].p_vaddr); print(" sz="); dprint_hex(ph[i].p_filesz); println(""); }

        uint8_t* dst = (uint8_t*)ph[i].p_vaddr;
        uint8_t* src = filebuf + ph[i].p_offset;
        for (uint32_t k = 0; k < ph[i].p_filesz; k++) dst[k] = src[k];
        for (uint32_t k = ph[i].p_filesz; k < ph[i].p_memsz; k++) dst[k] = 0;
        uint32_t seg_end = ph[i].p_vaddr + ph[i].p_memsz;
        if (seg_end > max_seg_end) max_seg_end = seg_end;
        loaded++;
    }

    if (loaded == 0) { println("elf: no PT_LOAD"); kfree(filebuf); return -1; }
    if (entry < ELF_LOAD_BASE) { println("elf: bad entry"); kfree(filebuf); return -1; }

    if (g_elf_debug) { print("elf: entry="); dprint_hex(entry); print(" byte0="); dprint_hex(*(uint8_t*)entry); println(""); }

    // Сохраняем поля заголовка ДО kfree(filebuf) — нужны для AT_PHDR/AT_PHENT/
    // AT_PHNUM. Сами phdr'ы к этому моменту физически уже лежат в памяти
    // программы: обычный статический ET_EXEC грузит сегмент с p_offset=0,
    // куда входят и ELF-заголовок, и таблица program header'ов — поэтому
    // достаточно координаты внутри уже загруженного образа, без отдельного
    // копирования.
    uint32_t saved_phoff  = eh->e_phoff;
    uint16_t saved_phnum  = eh->e_phnum;
    uint16_t saved_phentsz= eh->e_phentsize;

    // Program break = конец самого верхнего сегмента (для SYS_BRK / malloc)
    syscall_reset_fds();
    syscall_set_program_break(max_seg_end ? max_seg_end : (ELF_LOAD_BASE + 0x10000));
    if (g_elf_debug) { print("elf: brk="); dprint_hex(max_seg_end); println(""); }

    kfree(filebuf);

    uint8_t* top = g_stack_storage + ELF_STACK_SIZE;

    // ── argv: filename + args_tail (разбитый по пробелам) ───────────────────
    #define MAX_ARGC 16
    char* argv_ptrs[MAX_ARGC];
    int argc = 0;

    // argv[0] = имя программы
    int name_len = 0; while (filename[name_len]) name_len++;
    top -= (name_len + 1);
    argv_ptrs[argc] = (char*)top;
    for (int i = 0; i <= name_len; i++) argv_ptrs[argc][i] = filename[i];
    argc++;

    // argv[1..] из args_tail
    if (args_tail) {
        const char* p = args_tail;
        while (*p && argc < MAX_ARGC) {
            while (*p == ' ') p++;
            if (!*p) break;
            const char* start = p;
            while (*p && *p != ' ') p++;
            int len = (int)(p - start);
            top -= (len + 1);
            argv_ptrs[argc] = (char*)top;
            for (int i = 0; i < len; i++) argv_ptrs[argc][i] = start[i];
            argv_ptrs[argc][len] = 0;
            argc++;
        }
    }

    if (g_elf_debug) {
        print("elf: argc=");
        print_char((char)('0' + (argc < 10 ? argc : 9)));
        println("");
        for (int i = 0; i < argc; i++) {
            print("  argv[");
            print_char((char)('0' + i));
            print("]=");
            println(argv_ptrs[i]);
        }
        // Сверка того, что реально уйдёт в AT_PHDR/AT_PHENT/AT_PHNUM — чтобы
        // сразу видеть на экране (без serial), если тут окажется мусор.
        print("elf: phdr="); dprint_hex(ELF_LOAD_BASE + saved_phoff);
        print(" phent="); dprint_hex(saved_phentsz);
        print(" phnum="); dprint_hex(saved_phnum);
        println("");
    }

    top -= 16;
    uint8_t* randbytes = top;
    for (int i = 0; i < 16; i++) randbytes[i] = (uint8_t)(0x55 ^ i);

    // Строка платформы для AT_PLATFORM — glibc сверяет её при выборе
    // оптимизированных версий строковых функций/CPU-фич; отсутствие этого
    // auxv-элемента не должно быть фатальным, но раз уж выясняем совместимость
    // с glibc-бинарниками — добавляем за один заход со всем остальным.
    static const char platform_str[] = "i686";
    top -= sizeof(platform_str);
    uint8_t* platform = top;
    for (uint32_t i = 0; i < sizeof(platform_str); i++) platform[i] = (uint8_t)platform_str[i];

    // argc + argv[0..n] + NULL + env NULL + auxv (16 pairs) + AT_NULL
    // ─── auxv ────────────────────────────────────────────────────────────
    // Раньше здесь были только AT_RANDOM/AT_PAGESZ — этого хватало на очень
    // простые статические бинарники, но настоящие программы (tcc и т.п.)
    // используют __init_tls() из musl, которая сама сканирует таблицу
    // program header'ов в поисках PT_TLS — а найти эту таблицу в памяти она
    // может только через AT_PHDR/AT_PHENT/AT_PHNUM. Без них musl либо не
    // находит PT_TLS (для маленьких тестовых бинарников это ещё "прокатывает"
    // — TLS-блок нулевого размера), либо у более сложных программ уходит по
    // пути, которого мы не предусмотрели, и виснет ещё до первого syscall.
    // Добавляем полный стандартный набор, который ожидает статический ELF.
    // AT_HWCAP/AT_CLKTCK/AT_PLATFORM добавлены при разборе glibc-совместимости
    // (musl на них почти не смотрит, а glibc использует для CPU-detect и
    // sysconf(_SC_CLK_TCK) — без них не крашится, но лучше дать честные
    // значения, а не мусор со стека).
    int words = 1 + argc + 1 + 1 + 2*16; // argc, argv[], argv NULL, envp NULL, 16 aux pairs (incl. AT_NULL)
    uint32_t aligned = (uint32_t)top & ~0xFu;
    uint32_t* sp = (uint32_t*)aligned;
    sp -= words;
    sp = (uint32_t*)((uint32_t)sp & ~0xFu);

    uint32_t* w = sp;
    *w++ = (uint32_t)argc;
    for (int i = 0; i < argc; i++) *w++ = (uint32_t)argv_ptrs[i];
    *w++ = 0; // argv terminator
    *w++ = 0; // envp terminator
    *w++ = AT_PHDR;    *w++ = ELF_LOAD_BASE + saved_phoff;
    *w++ = AT_PHENT;   *w++ = saved_phentsz;
    *w++ = AT_PHNUM;   *w++ = saved_phnum;
    *w++ = AT_PAGESZ;  *w++ = 4096;
    *w++ = AT_BASE;    *w++ = 0;      // не динамический загрузчик
    *w++ = AT_ENTRY;   *w++ = entry;
    *w++ = AT_UID;     *w++ = 0;
    *w++ = AT_EUID;    *w++ = 0;
    *w++ = AT_GID;     *w++ = 0;
    *w++ = AT_EGID;    *w++ = 0;
    *w++ = AT_SECURE;  *w++ = 0;
    *w++ = AT_RANDOM;  *w++ = (uint32_t)randbytes;
    *w++ = AT_HWCAP;   *w++ = 0;      // без SSE/CPU-фич — безопасный минимум
    *w++ = AT_CLKTCK;  *w++ = 100;    // стандартные 100 Гц, используется times()/sysconf
    *w++ = AT_PLATFORM;*w++ = (uint32_t)platform;
    *w++ = AT_NULL;    *w++ = 0;

    uint32_t new_esp = (uint32_t)sp;
    if (g_elf_debug) { print("elf: esp="); dprint_hex(new_esp); println(""); println("elf: jump..."); }

    g_exit_code = 0;

    // Сохраняем cwd шелла ДО запуска программы. current_dir_cluster — общая
    // на всю систему переменная (нет отдельного cwd на процесс), и chdir()
    // внутри программы (например, busybox unzip -d <dir>) иначе навсегда
    // "утаскивает" шелл в чужую директорию — как при нормальном exit(),
    // так и при принудительном убийстве по Ctrl+C (оба пути возвращаются
    // сюда же через longjmp).
    g_saved_cwd_cluster = current_dir_cluster;

    // setjmp: 0 = первый заход (идём в программу), ≠0 = возврат из exit
    if (__builtin_setjmp(g_jmp) != 0) {
        g_elf_running = false;
        current_dir_cluster = g_saved_cwd_cluster;
        __asm__ volatile("sti");
        if (g_elf_debug) {
            print("elf: returned code=");
            int code = g_exit_code;
            if (code < 0) { print_char('-'); code = -code; }
            if (code == 0) print_char('0');
            else {
                char buf[12]; int n = 0;
                int c = code;
                while (c > 0) { buf[n++] = (char)('0' + c % 10); c /= 10; }
                while (n--) print_char(buf[n]);
            }
            println("");
        }
        return g_exit_code;
    }

    g_elf_running = true;
    jump_to_entry(new_esp, entry);
}