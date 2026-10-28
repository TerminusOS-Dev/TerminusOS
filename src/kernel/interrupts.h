#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>
#include <stdbool.h>

// Объявления ассемблерных стабов для 32 исключений (ISR 0-31)
#define DECLARE_ISR(n) extern "C" void isr##n(void);
DECLARE_ISR(0)
// Для полной реализации необходимо объявить isr1-isr31
DECLARE_ISR(31)

// Объявления ассемблерных стабов для 16 прерываний (IRQ 0-15)
#define DECLARE_IRQ(n) extern "C" void irq##n(void);
DECLARE_IRQ(0)
DECLARE_IRQ(1) // IRQ 1 (клавиатура)
// Для полной реализации необходимо объявить irq2-irq15

// Структура для дескриптора IDT
struct idt_entry {
    uint16_t offset_low;    // 0..15 бит смещения обработчика
    uint16_t selector;      // Селектор сегмента кода
    uint8_t zero;           // Всегда 0
    uint8_t flags;          // Флаги (P, DPL, S, Gate Type)
    uint16_t offset_high;   // 16..31 бит смещения обработчика
} __attribute__((packed));

// Структура для указателя IDT (IDTR)
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

// Структура для сохранения регистров в стеке при прерывании
struct registers {
    uint32_t ds;                                     
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; 
    uint32_t int_no, err_code;                       
    uint32_t eip, cs, eflags, useresp, ss;           
};


extern idt_entry idt[256];

// PIT frequency used by timer_init() (Hz). Keep in sync with syscall time math.
#define TIMER_HZ 100

// Monotonic tick counter, incremented on every PIT IRQ0.
// uint32_t is enough: at TIMER_HZ=100 wraps in ~497 days.
extern volatile uint32_t g_ticks;

extern "C" {
    void init_idt();
    void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
    void isr_handler(registers regs);
    void irq_handler(registers regs);

    void irq_set_mask(uint8_t irq_line);
    void irq_clear_mask(uint8_t irq_line);

    // Program PIT channel 0 for TIMER_HZ and start counting g_ticks.
    // Call once after init_idt() (sti already done).
    void timer_init(void);

    // Snapshot of g_ticks.
    uint32_t timer_get_ticks(void);
}

#endif // INTERRUPTS_H
