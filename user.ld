OUTPUT_FORMAT("elf32-i386")
ENTRY(_start)

SECTIONS
{
    /* Выставляем строго нужный адрес для ядра */
    . = 0x4000000;

    /* Позволяет glibc/musl найти ELF Header, убирает ошибку __ehdr_start */
    PROVIDE(__ehdr_start = .);

    .text : ALIGN(4) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
    }

    /* Секции инициализации (для конструкторов C++ и libc) */
    .init_array : ALIGN(4) {
        PROVIDE(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE(__preinit_array_end = .);
        
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE(__init_array_end = .);
    }

    .fini_array : ALIGN(4) {
        PROVIDE(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE(__fini_array_end = .);
    }

    .data : ALIGN(4) {
        *(.data .data.*)
    }

    .bss : ALIGN(4) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /* 
     * СПАСАТЕЛЬНЫЙ КРУГ ОТ GCC (crtbeginT.o)
     * Мы не выкидываем .eh_frame в /DISCARD/, потому что GCC намертво 
     * вшил в него ссылки. Мы просто кладем его и другой мусор В КОНЕЦ,
     * чтобы они не ломали адрес 0x4000000 и не вызывали ошибку below LOAD_BASE.
     */
    .eh_frame : { *(.eh_frame) }
    .comment  : { *(.comment) }
    .note     : { *(.note.*) }
}