// Размер полей структуры utsname в Linux ABI равен 65 байтам
struct os_info_raw {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

// Функция подсчета длины строки (чтобы избежать libc)
unsigned long mystrlen(const char *str) {
    unsigned long len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

// Прямой вызов sys_write через inline-ассемблер
void sys_write(int fd, const char *buf, unsigned long count) {
#if defined(__x86_64__)
    asm volatile (
        "mov $1, %%rax\n\t"  // sys_write = 1 в x86_64
        "syscall\n\t"
        :
        : "D"(fd), "S"(buf), "d"(count)
        : "rax", "rcx", "r11", "memory"
    );
#elif defined(__i386__)
    asm volatile (
        "mov $4, %%eax\n\t"  // sys_write = 4 в i386
        "int $0x80\n\t"
        :
        : "b"(fd), "c"(buf), "d"((unsigned int)count)
        : "eax", "memory"
    );
#endif
}

void print_str(const char *str) {
    sys_write(1, str, mystrlen(str));
}

void print_field(const char *label, const char *value) {
    print_str(label);
    print_str(value);
    print_str("\n");
}

// Прямой вызов sys_exit (для предотвращения SegFault при выходе)
void sys_exit(int code) {
#if defined(__x86_64__)
    asm volatile ("mov $60, %%rax\n\t" "mov %0, %%rdi\n\t" "syscall" : : "r"((long)code) : "rax", "rdi");
#elif defined(__i386__)
    asm volatile ("mov $1, %%eax\n\t" "mov %0, %%ebx\n\t" "int $0x80" : : "r"(code) : "eax", "ebx");
#endif
    while(1); // Защитный цикл, если ядро не успело убить поток
}

// Точка входа для флага -nostdlib
void _start(void) {
    struct os_info_raw info;
    long result;

#if defined(__x86_64__)
    asm volatile (
        "mov $63, %%rax\n\t" // sys_uname = 63 в x86_64
        "syscall\n\t"
        : "=a"(result)
        : "D"(&info)
        : "rcx", "r11", "memory"
    );
#elif defined(__i386__)
    asm volatile (
        "mov $122, %%eax\n\t" // sys_uname = 122 в i386
        "int $0x80\n\t"
        : "=a"(result)
        : "b"(&info)
        : "memory"
    );
#endif

    if (result < 0) {
        print_str("Error: uname syscall failed\n");
        sys_exit(1);
    }

    print_str("OS Information:\n");
    print_str("---------------\n");
    print_field("Kernel:  ", info.sysname);
    print_field("Host:    ", info.nodename);
    print_field("Release: ", info.release);
    print_field("Arch:    ", info.machine);

    sys_exit(0);
}
