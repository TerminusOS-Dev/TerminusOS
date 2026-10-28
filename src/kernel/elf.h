#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// ─── Минимальный набор структур ELF32 (только то, что нужно для статических,
// не-PIE, i386 исполняемых файлов — ET_EXEC). Никакой поддержки динамической
// линковки/PIE (ET_DYN) на этом этапе — сознательно, чтобы не открывать
// огромный отдельный кусок сложности (перемещения/relocations) раньше времени.

#define EI_NIDENT 16

struct Elf32_Ehdr {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf32_Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

#define ET_EXEC     2      // обычный статический исполняемый файл (не PIE)
#define EM_386      3      // архитектура: x86 (i386/i686) — то, что уже собирает твой тулчейн
#define PT_LOAD     1      // сегмент, который нужно грузить в память

#endif
