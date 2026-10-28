#include "syscall.h"
#include "interrupts.h"
#include "elf_loader.h"
#include "gdt.h"
#include "keyboard.h"
#include "mouse.h"
#include "../lib/screen.h"
#include "../lib/heap.h"
#include "../drivers/fat32.h"
#include "../drivers/disk.h"
#include "../drivers/vga_gfx.h"
#include "version.h"

extern "C" void isr128(void);

// ─── errno-коды (отрицательные в eax) ────────────────────────────────────────
#define EPERM        1
#define ENOENT       2
#define EIO          5
#define EBADF        9
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define EEXIST      17
#define ENOTDIR     20
#define EISDIR      21
#define EINVAL      22
#define ENFILE      23
#define EMFILE      24
#define ENOSPC      28
#define EPIPE       32
#define ENOSYS      38
#define ENODEV      19
#define ERANGE      34
#define ENOTTY      25

#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT   0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_CLOEXEC 0x80000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

// ─── brk ─────────────────────────────────────────────────────────────────────
static uint32_t g_brk_start = 0; // initial break (end of ELF image)
static uint32_t g_brk       = 0; // current break

void syscall_set_program_break(uint32_t brk) {
    // page-align up
    brk = (brk + 0xFFF) & ~0xFFFu;
    g_brk_start = brk;
    g_brk       = brk;
}

// ─── динамическая память программы: brk-куча и mmap-арена делят одну общую
// область ───────────────────────────────────────────────────────────────────
// Раньше границы были жёстко прибиты гвоздями (brk max = start+8 МБ, mmap —
// фиксированное окно 0x5000000..0x8000000) независимо от реального объёма
// ОЗУ. Теперь: всё, что выше ELF_LOAD_BASE (64 МБ, зона ОС ниже неё) и до
// g_mem_ceiling (реальный потолок ОЗУ, определённый загрузчиком через BIOS —
// см. syscall_set_mem_ceiling()) — общий пул программы. brk растёт СНИЗУ
// ВВЕРХ (от конца ELF-образа), mmap-арена растёт СВЕРХУ ВНИЗ (от потолка
// ОЗУ) — так один hlt-безопасный bump-указатель не может выдать ту же
// память дважды: они просто сталкиваются, когда место кончилось. Т.к. ядро
// однозадачное (одна программа выполняется синхронно за раз — см. elf_run),
// делить эту область больше не с кем.
static uint32_t g_mmap_bump   = 0;   // нижняя граница ещё свободной части mmap-арены
static uint32_t g_mem_ceiling = 0;   // потолок ОЗУ, доступной текущей программе
#define FALLBACK_MEM_CEILING 0x8000000u // 128 МБ — если объём ОЗУ не определён загрузчиком

void syscall_set_mem_ceiling(uint32_t mem_end) {
    g_mem_ceiling = mem_end ? mem_end : FALLBACK_MEM_CEILING;
    g_mmap_bump   = g_mem_ceiling;
}

// ─── file descriptors ────────────────────────────────────────────────────────
#define MAX_FDS 32
#define FD_STDIN  0
#define FD_STDOUT 1
#define FD_STDERR 2

enum FdType : uint8_t { FD_NONE = 0, FD_STD, FD_FILE };

struct FileFd {
    FdType   type;
    uint8_t  flags;      // O_RDONLY etc. low bits
    uint32_t cluster;    // start cluster
    uint32_t dir_cluster;// кластер родительской директории (где лежит directory entry) — нужен sys_write, чтобы обновить file_size на диске
    uint32_t size;       // file size
    uint32_t offset;     // current offset
    char     name[64];   // leaf name (for debug)
};

static FileFd g_fds[MAX_FDS];

void syscall_reset_fds() {
    for (int i = 0; i < MAX_FDS; i++) {
        g_fds[i].type = FD_NONE;
        g_fds[i].offset = 0;
        g_fds[i].size = 0;
        g_fds[i].cluster = 0;
        g_fds[i].dir_cluster = 0;
        g_fds[i].name[0] = 0;
    }
    g_fds[0].type = FD_STD; // stdin
    g_fds[1].type = FD_STD; // stdout
    g_fds[2].type = FD_STD; // stderr
    // mmap-арена (g_mmap_bump/g_mem_ceiling) сбрасывается отдельно —
    // см. syscall_set_mem_ceiling(), вызывается из elf_run() тем же тактом.
}

static int fd_alloc() {
    for (int i = 3; i < MAX_FDS; i++)
        if (g_fds[i].type == FD_NONE) return i;
    return -1;
}

static bool fd_valid(int fd) {
    return fd >= 0 && fd < MAX_FDS && g_fds[fd].type != FD_NONE;
}

// ─── helpers ─────────────────────────────────────────────────────────────────
struct Iovec {
    uint32_t base;
    uint32_t len;
};

struct NewUtsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct Stat64 {
    uint64_t st_dev;
    uint32_t __pad1;
    uint32_t st_ino_low;   // broken layout for simplicity — enough for musl checks
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint32_t __pad2;
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint64_t st_ino;
} __attribute__((packed));

#define S_IFREG 0x8000
#define S_IFDIR 0x4000
#define S_IFCHR 0x2000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IRGRP 0040
#define S_IROTH 0004

static void memset0(void* p, uint32_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) b[i] = 0;
}


static void open_print_hex(uint32_t v) {
    print("0x");
    for (int i = 7; i >= 0; i--) {
        uint8_t n = (v >> (i * 4)) & 0xF;
        print_char(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}
static void strcpy_pad(char* dst, const char* src, int n) {
    int i = 0;
    while (src[i] && i < n - 1) { dst[i] = src[i]; i++; }
    while (i < n) dst[i++] = 0;
}

// ─── open path relative to current_dir ───────────────────────────────────────
static int sys_open(const char* path, int flags, int /*mode*/) {
    if (!path) return -EFAULT;

    // Копия пути в ядерный буфер
    char name[128];
    int i = 0;
    while (path[i] && i < 127) { name[i] = path[i]; i++; }
    name[i] = 0;
    if (!name[0]) return -EINVAL;

    // ./prefix
    const char* np = name;
    if (np[0]=='.' && np[1]=='/') np += 2;

    // /dev/null
    if (np[0]=='/' && np[1]=='d' && np[2]=='e' && np[3]=='v' &&
        np[4]=='/' && np[5]=='n' && np[6]=='u' && np[7]=='l' &&
        np[8]=='l' && np[9]==0) {
        int fd = fd_alloc();
        if (fd < 0) return -EMFILE;
        g_fds[fd].type = FD_FILE;
        g_fds[fd].cluster = 0;
        g_fds[fd].size = 0;
        g_fds[fd].offset = 0;
        g_fds[fd].flags = (uint8_t)(flags & 3);
        strcpy_pad(g_fds[fd].name, "/dev/null", 64);
        return fd;
    }

    // Абсолютный / относительный с '/'
    uint32_t saved_dir = current_dir_cluster;
    char leaf[128];
    if (np[0] == '/') {
        uint32_t dir = 2;
        if (!fat32_resolve_path(np, &dir, leaf)) {
            print("open: resolve fail: ");
            println(np);
            return -ENOENT;
        }
        current_dir_cluster = dir;
    } else {
        // простой leaf — как cmd_head: ищем в current_dir_cluster
        int k = 0;
        while (np[k] && k < 127) { leaf[k] = np[k]; k++; }
        leaf[k] = 0;
        // current_dir_cluster уже нужный
    }

    // Разрешаем прерывания на время дискового I/O (IDT gate держит CLI)
    uint32_t entry_dir = current_dir_cluster; // родительская директория, где будет лежать найденная/созданная запись
    __asm__ volatile ("sti");

    FAT32_FindResult fr = fat32_find_entry(leaf, 0x00);

    __asm__ volatile ("cli");

    current_dir_cluster = saved_dir;

    if (!fr.found) {
        if (flags & O_CREAT) {
            current_dir_cluster = (np[0]=='/') ? /* resolved was leaf's parent */ saved_dir : saved_dir;
            // recreate: set dir again
            if (np[0]=='/') {
                uint32_t dir = 2;
                fat32_resolve_path(np, &dir, leaf);
                current_dir_cluster = dir;
            } else {
                current_dir_cluster = saved_dir;
            }
            entry_dir = current_dir_cluster; // пересчитано выше под O_CREAT — та же логика, что и при первом поиске
            __asm__ volatile ("sti");
            bool ok = fat32_create_file(leaf, "", 0);
            __asm__ volatile ("cli");
            if (!ok) { current_dir_cluster = saved_dir; return -ENOSPC; }
            __asm__ volatile ("sti");
            fr = fat32_find_entry(leaf, 0x00);
            __asm__ volatile ("cli");
            current_dir_cluster = saved_dir;
            if (!fr.found) return -EIO;
        } else {
            print("open: not found: ");
            println(leaf);
            print("  cwd cluster=");
            // print hex of saved_dir
            open_print_hex(saved_dir);
            println("");
            return -ENOENT;
        }
    }

    if (fr.entry.attributes & 0x10) {
        println("open: is directory");
        return -EISDIR;
    }

    int fd = fd_alloc();
    if (fd < 0) return -EMFILE;

    g_fds[fd].type = FD_FILE;
    g_fds[fd].cluster = FAT32_GET_CLUSTER(&fr.entry);
    g_fds[fd].dir_cluster = entry_dir;
    g_fds[fd].size = fr.entry.file_size;
    g_fds[fd].offset = (flags & O_APPEND) ? fr.entry.file_size : 0;
    g_fds[fd].flags = (uint8_t)(flags & 3);
    strcpy_pad(g_fds[fd].name, leaf, 64);

    if ((flags & O_TRUNC) && (flags & 3) != O_RDONLY) {
        g_fds[fd].size = 0;
        g_fds[fd].offset = 0;
    }
    return fd;
}

static int sys_close(int fd) {
    if (fd < 3) return 0; // ignore close stdin/out/err
    if (!fd_valid(fd)) return -EBADF;
    g_fds[fd].type = FD_NONE;
    return 0;
}

static int allive_test() {
    print("allive_test called\n");
    return 0;
}

static void strace_hex(uint32_t v); // определена ниже (используется debug-принтом в sys_read)

static int sys_read(int fd, char* buf, uint32_t count) {
    if (!buf) return -EFAULT;
    if (!fd_valid(fd)) return -EBADF;
    if (count == 0) return 0;

    if (g_fds[fd].type == FD_STD) {
        if (fd != FD_STDIN) return -EBADF;
        // Блокирующее чтение. IDT gate держит CLI — без STI клавиатура
        // (IRQ1) не заполняет буфер, а hlt никогда не просыпается.
        __asm__ volatile ("sti");

        // Ctrl+C — общий для обоих режимов ниже: убиваем процесс сразу,
        // даже если user-code завис в логике.
        auto handle_ctrl_c = [](uint8_t k) -> bool {
            if (k != CHAR_CTRL_C) return false;
            __asm__ volatile ("cli");
            {
                volatile unsigned short* vga = (volatile unsigned short*)0xB8000;
                for (int i = 0; i < 80 * 25; i++) vga[i] = 0x0F00 | ' ';
            }
            elf_request_exit(130); // 128+SIGINT
            return true; // не достижимо — elf_request_exit не возвращается
        };

        if (count == 1) {
            // "Сырой" режим — как раньше: ровно 1 символ, без построчной
            // буферизации/эха/Backspace. Не трогаем — на нём уже держится
            // рабочий real-time ввод (например, игра читает посимвольно
            // WASD без ожидания Enter).
            uint32_t got = 0;
            uint8_t k = 0;
            while (k == 0) {
                k = get_key();
                if (k == 0) __asm__ volatile ("hlt");
            }
            handle_ctrl_c(k);
            if (k == '\r') k = '\n';
            if (k < 128) { buf[0] = (char)k; got = 1; }
            __asm__ volatile ("cli");
            return (int)got;
        }

        // "Построчный" (canonical) режим — включается сам собой, когда
        // читающий просит больше 1 байта за раз (типичное поведение
        // fgets()/scanf() — просят большой буфер и ждут целую строку).
        // Накапливаем с редактированием Backspace и эхом на экран, отдаём
        // всё разом по Enter — как обычный терминал в "cooked"-режиме.
        // Специально не трогаем keyboard.cpp — это касается только чтения
        // из ELF-программ через syscall, остальной ввод (шелл, gite) как был.
        uint32_t got = 0;
        while (got < count) {
            uint8_t k = 0;
            while (k == 0) {
                k = get_key();
                if (k == 0) __asm__ volatile ("hlt");
            }
            handle_ctrl_c(k);
            if (k == '\b' || k == 127) {
                if (got > 0) { got--; print_char('\b'); print_char(' '); print_char('\b'); }
                continue;
            }
            if (k == '\r') k = '\n';
            if (k == '\n') {
                print_char('\n');
                if (got < count) buf[got++] = '\n';
                break;
            }
            if (k >= 128) continue; // служебные коды (стрелки и т.п.) — пропускаем
            if (got + 1 >= count) continue; // держим место под завершающий '\n'/конец буфера
            print_char((char)k); // эхо — иначе не видно, что печатаешь
            buf[got++] = (char)k;
        }
        __asm__ volatile ("cli");
        return (int)got;
    }

    // file
    if ((g_fds[fd].flags & 3) == O_WRONLY) return -EBADF;
    // /dev/null
    if (g_fds[fd].name[0]=='/' && g_fds[fd].cluster == 0 &&
        g_fds[fd].name[1]=='d') return 0;

    if (g_fds[fd].offset >= g_fds[fd].size) return 0;
    uint32_t remain = g_fds[fd].size - g_fds[fd].offset;
    if (count > remain) count = remain;
    if (count == 0) return 0;

    // Пустой файл (cluster < 2) — нечего читать
    if (g_fds[fd].cluster < 2 || g_fds[fd].size == 0) {
        g_fds[fd].offset += count;
        // zeros already not written — just EOF behavior: return 0 for empty
        return 0;
    }

    uint8_t* tmp = (uint8_t*)kmalloc(g_fds[fd].size);
    if (!tmp) { println("[rd] kmalloc FAILED"); return -ENOMEM; }

    __asm__ volatile ("sti");
    fat32_read_file_to_buffer(g_fds[fd].cluster, g_fds[fd].size, tmp, g_fds[fd].size);
    __asm__ volatile ("cli");

    for (uint32_t i = 0; i < count; i++)
        buf[i] = (char)tmp[g_fds[fd].offset + i];
    kfree(tmp);
    g_fds[fd].offset += count;

    return (int)count;
}

// Пишет count байт из buf в файл, начиная с f->offset, наращивая цепочку
// кластеров по мере необходимости, и обновляет file_size в directory entry
// на диске. Возвращает число реально записанных байт или -errno.
static int fat32_write_to_fd(FileFd* f, const uint8_t* buf, uint32_t count) {
    print("[realwrite]\n");
    if (count == 0) return 0;
    if (f->cluster < 2) return -EIO; // не должно случаться — fat32_create_file всегда даёт кластер

    const uint32_t CLUSTER_BYTES = FAT32_SECTORS_PER_CLUSTER * FAT32_BYTES_PER_SECTOR;
    uint32_t file_off = f->offset;
    uint32_t target_index = file_off / CLUSTER_BYTES;

    print("[wr:1 chain-walk start]\n");
    // Доходим по цепочке до кластера, на который приходится текущий offset,
    // при необходимости наращивая цепочку (например, после lseek() за EOF).
    uint32_t cluster = f->cluster;
    for (uint32_t idx = 0; idx < target_index; idx++) {
        uint32_t next = fat32_get_next_cluster(cluster);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) {
            uint32_t nc = fat32_find_free_cluster();
            if (nc == 0) return -ENOSPC;
            fat32_set_entry(cluster, nc);
            fat32_set_entry(nc, FAT32_EOC);
            next = nc;
        }
        cluster = next;
    }
    print("[wr:2 chain-walk done]\n");

    uint32_t written = 0;
    uint32_t safety = 0;
    while (written < count) {
        if (++safety > 100000) { print("[wr:LOOP-STUCK in write-loop]\n"); break; }
        uint32_t in_cluster_off = (file_off + written) % CLUSTER_BYTES;
        uint32_t chunk = CLUSTER_BYTES - in_cluster_off;
        if (chunk > count - written) chunk = count - written;

        fat32_write_cluster_data(cluster, buf + written, chunk, in_cluster_off);
        written += chunk;

        if (written < count) {
            uint32_t next = fat32_get_next_cluster(cluster);
            if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) {
                uint32_t nc = fat32_find_free_cluster();
                if (nc == 0) break; // диск кончился — отдаём, что успели записать
                fat32_set_entry(cluster, nc);
                fat32_set_entry(nc, FAT32_EOC);
                next = nc;
            }
            cluster = next;
        }
    }
    print("[wr:3 write-loop done]\n");

    f->offset += written;
    if (f->offset > f->size) f->size = f->offset;

    // Обновляем file_size прямо в directory entry на диске.
    print("[wr:4 dir-update start]\n");
    uint32_t saved_dir = current_dir_cluster;
    current_dir_cluster = f->dir_cluster;
    uint32_t lba; int idx2;
    FAT32_DirEntry* e = fat32_find_entry_for_modification(f->name, &lba, &idx2, 0x00);
    print("[wr:5 dir-scan done]\n");
    if (e) {
        e->file_size = f->size;
        print("[wr:6 dir-entry-write start]\n");
        ata_write_sector(lba, sector_buffer); // sector_buffer всё ещё содержит этот сектор — см. scan_dir_cluster
        print("[wr:7 dir-entry-write done]\n");
    } else {
        print("[wr:6b dir-entry NOT FOUND]\n");
    }
    current_dir_cluster = saved_dir;

    print("[wr:8 return]\n");
    return (int)written;
}

static int sys_write(int fd, const char* buf, uint32_t count) {
    if (!buf) return -EFAULT;
    if (!fd_valid(fd)) return -EBADF;

    if (g_fds[fd].type == FD_STD) {
        if (fd == FD_STDIN) return -EBADF;
        for (uint32_t i = 0; i < count; i++) print_char(buf[i]);
        return (int)count;
    }

    // /dev/null
    if (g_fds[fd].cluster == 0 && g_fds[fd].name[0]=='/') return (int)count;

    if ((g_fds[fd].flags & 3) == O_RDONLY) return -EBADF;

    __asm__ volatile ("sti");
    int n = fat32_write_to_fd(&g_fds[fd], (const uint8_t*)buf, count);
    __asm__ volatile ("cli");
    return n;
}

static int sys_writev(int fd, const Iovec* iov, int iovcnt) {
    if (iovcnt < 0) return -EINVAL;
    if (!iov && iovcnt > 0) return -EFAULT;
    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        int n = sys_write(fd, (const char*)iov[i].base, iov[i].len);
        if (n < 0) return n;
        total += n;
    }
    return total;
}

static int sys_readv(int fd, const Iovec* iov, int iovcnt) {
    if (iovcnt < 0) return -EINVAL;
    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        int n = sys_read(fd, (char*)iov[i].base, iov[i].len);
        if (n < 0) return n;
        total += n;
        if ((uint32_t)n < iov[i].len) break;
    }
    return total;
}

static int sys_lseek(int fd, int32_t offset, int whence) {
    if (!fd_valid(fd) || g_fds[fd].type != FD_FILE) return -EBADF;
    int64_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = g_fds[fd].offset;
    else if (whence == SEEK_END) base = g_fds[fd].size;
    else return -EINVAL;
    int64_t neu = base + offset;
    if (neu < 0) return -EINVAL;
    g_fds[fd].offset = (uint32_t)neu;
    return (int)g_fds[fd].offset;
}

static int sys_brk(uint32_t new_brk) {
    if (new_brk == 0) return (int)g_brk; // query
    if (new_brk < g_brk_start) return (int)g_brk; // ignore shrink below start
    // Верхний предел больше не жёсткая цифра — куча растёт, пока не
    // упрётся снизу в mmap-арену (та занимает всё выше g_mmap_bump).
    if (new_brk > g_mmap_bump) return -ENOMEM;
    // У нас нет paging/изоляции адресных пространств — весь диапазон
    // ELF_LOAD_BASE..mem_ceiling переиспользуется между программами как
    // есть, физически. Настоящий Linux гарантирует, что свежая память от
    // brk()/mmap() всегда нулевая (демисейд-страницы); mmap2() у нас это
    // соблюдает (см. memset0 выше), а brk() раньше — нет, из-за чего новая
    // программа получала под "нулевую" кучу мусор от предыдущей (например,
    // heavy vim -> tcc). Зануляем именно прирост, а не весь брк, чтобы не
    // трогать данные, которые программа уже сама туда записала.
    if (new_brk > g_brk) memset0((void*)g_brk, new_brk - g_brk);
    g_brk = new_brk;
    return (int)g_brk;
}

static int sys_mmap2(uint32_t addr, uint32_t len, uint32_t prot,
                     uint32_t flags, int fd, uint32_t pgoff) {
    (void)prot; (void)pgoff;
    if (len == 0) return -EINVAL;
    len = (len + 0xFFF) & ~0xFFFu;

    if (flags & MAP_ANONYMOUS) {
        if (flags & MAP_FIXED) {
            // accept fixed if in reasonable range
            if (addr < 0x100000u) return -EINVAL;
            memset0((void*)addr, len);
            return (int)addr;
        }
        // Арена растёт вниз от потолка ОЗУ; столкновение с кучей (g_brk),
        // растущей снизу, означает, что место действительно кончилось.
        if (len > g_mmap_bump) return -ENOMEM; // защита от переполнения вниз
        uint32_t base = g_mmap_bump - len;
        if (base < g_brk) return -ENOMEM;
        g_mmap_bump = base;
        memset0((void*)base, len);
        return (int)base;
    }

    // file-backed: not supported yet
    (void)fd;
    return -ENODEV;
}

static int sys_munmap(uint32_t /*addr*/, uint32_t /*len*/) {
    return 0; // leak is fine for short-lived programs
}

static int sys_mprotect(uint32_t /*addr*/, uint32_t /*len*/, uint32_t /*prot*/) {
    return 0; // no paging — always RWX
}

static int sys_ioctl(int fd, uint32_t req, uint32_t arg) {
    if (!fd_valid(fd)) return -EBADF;
    // TIOCGWINSZ = 0x5413 — struct winsize { row, col, xpixel, ypixel } as u16[4]
    if (req == 0x5413) {
        if (arg) {
            uint16_t* ws = (uint16_t*)arg;
            ws[0] = 25;  // ws_row
            ws[1] = 80;  // ws_col
            ws[2] = 0;
            ws[3] = 0;
        }
        return 0;
    }
    // TCGETS / TCSETS — pretend we are a dumb TTY (success, leave buffer alone)
    if (req == 0x5401 || req == 0x5402 || req == 0x5403 || req == 0x5404)
        return 0;
    return 0; // hobby OS: ignore unknown ioctls instead of ENOTTY spam
}

static int sys_uname(NewUtsname* u) {
    if (!u) return -EFAULT;
    strcpy_pad(u->sysname,    OS_NAME, 65);
    strcpy_pad(u->nodename,   OS_NAME " " OS_CODENAME, 65);
    strcpy_pad(u->release,    OS_NAME " " OS_RELEASE, 65);
    strcpy_pad(u->version,    OS_NAME " " OS_VERSION, 65);
    strcpy_pad(u->machine,    OS_ARCH, 65);
    strcpy_pad(u->domainname, "(none)", 65);
    return 0;
}

// User_desc for set_thread_area (simplified)
struct UserDesc {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags; // bitfield packed — we just accept
};

// Отладочная зацепка: адрес TCB, который последним отдали через
// set_thread_area. Нужен только чтобы дампить %gs:0x14 (canary) на
// каждом syscall и понять, меняется ли он между установкой и крашем.
uint32_t g_debug_tcb_base = 0;

static int sys_set_thread_area(UserDesc* desc) {
    if (!desc) return -EFAULT;
    int idx = (int)desc->entry_number;
    if (idx == -1) idx = 6; // glibc/musl convention: -1 = "дай любой свободный", у нас всегда слот 6
    if (idx < 6 || idx > 8) return -EINVAL;
    uint16_t sel = gdt_set_tls(idx, desc->base_addr);
    if (!sel) return -EINVAL;
    desc->entry_number = (uint32_t)idx; // сообщаем обратно, какой слот реально заняли
    g_debug_tcb_base = desc->base_addr;
    return 0;
}

static int sys_fstat64(int fd, Stat64* st) {
    if (!st) return -EFAULT;
    if (!fd_valid(fd)) return -EBADF;
    memset0(st, sizeof(Stat64));
    if (g_fds[fd].type == FD_STD) {
        st->st_mode = S_IFCHR | 0666;
        st->st_nlink = 1;
        return 0;
    }
    st->st_mode = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    st->st_nlink = 1;
    st->st_size = g_fds[fd].size;
    st->st_blksize = 4096;
    st->st_blocks = (g_fds[fd].size + 511) / 512;
    return 0;
}

static int sys_stat64(const char* path, Stat64* st) {
    if (!path || !st) return -EFAULT;
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    int r = sys_fstat64(fd, st);
    sys_close(fd);
    return r;
}

static int sys_access(const char* path, int /*mode*/) {
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    sys_close(fd);
    return 0;
}

static int sys_getdents64(int /*fd*/, void* /*dirp*/, uint32_t /*count*/) {
    return -ENOSYS; // later
}

static int sys_fcntl(int fd, int cmd, uint32_t /*arg*/) {
    if (!fd_valid(fd)) return -EBADF;
    // F_GETFL = 3, F_SETFL = 4, F_GETFD = 1, F_SETFD = 2
    if (cmd == 1 || cmd == 2 || cmd == 3 || cmd == 4) return 0;
    return 0;
}

static int sys_getcwd(char* buf, uint32_t size) {
    if (!buf || size < 2) return -ERANGE;
    buf[0] = '/';
    buf[1] = 0;
    return 2; // bytes including NUL
}

// ─── chdir / mkdir (FAT32) ───────────────────────────────────────────────────
// Normalize: '\' -> '/', strip trailing slashes, copy into buf (max 127).
static int path_norm(const char* path, char* buf, int bufsz) {
    if (!path || !path[0] || bufsz < 2) return -1;
    int i = 0, j = 0;
    while (path[i] && j < bufsz - 1) {
        char c = path[i++];
        if (c == '\\') c = '/';
        buf[j++] = c;
    }
    buf[j] = 0;
    while (j > 1 && buf[j - 1] == '/') buf[--j] = 0;
    return j;
}

// Walk one path component: look up `seg` as directory inside `*cluster`, update *cluster.
static bool dir_walk_one(uint32_t* cluster, const char* seg) {
    if (!seg[0] || (seg[0] == '.' && seg[1] == 0)) return true; // "" or "."
    if (seg[0] == '.' && seg[1] == '.' && seg[2] == 0) {
        // ".." — look up in current cluster
        uint32_t saved = current_dir_cluster;
        current_dir_cluster = *cluster;
        FAT32_FindResult r = fat32_find_entry("..", 0x10);
        current_dir_cluster = saved;
        if (!r.found) return false;
        uint32_t next = FAT32_GET_CLUSTER(&r.entry);
        *cluster = (next < 2) ? FAT32_ROOT_CLUSTER : next;
        return true;
    }
    uint32_t saved = current_dir_cluster;
    current_dir_cluster = *cluster;
    // First try directory-only search, then any entry + check attr
    FAT32_FindResult r = fat32_find_entry(seg, 0x10);
    if (!r.found) r = fat32_find_entry(seg, 0);
    current_dir_cluster = saved;
    if (!r.found || !(r.entry.attributes & 0x10)) return false;
    uint32_t next = FAT32_GET_CLUSTER(&r.entry);
    *cluster = (next < 2) ? FAT32_ROOT_CLUSTER : next;
    return true;
}

// Create directory `seg` inside `cluster` (must not already exist as file).
static int dir_create_one(uint32_t cluster, const char* seg) {
    if (!seg[0] || (seg[0] == '.' && (seg[1] == 0 || (seg[1] == '.' && seg[2] == 0))))
        return 0;
    uint32_t saved = current_dir_cluster;
    current_dir_cluster = cluster;
    FAT32_FindResult r = fat32_find_entry(seg, 0);
    if (r.found) {
        current_dir_cluster = saved;
        if (r.entry.attributes & 0x10) return 0; // already a dir — ok (mkdir -p)
        return -EEXIST;
    }
    char namebuf[128];
    int i = 0;
    while (seg[i] && i < 127) { namebuf[i] = seg[i]; i++; }
    namebuf[i] = 0;
    fat32_create_dir(namebuf);
    // verify
    r = fat32_find_entry(seg, 0x10);
    current_dir_cluster = saved;
    if (!r.found) return -EIO;
    return 0;
}

static int sys_chdir(const char* path) {
    // BusyBox unzip may call chdir("") for directory entries — treat as no-op.
    if (!path || !path[0]) return 0;
    char npath[128];
    if (path_norm(path, npath, sizeof(npath)) < 0) return -ENOENT;
    if (!npath[0]) return 0;

    if (npath[0] == '/' && npath[1] == 0) {
        current_dir_cluster = FAT32_ROOT_CLUSTER;
        return 0;
    }

    uint32_t cluster = (npath[0] == '/') ? FAT32_ROOT_CLUSTER : current_dir_cluster;
    int i = (npath[0] == '/') ? 1 : 0;

    while (npath[i]) {
        char seg[128]; int si = 0;
        while (npath[i] == '/') i++;
        while (npath[i] && npath[i] != '/' && si < 127) seg[si++] = npath[i++];
        seg[si] = 0;
        if (!seg[0]) break;
        if (!dir_walk_one(&cluster, seg)) return -ENOENT;
    }
    current_dir_cluster = cluster;
    return 0;
}

static int sys_mkdir(const char* path, uint32_t /*mode*/) {
    if (!path || !path[0]) return -ENOENT;
    char npath[128];
    if (path_norm(path, npath, sizeof(npath)) < 0) return -ENOENT;
    if (npath[0] == '/' && npath[1] == 0) return -EEXIST;

    // mkdir -p style: create every missing component (BusyBox unzip needs this)
    uint32_t cluster = (npath[0] == '/') ? FAT32_ROOT_CLUSTER : current_dir_cluster;
    int i = (npath[0] == '/') ? 1 : 0;
    int created = 0;

    while (npath[i]) {
        char seg[128]; int si = 0;
        while (npath[i] == '/') i++;
        while (npath[i] && npath[i] != '/' && si < 127) seg[si++] = npath[i++];
        seg[si] = 0;
        if (!seg[0]) break;

        uint32_t saved = current_dir_cluster;
        current_dir_cluster = cluster;
        FAT32_FindResult r = fat32_find_entry(seg, 0);
        current_dir_cluster = saved;

        if (r.found) {
            if (!(r.entry.attributes & 0x10)) return -EEXIST; // file in the way
            uint32_t next = FAT32_GET_CLUSTER(&r.entry);
            cluster = (next < 2) ? FAT32_ROOT_CLUSTER : next;
        } else {
            int rc = dir_create_one(cluster, seg);
            if (rc < 0) return rc;
            created++;
            // re-walk into the new dir
            if (!dir_walk_one(&cluster, seg)) return -EIO;
        }
    }
    return 0;
}


// ─── dispatch ────────────────────────────────────────────────────────────────
void syscall_init() {
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0x8E);
    syscall_reset_fds();
}

static void strace_hex(uint32_t v) {
    print("0x");
    for (int i = 7; i >= 0; i--) {
        uint8_t n = (uint8_t)((v >> (i * 4)) & 0xF);
        print_char(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}

extern uint32_t g_debug_tcb_base;

// Дамп 32 байт TCB начиная с base_addr, отдельно подсвечиваем
// offset 0x14 (там glibc держит __stack_chk_guard/canary у tcbhead_t).
// Если это значение "прыгает" между вызовами без причины — TCB реально
// портится где-то в промежутке; если стабильно — дело не в этом.
static void strace_dump_tcb() {
    if (g_debug_tcb_base == 0) return;
    uint8_t* p = (uint8_t*)g_debug_tcb_base;
    print("  tcb@"); strace_hex(g_debug_tcb_base); print(": ");
    for (int i = 0; i < 32; i++) {
        uint8_t b = p[i];
        print_char(i == 0x14 ? '[' : ' ');
        uint8_t hi = (uint8_t)(b >> 4), lo = (uint8_t)(b & 0xF);
        print_char(hi < 10 ? (char)('0' + hi) : (char)('A' + hi - 10));
        print_char(lo < 10 ? (char)('0' + lo) : (char)('A' + lo - 10));
        print_char(i == 0x14 ? ']' : ' ');
    }
    println("");
}

extern "C" uint32_t syscall_dispatch(uint32_t num,
                                     uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t a4, uint32_t a5, uint32_t a6) {
    (void)a6;  // a5 used by multi-arg gfx syscalls
    if (g_elf_debug) {
        print("syscall: num="); strace_hex(num);
        print(" a1="); strace_hex(a1);
        print(" a2="); strace_hex(a2);
        print(" a3="); strace_hex(a3);
        println("");
        strace_dump_tcb();
    }
    switch (num) {
        case SYS_EXIT:
        case SYS_EXIT_GROUP:
            elf_request_exit((int)a1);
            return 0;

        case SYS_READ:
            return (uint32_t)sys_read((int)a1, (char*)a2, a3);

        case SYS_WRITE:
            return (uint32_t)sys_write((int)a1, (const char*)a2, a3);

        case SYS_OPEN:
            return (uint32_t)sys_open((const char*)a1, (int)a2, (int)a3);

        case SYS_OPENAT: // openat(dfd, path, flags, mode) — ignore dfd
            return (uint32_t)sys_open((const char*)a2, (int)a3, (int)a4);

        case SYS_CLOSE:
            return (uint32_t)sys_close((int)a1);

        case SYS_LSEEK:
            return (uint32_t)sys_lseek((int)a1, (int32_t)a2, (int)a3);

        case SYS_LLSEEK: {
            // _llseek(fd, offset_high, offset_low, result*, whence)
            // BusyBox unzip uses this. Our FS offsets stay 32-bit.
            if (!fd_valid((int)a1) || g_fds[a1].type != FD_FILE)
                return (uint32_t)-EBADF;
            int whence = (int)a5;
            // Ignore high 32 bits for now (files < 4GB)
            int32_t off = (int32_t)a3;
            uint32_t base = 0;
            if (whence == SEEK_SET) base = 0;
            else if (whence == SEEK_CUR) base = g_fds[a1].offset;
            else if (whence == SEEK_END) base = g_fds[a1].size;
            else return (uint32_t)-EINVAL;
            int32_t neu = (int32_t)base + off;
            if (neu < 0) return (uint32_t)-EINVAL;
            g_fds[a1].offset = (uint32_t)neu;
            if (a4) {
                uint32_t* res = (uint32_t*)a4;
                res[0] = (uint32_t)neu; // low
                res[1] = 0;             // high
            }
            return 0;
        }

        case SYS_BRK:
            return (uint32_t)sys_brk(a1);

        case SYS_IOCTL:
            return (uint32_t)sys_ioctl((int)a1, a2, a3);

        case SYS_POLL: {
            // a1 = struct pollfd* { int fd; short events; short revents; }
            // a2 = nfds, a3 = timeout_ms (-1 = infinite, 0 = immediate)
            if (!a1 && a2) return (uint32_t)-EINVAL;
            struct PollFd { int32_t fd; int16_t events; int16_t revents; };
            PollFd* pfd = (PollFd*)a1;
            uint32_t nfds = a2;
            int32_t timeout = (int32_t)a3;
            const int16_t POLLIN = 0x0001, POLLOUT = 0x0004, POLLERR = 0x0008;

            auto fill = [&]() -> int {
                int ready = 0;
                for (uint32_t i = 0; i < nfds; i++) {
                    pfd[i].revents = 0;
                    int fd = pfd[i].fd;
                    if (fd < 0) continue;
                    if (!fd_valid(fd)) { pfd[i].revents = POLLERR; ready++; continue; }
                    // stdin: readable if a key is queued
                    if (fd == 0 && (pfd[i].events & POLLIN) && keyboard_has_input())
                        pfd[i].revents = (int16_t)(pfd[i].revents | POLLIN);
                    // stdout/stderr always writable
                    if ((fd == 1 || fd == 2) && (pfd[i].events & POLLOUT))
                        pfd[i].revents = (int16_t)(pfd[i].revents | POLLOUT);
                    // other open fds: treat as writable
                    if (fd > 2 && (pfd[i].events & POLLOUT))
                        pfd[i].revents = (int16_t)(pfd[i].revents | POLLOUT);
                    if (pfd[i].revents) ready++;
                }
                return ready;
            };

            int ready = fill();
            if (ready > 0 || timeout == 0) return (uint32_t)ready;

            // Wait: sleep on hlt until key or rough timeout (PIT 100Hz)
            uint32_t start = timer_get_ticks();
            uint32_t need = (timeout < 0) ? 0xFFFFFFFFu
                          : (uint32_t)((timeout * (int)TIMER_HZ + 999) / 1000);
            if (need == 0) need = 1;
            __asm__ volatile ("sti");
            while (true) {
                ready = fill();
                if (ready > 0) return (uint32_t)ready;
                if (timeout >= 0 && (uint32_t)(timer_get_ticks() - start) >= need)
                    return 0;
                __asm__ volatile ("hlt");
            }
        }

        case SYS_SELECT:
            // Too niche for now — return 0 (timeout, nothing ready)
            return 0;

        case SYS_FCNTL:
        case SYS_FCNTL64:
            return (uint32_t)sys_fcntl((int)a1, (int)a2, a3);

        case SYS_WRITEV:
            return (uint32_t)sys_writev((int)a1, (const Iovec*)a2, (int)a3);

        case SYS_READV:
            return (uint32_t)sys_readv((int)a1, (const Iovec*)a2, (int)a3);

        case SYS_MMAP2:
            return (uint32_t)sys_mmap2(a1, a2, a3, a4, (int)a5, a6);

        case SYS_MUNMAP:
            return (uint32_t)sys_munmap(a1, a2);

        case SYS_MPROTECT:
            return (uint32_t)sys_mprotect(a1, a2, a3);

        case SYS_UNAME:
            return (uint32_t)sys_uname((NewUtsname*)a1);

        case SYS_GETPID:
        case SYS_GETTID:
        case SYS_GETPPID:
            return 1;

        case SYS_GETUID:
        case SYS_GETEUID:
        case SYS_GETGID:
        case SYS_GETEGID:
        case SYS_GETUID32:
        case SYS_GETGID32:
        case SYS_GETEUID32:
        case SYS_GETEGID32:
            return 0; // single-user hobby OS: always root

        // set*id — no real multiuser, pretend success (BusyBox wants this)
        // Both legacy and i386 *32 variants (busybox-i686 uses setgid32=214).
        case SYS_SETUID:
        case SYS_SETGID:
        case SYS_SETREUID:
        case SYS_SETREGID:
        case SYS_SETRESUID:
        case SYS_SETRESGID:
        case SYS_SETFSUID:
        case SYS_SETFSGID:
        case SYS_SETSID:
        case SYS_SETUID32:
        case SYS_SETGID32:
        case SYS_SETREUID32:
        case SYS_SETREGID32:
        case SYS_SETRESUID32:
        case SYS_SETRESGID32:
            return 0;

        case SYS_UMASK:
            // a1 = new mask; return old mask (pretend previous was 0022)
            return 0022;

        case SYS_SET_THREAD_AREA:
            return (uint32_t)sys_set_thread_area((UserDesc*)a1);

        case SYS_SET_TID_ADDRESS:
            return 1; // return tid

        case SYS_FSTAT64:
            return (uint32_t)sys_fstat64((int)a1, (Stat64*)a2);

        case SYS_STAT64:
            return (uint32_t)sys_stat64((const char*)a1, (Stat64*)a2);

        case SYS_ACCESS:
            return (uint32_t)sys_access((const char*)a1, (int)a2);

        case SYS_CHDIR:
            return (uint32_t)sys_chdir((const char*)a1);

        case SYS_MKDIR:
            return (uint32_t)sys_mkdir((const char*)a1, a2);

        case SYS_GETCWD:
            return (uint32_t)sys_getcwd((char*)a1, a2);

        case SYS_GETDENTS64:
            return (uint32_t)sys_getdents64((int)a1, (void*)a2, a3);

        case SYS_UGETRLIMIT:
            // return soft/hard = -1 (unlimited) for common resources.
            // struct rlimit — ЭТО ВСЕГО 2 поля (rlim_cur, rlim_max) = 8 байт,
            // не 4! Раньше тут писалось r[0..3] (16 байт) в 8-байтный буфер
            // вызывающего — переполнение затирало всё, что лежало сразу за
            // структурой на стеке (у статического glibc там нередко лежит
            // сохранённая копия stack-protector canary, что и вызывало
            // ложные "*** stack smashing detected ***" при getrlimit(),
            // независимо от того, что за программа его вызвала).
            if (a2) {
                uint32_t* r = (uint32_t*)a2;
                r[0] = 0xFFFFFFFF; r[1] = 0xFFFFFFFF;
            }
            return 0;

        case SYS_FUTEX:
            // wake/wait stub: pretend success
            return 0;

        case SYS_NANOSLEEP: {
            // a1 = const struct timespec* req { tv_sec, tv_nsec }
            // Busy-wait on PIT ticks (no scheduler yet). Good enough for short sleeps.
            if (!a1) return (uint32_t)-EINVAL;
            uint32_t* req = (uint32_t*)a1;
            uint32_t sec  = req[0];
            uint32_t nsec = req[1];
            // All 32-bit math — avoids __udivdi3 on i386 without libgcc.
            if (sec > 0xFFFFFFFFu / (uint32_t)TIMER_HZ)
                sec = 0xFFFFFFFFu / (uint32_t)TIMER_HZ;
            uint32_t need = sec * (uint32_t)TIMER_HZ
                          + (nsec / (1000000000u / (uint32_t)TIMER_HZ));
            if (nsec % (1000000000u / (uint32_t)TIMER_HZ)) need++;
            if (need == 0) need = 1;
            uint32_t start = timer_get_ticks();
            while ((uint32_t)(timer_get_ticks() - start) < need) {
                __asm__ __volatile__("sti; hlt");
            }
            return 0;
        }

        case SYS_CLOCK_GETTIME: {
            // a1 = clockid_t (ignored: all clocks = monotonic PIT)
            // a2 = struct timespec { time_t tv_sec; long tv_nsec; }  — 32-bit time_t
            if (!a2) return (uint32_t)-EINVAL;
            uint32_t t = timer_get_ticks();
            uint32_t* ts = (uint32_t*)a2;
            ts[0] = t / (uint32_t)TIMER_HZ;                                      // sec
            ts[1] = (t % (uint32_t)TIMER_HZ) * (1000000000u / (uint32_t)TIMER_HZ); // nsec
            return 0;
        }

        case SYS_CLOCK_GETTIME64: {
            // musl i386 calls this first (nr 403).
            // a1 = clockid_t (ignored)
            // a2 = struct __kernel_timespec {
            //         int64_t tv_sec;
            //         long long tv_nsec;   // also 64-bit on Linux
            //     };
            if (!a2) return (uint32_t)-EINVAL;
            uint32_t t = timer_get_ticks();
            uint32_t sec  = t / (uint32_t)TIMER_HZ;
            uint32_t nsec = (t % (uint32_t)TIMER_HZ) * (1000000000u / (uint32_t)TIMER_HZ);
            uint32_t* ts = (uint32_t*)a2;
            ts[0] = sec;   // tv_sec low
            ts[1] = 0;     // tv_sec high
            ts[2] = nsec;  // tv_nsec low
            ts[3] = 0;     // tv_nsec high
            return 0;
        }

        case SYS_GETTIMEOFDAY: {
            // a1 = struct timeval* { time_t tv_sec; suseconds_t tv_usec; }
            // a2 = struct timezone* (ignored)
            if (!a1) return (uint32_t)-EINVAL;
            uint32_t t = timer_get_ticks();
            uint32_t* tv = (uint32_t*)a1;
            tv[0] = t / (uint32_t)TIMER_HZ;                                     // sec
            tv[1] = (t % (uint32_t)TIMER_HZ) * (1000000u / (uint32_t)TIMER_HZ); // usec
            return 0;
        }

        case SYS_DUP:
            if (!fd_valid((int)a1)) return -EBADF;
            {
                int n = fd_alloc();
                if (n < 0) return -EMFILE;
                g_fds[n] = g_fds[a1];
                return n;
            }

        case SYS_DUP2:
            if (!fd_valid((int)a1)) return -EBADF;
            if ((int)a2 < 0 || (int)a2 >= MAX_FDS) return -EBADF;
            if (a1 == a2) return (int)a2;
            g_fds[a2] = g_fds[a1];
            return (int)a2;

        // Not implemented — clear -ENOSYS
        case SYS_FORK:
        case SYS_CLONE:
        case SYS_EXECVE:
        case SYS_WAITPID:
        case SYS_WAIT4:
        case SYS_PIPE:
        case SYS_SOCKETCALL:
        case SYS_KILL:
        case SYS_PTRACE:
            return (uint32_t)-ENOSYS;

        // ─── Graphics (vga_gfx full pass-through) ────────────────────────────
        case SYS_GFX_ENTER_MODE13:
            vga_enter_mode13();
            return 0;
        case SYS_GFX_ENTER_640x480:
            vga_enter_mode_640x480();
            return 0;
        case SYS_GFX_EXIT_TEXT:
            vga_exit_to_text();
            return 0;
        case SYS_GFX_GET_WIDTH:
            return (uint32_t)gfx_get_width();
        case SYS_GFX_GET_HEIGHT:
            return (uint32_t)gfx_get_height();
        case SYS_GFX_SET_PALETTE:
            // a1=idx, a2=r, a3=g, a4=b  (0-63)
            gfx_set_palette((uint8_t)a1, (uint8_t)a2, (uint8_t)a3, (uint8_t)a4);
            return 0;
        case SYS_GFX_INIT_PALETTE:
            gfx_init_palette();
            return 0;
        case SYS_GFX_PUT_PIXEL:
            gfx_put_pixel((int)a1, (int)a2, (uint8_t)a3);
            return 0;
        case SYS_GFX_CLEAR:
            gfx_clear((uint8_t)a1);
            return 0;
        case SYS_GFX_FILL_RECT:
            // a1=x a2=y a3=w a4=h a5=color
            gfx_fill_rect((int)a1, (int)a2, (int)a3, (int)a4, (uint8_t)a5);
            return 0;
        case SYS_GFX_DRAW_RECT:
            gfx_draw_rect((int)a1, (int)a2, (int)a3, (int)a4, (uint8_t)a5);
            return 0;
        case SYS_GFX_DRAW_HLINE:
            gfx_draw_hline((int)a1, (int)a2, (int)a3, (uint8_t)a4);
            return 0;
        case SYS_GFX_DRAW_VLINE:
            gfx_draw_vline((int)a1, (int)a2, (int)a3, (uint8_t)a4);
            return 0;
        case SYS_GFX_DRAW_LINE:
            // a1=x0 a2=y0 a3=x1 a4=y1 a5=color
            gfx_draw_line((int)a1, (int)a2, (int)a3, (int)a4, (uint8_t)a5);
            return 0;
        case SYS_GFX_FLIP:
            gfx_flip();
            return 0;
        case SYS_GFX_SET_CLIP:
            gfx_set_clip((int)a1, (int)a2, (int)a3, (int)a4);
            return 0;
        case SYS_GFX_CLEAR_CLIP:
            gfx_clear_clip();
            return 0;
        case SYS_GFX_DRAW_CHAR:
            // a1=x a2=y a3=c a4=fg a5=bg
            gfx_draw_char((int)a1, (int)a2, (char)a3, (uint8_t)a4, (uint8_t)a5);
            return 0;
        case SYS_GFX_DRAW_TEXT: {
            // a1=x a2=y a3=str_ptr a4=fg a5=bg
            if (!a3) return (uint32_t)-EFAULT;
            gfx_draw_text((int)a1, (int)a2, (const char*)a3, (uint8_t)a4, (uint8_t)a5);
            return 0;
        }
        case SYS_GFX_CURSOR_SHOW:
            gfx_cursor_show();
            return 0;
        case SYS_GFX_CURSOR_HIDE:
            gfx_cursor_hide();
            return 0;
        case SYS_GFX_CURSOR_MOVE:
            gfx_cursor_move((int)a1, (int)a2);
            return 0;
        case SYS_GFX_CURSOR_GET_X:
            return (uint32_t)gfx_cursor_get_x();
        case SYS_GFX_CURSOR_GET_Y:
            return (uint32_t)gfx_cursor_get_y();
        case SYS_GFX_CURSOR_UPDATE_POS:
            gfx_cursor_update_pos();
            return 0;
        case SYS_GFX_ACTIVE:
            return vga_gfx_active() ? 1u : 0u;
        case SYS_GFX_CURSOR_SET_LOCKED:
            gfx_cursor_set_locked(a1 != 0);
            return 0;
        case SYS_GFX_LFB_AVAILABLE:
            return vga_lfb_available() ? 1u : 0u;
        case SYS_GFX_GET_BACKBUFFER:
            return (uint32_t)gfx_get_backbuffer();

        // ─── Keyboard (non-blocking) + Mouse ────────────────────────────────
        case SYS_KB_GETKEY:
            return (uint32_t)get_key();   /* 0 if no key pending */
        case SYS_KB_GET_STATE:
            return keyboard_get_state();
        case SYS_YIELD:
            /* Sleep until next interrupt (key/mouse/timer). */
            __asm__ __volatile__("sti; hlt");
            return 0;

        case SYS_MOUSE_GET_X:
            return (uint32_t)mouse_state.x;
        case SYS_MOUSE_GET_Y:
            return (uint32_t)mouse_state.y;
        case SYS_MOUSE_GET_BUTTONS:
            return (uint32_t)mouse_state.buttons;
        case SYS_MOUSE_GET_DX: {
            int dx = mouse_state.raw_dx;
            mouse_state.raw_dx = 0;
            return (uint32_t)dx;
        }
        case SYS_MOUSE_GET_DY: {
            int dy = mouse_state.raw_dy;
            mouse_state.raw_dy = 0;
            return (uint32_t)dy;
        }

        case ALLIVE_TEST:
            return (uint32_t)allive_test();

        /* ── musl CRT stubs (prevent hang on -ENOSYS during startup) ── */
        case SYS_READLINKAT:
            return (uint32_t)-ENOENT;

        case SYS_SET_ROBUST_LIST:
        case SYS_GET_ROBUST_LIST:
            return 0;

        case SYS_GETRANDOM: {
            /* a1 = buf, a2 = buflen, a3 = flags */
            if (!a1 || a2 == 0)
                return 0;
            uint8_t *buf = (uint8_t *)a1;
            uint32_t n = (uint32_t)a2;
            for (uint32_t i = 0; i < n; i++)
                buf[i] = (uint8_t)((i * 17u) + 0xA5u);
            return n;
        }

        case SYS_STATX:
            return (uint32_t)-ENOSYS;

        default:
            return (uint32_t)-ENOSYS;
    }
}