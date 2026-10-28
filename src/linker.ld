OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    /* Секция кода и только для чтения: Read + Execute */
    .text : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
        *(.rodata)
    } :text

    /* Секция данных: Read + Write */
    .data : ALIGN(4K)
    {
        *(.data)
    } :data

    /* Секция неинициализированных данных: Read + Write */
    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    } :data

    . = ALIGN(4K);
    _kernel_end = .;   /* конец образа ядра — сюда смотрит elf_loader.cpp,
                           чтобы выбрать безопасный адрес загрузки программ */

    /DISCARD/ : { *(.comment) *(.eh_frame) }
}

/* Определение сегментов для правильных прав доступа */
PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5); /* R-X */
    data PT_LOAD FLAGS(6);               /* RW- */
}