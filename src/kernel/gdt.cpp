#include "gdt.h"

#define GDT_ENTRIES 9   // 0=null,1=kcode,2=kdata,3-5=резерв,6-8=TLS

static GdtEntry gdt[GDT_ENTRIES];
static GdtPtr   gp;

extern "C" void gdt_flush(uint32_t gdt_ptr_addr);

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].base_high   = (uint8_t)((base >> 24) & 0xFF);
    gdt[i].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[i].granularity = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access      = access;
}

void gdt_init() {
    gdt_set(0, 0, 0, 0, 0); // null-дескриптор — обязателен по архитектуре

    // Точь-в-точь то же самое, что уже поставил boot/stage2.asm: плоский
    // 4 ГБ код/данные, кольцо 0, гранулярность 4К + 32-бит операнды (0xC0).
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xC0); // 0x08: код
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xC0); // 0x10: данные

    gdt_set(3, 0, 0, 0, 0); // резерв
    gdt_set(4, 0, 0, 0, 0); // резерв
    gdt_set(5, 0, 0, 0, 0); // резерв

    gdt_set(6, 0, 0, 0, 0); // TLS-слоты — пустые, заполнит set_thread_area
    gdt_set(7, 0, 0, 0, 0);
    gdt_set(8, 0, 0, 0, 0);

    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint32_t)&gdt;
    gdt_flush((uint32_t)&gp);
}

uint16_t gdt_set_tls(int index, uint32_t base) {
    if (index < 6 || index > 8) return 0;
    // DPL=3 (биты 5-6 access-байта) — НЕ потому что мы реально работаем в
    // кольце 3 (у нас везде кольцо 0), а потому что userland-код (musl)
    // всегда считает селектор как (index*8+3)|3 — с RPL=3. Правило загрузки
    // сегмента: MAX(CPL,RPL) <= DPL. Если тут поставить DPL=0 как у
    // кода/данных ядра, то MAX(0,3)=3 <= 0 — ложь, и любой mov %ax,%gs с
    // таким селектором тут же уронит программу в #GP. DPL=3 с CPL=0
    // (мы всегда в кольце 0) наоборот разрешено всегда — более
    // привилегированный код может грузить менее привилегированный сегмент.
    gdt_set(index, base, 0xFFFFF, 0xF2, 0xC0);
    return (uint16_t)((index * 8) | 3);
}
