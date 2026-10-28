#include "interrupts.h"
#include "../lib/screen.h"
#include "panic.h"
#include "../drivers/disk.h" // Для inb, outb
#include "../drivers/serial.h"
#include "mouse.h"
#include "keyboard.h"
#include "elf_loader.h"

// IDT
idt_entry idt[256];
idt_ptr idtp;

// Global monotonic tick counter (IRQ0 / PIT). Starts at 0 after timer_init().
volatile uint32_t g_ticks = 0;

// ─── PIT (Programmable Interval Timer) ───────────────────────────────────────
// Channel 0, mode 3 (square wave), divisor = 1193182 / TIMER_HZ.
// Without this the BIOS leaves ~18.2 Hz; we want a known rate for time syscalls.
void timer_init(void) {
    const uint32_t divisor = 1193182u / (uint32_t)TIMER_HZ;
    // Command: channel 0 | lobyte/hibyte | mode 3 | binary
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
    g_ticks = 0;
    serial_write("[TIMER] PIT ");
    // tiny decimal dump of TIMER_HZ
    {
        char buf[8]; int i = 0; int hz = TIMER_HZ;
        if (hz == 0) buf[i++] = '0';
        else { char tmp[8]; int n = 0; while (hz) { tmp[n++] = '0' + (hz % 10); hz /= 10; }
               while (n--) buf[i++] = tmp[n]; }
        buf[i] = 0;
        serial_write(buf);
    }
    serial_write(" Hz, ticks armed\n");
}

uint32_t timer_get_ticks(void) {
    return g_ticks;
}

// Коды ошибок для исключений
const char *exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point Exception", "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
    "Virtualization Exception", "Control Protection Exception", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved"
};

// **********************************************
// ===== Объявления обработчиков из ассемблера (ISR/IRQ) - ИСПРАВЛЕНО =====
// **********************************************
extern "C" {
    // 32 Обработчика исключений (ISR 0-31)
    void isr0(); void isr1(); void isr2(); void isr3(); void isr4(); void isr5();
    void isr6(); void isr7(); void isr8(); void isr9(); void isr10(); void isr11();
    void isr12(); void isr13(); void isr14(); void isr15(); void isr16(); void isr17();
    void isr18(); void isr19(); void isr20(); void isr21(); void isr22(); void isr23();
    void isr24(); void isr25(); void isr26(); void isr27(); void isr28(); void isr29();
    void isr30(); void isr31(); 
    
    // 16 Обработчиков прерываний (IRQ 0-15 -> INT 32-47)
    void irq0(); void irq1(); void irq2(); void irq3(); void irq4(); void irq5(); 
    void irq6(); void irq7(); void irq8(); void irq9(); void irq10(); void irq11(); 
    void irq12(); void irq13(); void irq14(); void irq15(); 

    // Внешняя функция на ассемблере для загрузки IDT
    void idt_flush(uint32_t idt_ptr_base);
}
// **********************************************
// ===== IDT Initialization and Gates =====
// **********************************************

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].offset_high = (base >> 16) & 0xFFFF;
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].flags = flags; 
}

// Переназначение PIC
void irq_remap() {
    #define PIC1_COMMAND    0x20
    #define PIC1_DATA       0x21
    #define PIC2_COMMAND    0xA0
    #define PIC2_DATA       0xA1
    
    // Инициализация (ICW1)
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    
    // Смещение вектора (ICW2): IRQ 0-7 -> INT 32-39, IRQ 8-15 -> INT 40-47
    outb(PIC1_DATA, 0x20); 
    outb(PIC2_DATA, 0x28); 
    
    // Подключение (ICW3)
    outb(PIC1_DATA, 0x04); 
    outb(PIC2_DATA, 0x02); 
    
    // Режим работы (ICW4)
    outb(PIC1_DATA, 0x01); 
    outb(PIC2_DATA, 0x01); 
    
    // Маскировка (OCW1): Отключаем все IRQ
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    // Включаем IRQ 1 (клавиатура), IRQ 12 (мышь) и IRQ 2 (cascade slave->master,
    // без этого IRQ8-15, в т.ч. IRQ12, до CPU не доходят вообще)
    irq_clear_mask(0);  /* timer */
    irq_clear_mask(1);
    irq_clear_mask(12);
    irq_clear_mask(2);

    serial_write("[PIC] mask1=");
    serial_hex8(inb(PIC1_DATA));
    serial_write(" mask2=");
    serial_hex8(inb(PIC2_DATA));
    serial_write("\n");
}

void init_idt() {
    idtp.limit = (sizeof(idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;

    // Очистка
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0; idt[i].offset_high = 0;
        idt[i].selector = 0; idt[i].zero = 0; idt[i].flags = 0;
    }

    // Установка ворот для исключений (ISR 0-31)
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E); 
    idt_set_gate(8, (uint32_t)isr8, 0x08, 0x8E); // Double Fault — страховка от тихого triple fault
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E); // General Protection Fault
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E); // Page Fault
    // ... здесь должны быть все остальные isr
    
    // Установка ворот для IRQs (IRQ 0-15 -> INT 32-47)
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E); // IRQ 0 (Timer)
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E); // IRQ 1 (Keyboard)
    idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E); // IRQ 2 (cascade) — страховка
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E); // IRQ 12 (Mouse)
    // ... здесь должны быть все остальные irq

    // Переназначение PIC
    irq_remap();
    
    // Сначала загружаем IDT (lidt), потом включаем прерывания
    idt_flush((uint32_t)&idtp);
    __asm__ volatile ("sti");
}

// **********************************************
// ===== Обработчики =====
// **********************************************

// Обработчик исключений (0-31)
extern "C" void isr_handler(registers regs) {
    if (regs.int_no < 32) {
        print("EXCEPTION: ");
        print(exception_messages[regs.int_no]);
        
        // Показать EIP (указатель инструкции)
        print(" (EIP: ");
        print_hex_byte((regs.eip >> 24) & 0xFF);
        print_hex_byte((regs.eip >> 16) & 0xFF);
        print_hex_byte((regs.eip >> 8) & 0xFF);
        print_hex_byte(regs.eip & 0xFF);
        println(")");
        
        panic(exception_messages[regs.int_no]);
    }
}

// Обработчик прерываний IRQ (32-47)
extern "C" void irq_handler(registers regs) {
    #define PIC_EOI 0x20

    // Прямая запись в видеопамять — без портов/циклов, не может зависнуть.
    // Показывает hex номер последнего сработавшего вектора в правом нижнем углу.
    // {
    //     volatile uint16_t* dbg = (volatile uint16_t*)0xB8000;
    //     const char* hexd = "0123456789ABCDEF";
    //     uint8_t v = (uint8_t)regs.int_no;
    //     dbg[24*80 + 78] = (uint16_t)(0x0F00 | (uint8_t)hexd[(v >> 4) & 0xF]);
    //     dbg[24*80 + 79] = (uint16_t)(0x0F00 | (uint8_t)hexd[v & 0xF]);
    // }

    // Отправка EOI (End of Interrupt) на Slave, если IRQ 8-15
    if (regs.int_no >= 40) { 
        outb(0xA0, PIC_EOI);
    }
    // Отправка EOI на Master
    outb(0x20, PIC_EOI); 

    switch (regs.int_no) {
        case 32: { // IRQ 0: Timer (PIT)
            g_ticks++;
            // ВНИМАНИЕ: из-за отсутствия push %ds в общем ISR/IRQ asm-шаблоне
            // (interrupts_asm.s) структура registers сдвинута на одно слово для
            // ЛЮБОГО прерывания: regs.eip в реальности содержит CS, а настоящий
            // EIP лежит в regs.err_code. Это отдельный баг за пределами текущей
            // задачи — здесь просто читаем правильное поле, не трогая asm.
            // Debug dump ~5 times/sec while an ELF is running (at TIMER_HZ=100).
            if (g_elf_running && (g_ticks % 20) == 0) {
                uint32_t real_eip = regs.err_code;
                // Тот же "сдвиг на одно слово" (см. коммент выше) означает,
                // что CS реально лежит в regs.eip, а EFLAGS — в regs.cs.
                // Печатаем оба для перекрёстной проверки: CS должен всегда
                // быть 0x00000008 (плоская модель, единое кольцо 0, ни
                // одного far jump/call gate ни разу не происходит) — если
                // тут что-то другое, значит наше предположение о сдвиге
                // само неверное и eip читаем не оттуда.
                uint32_t real_cs     = regs.eip;
                uint32_t real_eflags = regs.cs;
                serial_write("[tick eip=0x");
                serial_hex8((uint8_t)(real_eip >> 24));
                serial_hex8((uint8_t)(real_eip >> 16));
                serial_hex8((uint8_t)(real_eip >> 8));
                serial_hex8((uint8_t)(real_eip));
                serial_write(" cs=0x");
                serial_hex8((uint8_t)(real_cs >> 24));
                serial_hex8((uint8_t)(real_cs >> 16));
                serial_hex8((uint8_t)(real_cs >> 8));
                serial_hex8((uint8_t)(real_cs));
                serial_write(" eflags=0x");
                serial_hex8((uint8_t)(real_eflags >> 24));
                serial_hex8((uint8_t)(real_eflags >> 16));
                serial_hex8((uint8_t)(real_eflags >> 8));
                serial_hex8((uint8_t)(real_eflags));
                serial_write("]\n");
            }
            break;
        }
        case 33: // IRQ 1: Keyboard
            // Always drain the PS/2 data port (keyboard_on_irq). Never call get_key()
            // here — it used to pop the software queue first and leave the hardware
            // byte unread → IRQ storm / freeze.
            keyboard_on_irq();
            if (g_elf_running) {
                // Hard Ctrl+C: kill the ELF even if it never calls read().
                // Inspect only the software queue (never the PS/2 data port from
                // IRQ context); the first key is Ctrl+C iff the user pressed it.
                uint8_t k = keyboard_check_ctrl_c();
                if (k == CHAR_CTRL_C) {
                    __asm__ volatile ("cli");
                    serial_write("[IRQ1] Ctrl+C → kill ELF\n");
                    // visual feedback
                    print("^C\n");
                    // 130 = 128 + SIGINT; never returns
                    elf_request_exit(130);
                }
            }
            break;
        case 44: // IRQ 12: Mouse
            mouse_handle_packet();
            break;
    }
}

void irq_set_mask(uint8_t irq_line) {
    uint16_t port = (irq_line < 8) ? 0x21 : 0xA1;
    uint8_t value = inb(port) | (1 << (irq_line % 8));
    outb(port, value);        
}

void irq_clear_mask(uint8_t irq_line) {
    uint16_t port = (irq_line < 8) ? 0x21 : 0xA1;
    uint8_t value = inb(port) & ~(1 << (irq_line % 8));
    outb(port, value);        
}