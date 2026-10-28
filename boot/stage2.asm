; ============================================================
; stage2.asm — TerminusOS Stage 2 Bootloader
; ============================================================
; Загружается по адресу 0x7E00 (сразу после Stage 1 на 0x7C00),
; либо самим Stage 1 через диск (HDD/USB), либо BIOS целиком
; через El-Torito no-emulation boot (ISO/CD-ROM) — см. stage1.asm.
;
; Первые 4 байта этого файла — магическая метка 0xDEADC0DE,
; по которой Stage 1 узнаёт "Stage 2 уже в памяти". Реальный код
; начинается сразу после неё.
;
; Собрать (build.sh делает это сам):
;   nasm -f bin -D KERNEL_SECTORS=<N> stage2.asm -o stage2.bin
;   где N = (размер kernel.elf в байтах + 511) / 512
;
; Поддерживает ДВА способа чтения диска (когда чтение вообще
; требуется — см. ниже про автоопределение преднагрузки):
;   1) Extended LBA Read (ah=42h) — быстрый, чанками по 64KB.
;   2) CHS Read (ah=02h)          — fallback по сектору.
;
; Раскладка памяти:
;   0x7C00          : Stage 1 (MBR / El-Torito loader)
;   0x7E00          : Stage 2 (этот файл, начинается с метки)
;   0x9000          : Multiboot Info Structure
;   0x9F000         : Вершина стека
;   0x10000-0x9FFFF : Буфер с kernel.elf (преднагружен или читается)
;   0x100000+       : Ядро, скопированное ELF-загрузчиком
;
; Раскладка диска (см. stage1.asm):
;   Сектор  0      : stage1.bin
;   Секторы 1-65    : stage2.bin (с запасом)
;   Сектор  66+     : kernel.elf — выровнено так, что при полной
;                      преднагрузке BIOS (0x7C00 + 66*512 = 0x10000)
;                      ядро попадает РОВНО в ELF_BUFFER без копирования.
; ============================================================

    [bits 16]
    [org 0x7E00]

; --- Метка для Stage 1 (должна быть первой!) --------------------------
    dd 0xDEADC0DE

; --- Константы ----------------------------------------------------------
%define ELF_BUFFER  0x10000     ; Буфер для ELF файла ядра
%define MB_INFO     0x9000      ; Адрес Multiboot Info
%define STACK_TOP   0x9F000     ; Вершина стека в PM
%define KERN_LBA    66          ; LBA ядра: 0x7C00+66*512 = 0x10000

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 1024     ; fallback: 512KB, если не передано
%endif

; --- Точка входа Stage 2 (сразу после 4-байтной метки) -----------------
stage2_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov si, msg_hello
    call puts16

    call enable_a20
    call detect_memory
    call vbe_setup        ; Найти и включить VBE LFB 640x480 (если BIOS умеет)
                            ; — заменяет то, что раньше делал GRUB.

; --- Преднагружено ли ядро уже в RAM целиком (сценарий ISO/no-emul)? ---
    push es
    mov ax, 0x1000               ; ES:0 = 0x1000:0x0000 = физ. 0x10000
    mov es, ax
    cmp dword [es:0x0000], 0x464C457F   ; ELF magic 0x7F 'E' 'L' 'F'
    pop es
    je .kernel_already_loaded

    call get_geometry
    call load_kernel_elf
    jmp .pm_switch

.kernel_already_loaded:
    mov si, msg_preloaded
    call puts16

.pm_switch:
    ; Очистить текстовый экран от наших debug-сообщений — иначе их
    ; остатки/мигающий курсор могут проступать после того, как
    ; ядро возьмёт управление. vbe_setup больше не переключает видео
    ; (только спрашивает параметры режима), так что экран всегда
    ; в текстовом режиме и эту очистку можно делать безусловно.
    mov ax, 0x0003
    int 0x10

    ; --- Переход в Protected Mode ----------------------------------
    cli
    lgdt [gdt_ptr]

    mov eax, cr0
    or eax, 0x01
    mov cr0, eax

    jmp 0x08:pm_entry

; ============================================================
; ПОДПРОГРАММЫ (Real Mode)
; ============================================================

enable_a20:
    mov ax, 0x2401
    int 0x15
    jnc .done

    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al

.done:
    mov si, msg_a20
    call puts16
    ret

detect_memory:
    xor ax, ax
    mov bx, ax
    mov cx, ax
    mov dx, ax
    mov ax, 0xE801
    int 0x15
    jc .fallback

    ; Некоторые BIOS возвращают результат в AX/BX, другие в CX/DX —
    ; по конвенции используем CX/DX, если они ненулевые (надёжнее).
    test cx, cx
    jnz .use_cx_dx
    mov cx, ax
    mov dx, bx

.use_cx_dx:
    ; mem_upper (KB) = (CX, память 1-16MB) + (DX * 64, память выше 16MB)
    movzx eax, cx
    movzx ebx, dx
    shl ebx, 6                  ; DX блоков по 64KB -> в KB (*64)
    add eax, ebx
    mov [mem_upper], eax
    jmp .done

.fallback:
    mov dword [mem_upper], 65536

.done:
    mov word [mem_lower], 640
    mov si, msg_mem
    call puts16
    ret

; --- Определить поддержку расширений int13h, иначе геометрию -----------
; --- VBE: найти и включить 640x480 LFB режим (bpp>=24), если BIOS умеет ---
; Это то же самое, что раньше делал GRUB по запросу в multiboot-заголовке
; ядра (640x480x32). Результат сохраняется в vbe_* переменные и позже
; (в Protected Mode) копируется в Multiboot Info, чтобы ядро увидело его
; через уже готовый путь lfb_ready/vga_lfb_init().
%define VBE_INFO_BUF   0x6000      ; 512 байт под VbeInfoBlock
%define VBE_MODE_BUF   0x6200      ; 256 байт под ModeInfoBlock

vbe_setup:
    mov byte [vbe_ok], 0

    ; Запросить VBE2+ контрольную информацию (сигнатура "VBE2" на входе)
    mov word [VBE_INFO_BUF+0], 'VB'
    mov word [VBE_INFO_BUF+2], 'E2'

    push es
    xor ax, ax
    mov es, ax
    mov di, VBE_INFO_BUF
    mov ax, 0x4F00
    int 0x10
    pop es
    cmp ax, 0x004F
    jne .fail

    cmp dword [VBE_INFO_BUF], 'VESA'
    jne .fail

    ; VideoModePtr: offset на +14, segment на +16
    mov ax, [VBE_INFO_BUF+14]
    mov dx, [VBE_INFO_BUF+16]
    mov [vbe_modelist_off], ax
    mov [vbe_modelist_seg], dx

.scan_loop:
    push es
    mov es, [vbe_modelist_seg]
    mov si, [vbe_modelist_off]
    mov cx, [es:si]
    pop es
    cmp cx, 0xFFFF
    je .fail                        ; конец списка — подходящего режима нет

    add word [vbe_modelist_off], 2

    push cx
    push es
    xor ax, ax
    mov es, ax
    mov di, VBE_MODE_BUF
    mov ax, 0x4F01
    int 0x10
    pop es
    pop cx
    cmp ax, 0x004F
    jne .scan_loop                  ; этот режим прочитать не удалось

    cmp word [VBE_MODE_BUF+18], 640 ; XResolution
    jne .scan_loop
    cmp word [VBE_MODE_BUF+20], 480 ; YResolution
    jne .scan_loop
    movzx ax, byte [VBE_MODE_BUF+25]; BitsPerPixel
    cmp ax, 24
    jl .scan_loop

    mov ax, [VBE_MODE_BUF+0]        ; ModeAttributes
    test ax, 0x80                   ; бит7 = LFB доступен
    jz .scan_loop

    ; Нашли подходящий режим — НЕ переключаем железо (AX=4F02h) вообще!
    ; Просто запоминаем параметры из GET MODE INFO — этого достаточно,
    ; PhysBasePtr/pitch/bpp одинаковы независимо от того, активен ли
    ; режим сейчас. Реальное включение LFB остаётся за ядерным
    ; vbe_enable() (Bochs DISPI порты), как и раньше — экран всю
    ; загрузку остаётся в текстовом режиме, ничего не трогаем зря.

    movzx eax, word [VBE_MODE_BUF+18]
    mov [vbe_width], eax
    movzx eax, word [VBE_MODE_BUF+20]
    mov [vbe_height], eax
    movzx eax, word [VBE_MODE_BUF+16]
    mov [vbe_pitch], eax
    movzx eax, byte [VBE_MODE_BUF+25]
    mov [vbe_bpp], eax
    mov eax, [VBE_MODE_BUF+40]      ; PhysBasePtr
    mov [vbe_addr], eax

    mov byte [vbe_ok], 1
    mov si, msg_vbe_ok
    call puts16
    ret

.fail:
    mov byte [vbe_ok], 0
    mov si, msg_vbe_no
    call puts16
    ret

; --- Геометрия диска через древнюю (универсально поддерживаемую)
; функцию AH=0x08. БЕЗ проверки Extended LBA (AH=0x41/0x42) — именно
; она виснет/ребутит машину на реальном железе (подтверждено: BIOS
; здесь честно печатает "EXT LBA OK", но сам чанковый AH=0x42-путь
; в load_kernel_elf роняет систему в reset). Та же история и то же
; решение, что и в stage1.asm — только CHS.
get_geometry:
    push es
    push di
    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    pop di
    pop es
    jc .geom_fallback

    mov al, cl
    and al, 0x3F
    test al, al
    jz .geom_fallback             ; BIOS вернул 0 -> мусор, не доверяем
    movzx ax, al
    mov [spt], ax

    movzx ax, dh
    inc ax
    mov [heads], ax
    jmp .done

.geom_fallback:
    mov word [spt], 63
    mov word [heads], 16

.done:
    mov si, msg_geom_ok
    call puts16
    ret

; --- LBA -> CHS (cylinder может быть > 255 -> 10-бит CHS) ---------------
; in:  [cur_lba_lo]
; out: ch=cyl_low8, cl=sector(0-5)|cyl_high(6-7), dh=head
lba_to_chs:
    push ax
    push bx

    mov ax, [cur_lba_lo]
    xor dx, dx
    mov bx, [spt]
    div bx                       ; ax = lba/spt, dx = lba%spt
    inc dx
    mov cl, dl                   ; sector (биты 0-5, т.к. dl < 64)

    xor dx, dx
    mov bx, [heads]
    div bx                       ; ax = cylinder, dx = head
    mov dh, dl

    mov bl, ah                   ; bl = cylinder high byte (для >255)
    shl bl, 6
    or  cl, bl                   ; cl биты 6-7 = cyl high биты
    mov ch, al                   ; ch = cylinder low 8 бит

    pop bx
    pop ax
    ret

; --- Загрузить kernel.elf с диска (только если НЕ преднагружено) -------
load_kernel_elf:
    mov si, msg_loading
    call puts16

; ---------- CHS путь (единственный — см. комментарий у get_geometry) ---
.chs_mode:
    mov word [cur_lba_lo], KERN_LBA
    mov word [buf_seg],    0x1000   ; ES сегмент буфера (физ. 0x10000)
    mov word [buf_off],    0
    mov word [remaining],  KERNEL_SECTORS

.chs_loop:
    mov ax, [remaining]
    test ax, ax
    jz .read_done

    call lba_to_chs                  ; -> ch, cl, dh

    mov es, [buf_seg]
    mov bx, [buf_off]

    mov ax, 0x0201                   ; AH=02, AL=1 сектор
    mov dl, [boot_drive]
    int 0x13
    jc .disk_error

    ; Сдвинуть буфер на 512 байт, с переходом сегмента на границе 64KB
    add word [buf_off], 512
    jnc .no_seg_bump
    add word [buf_seg], 0x1000
    mov word [buf_off], 0
.no_seg_bump:

    inc word [cur_lba_lo]
    dec word [remaining]
    jmp .chs_loop

.read_done:
    xor ax, ax
    mov es, ax                       ; Восстановить ES=0 (используется дальше)
    mov si, msg_ok
    call puts16
    ret

.disk_error:
    mov si, msg_derr
    call puts16
    cli
    hlt

; --- puts16: вывести строку (Real Mode) ----------------------------------
puts16:
    push ax
    push bx
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bx, bx
    int 0x10
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; ============================================================
; ДАННЫЕ (Real Mode секция)
; ============================================================
msg_hello     db "Stage2: TerminusOS Bootloader", 0x0D, 0x0A, 0
msg_a20       db "  A20 OK", 0x0D, 0x0A, 0
msg_mem       db "  Memory OK", 0x0D, 0x0A, 0
msg_geom_ok   db "  CHS geometry OK", 0x0D, 0x0A, 0
msg_loading   db "  Loading kernel.elf...", 0
msg_preloaded db "  Kernel preloaded (ISO)", 0x0D, 0x0A, 0
msg_ok        db " OK", 0x0D, 0x0A, 0
msg_derr      db " DISK ERROR!", 0x0D, 0x0A, 0

boot_drive  db 0x80

; --- VBE данные/результат ----------------------------------------------
vbe_ok            db 0
vbe_modelist_off  dw 0
vbe_modelist_seg  dw 0
vbe_width         dd 0
vbe_height        dd 0
vbe_pitch         dd 0
vbe_bpp           dd 0
vbe_addr          dd 0
msg_vbe_ok        db "  VBE LFB 640x480 OK", 0x0D, 0x0A, 0
msg_vbe_no        db "  VBE LFB unavailable (fallback to legacy)", 0x0D, 0x0A, 0
mem_lower   dd 640
mem_upper   dd 0
remaining   dw 0

spt         dw 63
heads       dw 16
cur_lba_lo  dw 0
buf_seg     dw 0
buf_off     dw 0

; --- GDT --------------------------------------------------------------------
align 8
gdt_start:
    dq 0

    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00

    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ============================================================
; PROTECTED MODE (32-bit)
; ============================================================
[bits 32]

pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, STACK_TOP
    cld                 ; ОБЯЗАТЕЛЬНО: DF=0 для rep movs/stos ниже и для
                         ; всего кода ядра — компилятор (GCC -O2) сам
                         ; генерирует rep stosb/movsb для обычных C-циклов
                         ; заполнения массивов, и ожидает DF=0 по умолчанию
                         ; (это требование System V ABI). GRUB всегда это
                         ; гарантировал, наш загрузчик — нет, отсюда битая
                         ; графика, если DF унаследовался из real mode = 1.

    call load_elf32

    mov edi, MB_INFO
    xor eax, eax
    mov ecx, 30                ; Зануляем весь MultibootInfo (включая framebuffer_*)
    rep stosd

    mov eax, [mem_lower]
    mov [MB_INFO + 0x04], eax

    mov eax, [mem_upper]
    mov [MB_INFO + 0x08], eax

    movzx eax, byte [boot_drive]
    shl eax, 24
    mov [MB_INFO + 0x0C], eax

    ; flags: bit0=mem, bit1=boot_device всегда; bit12=framebuffer, если VBE удался
    cmp byte [vbe_ok], 0
    je .no_vbe

    mov dword [MB_INFO + 0x00], 0x1003   ; 0x1000 | 0x03

    mov eax, [vbe_addr]
    mov [MB_INFO + 0x58], eax            ; framebuffer_addr (нижние 32 бита)
    mov dword [MB_INFO + 0x5C], 0        ; framebuffer_addr (верхние 32 бита)

    mov eax, [vbe_pitch]
    mov [MB_INFO + 0x60], eax            ; framebuffer_pitch

    mov eax, [vbe_width]
    mov [MB_INFO + 0x64], eax            ; framebuffer_width

    mov eax, [vbe_height]
    mov [MB_INFO + 0x68], eax            ; framebuffer_height

    mov eax, [vbe_bpp]
    mov [MB_INFO + 0x6C], al             ; framebuffer_bpp

    mov byte [MB_INFO + 0x6D], 1         ; framebuffer_type = 1 (RGB)
    jmp .mbinfo_done

.no_vbe:
    mov dword [MB_INFO + 0x00], 0x03

.mbinfo_done:
    mov eax, 0x2BADB002
    mov ebx, MB_INFO
    call [kernel_entry]

.hang:
    cli
    hlt
    jmp .hang

; ============================================================
; ELF32 ЗАГРУЗЧИК (Protected Mode)
; ============================================================
load_elf32:
    cmp dword [ELF_BUFFER + 0x00], 0x464C457F
    jne .not_elf

    mov eax, [ELF_BUFFER + 0x18]
    mov [kernel_entry], eax

    mov ebx, [ELF_BUFFER + 0x1C]
    add ebx, ELF_BUFFER

    movzx ecx, word [ELF_BUFFER + 0x2C]

.phdr_loop:
    test ecx, ecx
    jz .done

    cmp dword [ebx + 0x00], 1
    jne .next_phdr

    mov esi, [ebx + 0x04]
    add esi, ELF_BUFFER
    mov edi, [ebx + 0x0C]
    mov edx, [ebx + 0x14]

    push ecx
    push ebx

    mov ecx, [ebx + 0x10]
    push ecx
    shr ecx, 2
    rep movsd
    pop ecx
    and ecx, 3
    rep movsb

    pop ebx
    mov ecx, edx
    sub ecx, [ebx + 0x10]
    jz .skip_bss
    xor eax, eax
    ; ВАЖНО: memsz-filesz не всегда кратно 4 (например, если после этого
    ; сегмента в файле "довешен" ещё код/данные не кратно dword). Раньше
    ; здесь было только "shr ecx,2; rep stosd" — остаток 1-3 байта в конце
    ; BSS НИКОГДА не зануляется и хранит мусор, оставшийся в RAM с
    ; предыдущей стадии загрузки (стек real-mode, буфер ELF и т.п.).
    ; Это давало непредсказуемые начальные значения глобальных переменных,
    ; расположенных в хвосте .bss — отсюда "случайные" баги вроде зависаний
    ; и пропавшего вывода у отдельных команд после переезда с GRUB на
    ; собственный загрузчик (GRUB зануляет BSS полностью и корректно).
    push ecx
    shr ecx, 2
    rep stosd
    pop ecx
    and ecx, 3
    rep stosb

.skip_bss:
    pop ecx

.next_phdr:
    add ebx, 0x20
    dec ecx
    jmp .phdr_loop

.done:
    ret

.not_elf:
    mov dword [kernel_entry], 0x100000
    ret

kernel_entry    dd 0