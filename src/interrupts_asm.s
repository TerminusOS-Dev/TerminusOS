# src/interrupts_asm.s
.section .text
.code32

# ********************************************************
# *********** Объявления глобальных символов ************
# ********************************************************
.global idt_flush 
.extern isr_handler
.extern irq_handler

# Объявления всех ISR (0-31)
.global isr0
.global isr1
.global isr2
.global isr3
.global isr4
.global isr5
.global isr6
.global isr7
.global isr8
.global isr9
.global isr10
.global isr11
.global isr12
.global isr13
.global isr14
.global isr15
.global isr16
.global isr17
.global isr18
.global isr19
.global isr20
.global isr21
.global isr22
.global isr23
.global isr24
.global isr25
.global isr26
.global isr27
.global isr28
.global isr29
.global isr30
.global isr31

# Объявления всех IRQ (0-15)
.global irq0
.global irq1
.global irq2
.global irq3
.global irq4
.global irq5
.global irq6
.global irq7
.global irq8
.global irq9
.global irq10
.global irq11
.global irq12
.global irq13
.global irq14
.global irq15

# ********************************************************
# ************ Код функций IDT и обработчиков ************
# ********************************************************

# IDT FLUSH (загрузка регистра IDTR)
.type idt_flush, @function
idt_flush:
    mov 4(%esp), %eax
    lidtl (%eax)
    ret

# GDT FLUSH (загрузка регистра GDTR + перезагрузка сегментных регистров)
# Селекторы 0x08/0x10 те же, что уже стоят — поэтому это "невидимая" замена
# GDT загрузчика на нашу собственную (расширенную под TLS), а не смена
# текущих сегментов на что-то другое.
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ljmp $0x08, $.gdt_flush_cs
.gdt_flush_cs:
    ret


# ISR 0
.type isr0, @function
isr0: 
    push $0
    pusha
    push $0 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 1
.type isr1, @function
isr1: 
    push $0
    pusha
    push $1 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 2
.type isr2, @function
isr2: 
    push $0
    pusha
    push $2
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 3
.type isr3, @function
isr3: 
    push $0
    pusha
    push $3
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 4
.type isr4, @function
isr4: 
    push $0
    pusha
    push $4
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 5
.type isr5, @function
isr5: 
    push $0
    pusha
    push $5
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 6
.type isr6, @function
isr6: 
    push $0
    pusha
    push $6
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 7
.type isr7, @function
isr7: 
    push $0
    pusha
    push $7
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 9
.type isr9, @function
isr9: 
    push $0
    pusha
    push $9
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 15
.type isr15, @function
isr15: 
    push $0
    pusha
    push $15 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 16
.type isr16, @function
isr16: 
    push $0
    pusha
    push $16 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 18
.type isr18, @function
isr18: 
    push $0
    pusha
    push $18
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 19
.type isr19, @function
isr19: 
    push $0
    pusha
    push $19 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 20
.type isr20, @function
isr20: 
    push $0
    pusha
    push $20 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 21
.type isr21, @function
isr21: 
    push $0
    pusha
    push $21 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 22
.type isr22, @function
isr22: 
    push $0
    pusha
    push $22 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 23
.type isr23, @function
isr23: 
    push $0
    pusha
    push $23 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 24
.type isr24, @function
isr24: 
    push $0
    pusha
    push $24 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 25
.type isr25, @function
isr25: 
    push $0
    pusha
    push $25 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 26
.type isr26, @function
isr26: 
    push $0
    pusha
    push $26 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 27
.type isr27, @function
isr27: 
    push $0
    pusha
    push $27 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 28
.type isr28, @function
isr28: 
    push $0
    pusha
    push $28 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 29
.type isr29, @function
isr29: 
    push $0
    pusha
    push $29 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret
    
# ISR 30
.type isr30, @function
isr30: 
    push $0
    pusha
    push $30 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 31
.type isr31, @function
isr31: 
    push $0
    pusha
    push $31 
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret


# ==================================
# ISRs С КОДОМ ОШИБКИ (8, 10-14, 17)
# ==================================
# Общий шаблон: pusha, push $INT_NO, call isr_handler, pop %eax, popa, add $4, %esp, iret

# ISR 8
.type isr8, @function
isr8: 
    pusha
    push $8
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 10
.type isr10, @function
isr10: 
    pusha
    push $10
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 11
.type isr11, @function
isr11: 
    pusha
    push $11
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 12
.type isr12, @function
isr12: 
    pusha
    push $12
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 13
.type isr13, @function
isr13: 
    pusha
    push $13
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 14
.type isr14, @function
isr14: 
    pusha
    push $14
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ISR 17
.type isr17, @function
isr17: 
    pusha
    push $17
    call isr_handler
    pop %eax
    popa
    add $4, %esp
    iret


# ==================================
# IRQs (32-47)
# ==================================
# Общий шаблон: push $0, pusha, push $INT_NO, call irq_handler, pop %eax, popa, add $4, %esp, iret

# IRQ 0 -> INT 32
.type irq0, @function
irq0: 
    push $32
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 1 -> INT 33
.type irq1, @function
irq1: 
    push $33
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 2 -> INT 34
.type irq2, @function
irq2: 
    push $34
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 3 -> INT 35
.type irq3, @function
irq3: 
    push $35
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 4 -> INT 36
.type irq4, @function
irq4: 
    push $36
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 5 -> INT 37
.type irq5, @function
irq5: 
    push $37
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 6 -> INT 38
.type irq6, @function
irq6: 
    push $38
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 7 -> INT 39
.type irq7, @function
irq7: 
    push $39
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 8 -> INT 40
.type irq8, @function
irq8: 
    push $40
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 9 -> INT 41
.type irq9, @function
irq9: 
    push $41
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 10 -> INT 42
.type irq10, @function
irq10: 
    push $42
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 11 -> INT 43
.type irq11, @function
irq11: 
    push $43
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 12 -> INT 44
.type irq12, @function
irq12: 
    push $44
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 13 -> INT 45
.type irq13, @function
irq13: 
    push $45
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 14 -> INT 46
.type irq14, @function
irq14: 
    push $46
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# IRQ 15 -> INT 47
.type irq15, @function
irq15: 
    push $47
    pusha
    push $0
    call irq_handler
    pop %eax
    popa
    add $4, %esp
    iret

# ==================================
# int 0x80 — syscall'ы
# Своя, отдельная от ISR/IRQ конвенция (не переиспользует их struct registers,
# чтобы не рисковать несовпадением раскладки с уже рабочим кодом выше).
# Классические регистры Linux i386: eax=номер сисколла, ebx/ecx/edx=аргументы.
# syscall_dispatch(num, arg1, arg2, arg3) — cdecl, возврат в eax.
# ==================================
.global isr128
.extern syscall_dispatch
.type isr128, @function
isr128:
    # Linux i386: eax=num, ebx/ecx/edx/esi/edi/ebp = arg1..arg6; возврат в eax.
    # ВАЖНО: настоящий int 0x80 в Linux сохраняет ВСЕ регистры, кроме eax
    # (возвращаемое значение). ebx/esi/edi/ebp — callee-saved по cdecl, их
    # автоматически спасает компилятор при вызове syscall_dispatch. Но ecx
    # и edx в cdecl — caller-saved, их НИКТО не обязан сохранять, и
    # syscall_dispatch (переходы, арифметика, вложенные вызовы) их почти
    # гарантированно затирает. Libc (musl) полагается на то, что ecx/edx
    # переживут syscall — раньше это было не так, что приводило к порче
    # указателей/счётчиков, хранящихся в ecx/edx поперёк syscall'а (мусорные
    # пути при chdir, зависания в malloc/stdio и т.п.). Восстанавливаем их
    # явно из уже сохранённых на стеке исходных значений аргументов.
    push %ebp
    push %edi
    push %esi
    # аргументы cdecl справа налево: arg6..arg1, num
    push %ebp              # arg6
    push %edi              # arg5
    push %esi              # arg4
    push %edx              # arg3
    push %ecx              # arg2
    push %ebx              # arg1
    push %eax              # num
    call syscall_dispatch
    # восстановить ebx/ecx/edx из ИСХОДНЫХ (до-syscall) значений аргументов,
    # которые всё ещё лежат в стеке нетронутыми (память аргументов cdecl-
    # вызовов callee не модифицирует)
    movl 4(%esp), %ebx      # исходный arg1 (ebx)
    movl 8(%esp), %ecx      # исходный arg2 (ecx)
    movl 12(%esp), %edx     # исходный arg3 (edx)
    add $28, %esp          # 7 args * 4
    pop %esi
    pop %edi
    pop %ebp
    iret
