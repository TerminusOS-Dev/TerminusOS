#!/bin/bash

set -e

# ==============================================
# Цвета для красивого вывода
# ==============================================

RESET='\033[0m'
BOLD='\033[1m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
RED='\033[0;31m'

# ==============================================
# Аргументы командной строки
# ==============================================
# ./build.sh         → ISO + disk.img (по умолчанию)
# ./build.sh iso      → только ISO/CD-ROM
# ./build.sh disk      → только disk.img (для HDD/USB через dd)
# ./build.sh all      → ISO + disk.img
#
# ВАЖНО: GRUB больше не используется. ISO собирается на основе
# того же disk.img (Stage1 MBR + Stage2 ELF loader), встроенного
# в ISO через El Torito hard-disk emulation. Поэтому Stage 2
# (сборка disk.img) теперь обязательна для обеих целей.

TARGET="${1:-all}"

case "$TARGET" in
    iso|disk|all) ;;
    *)
        echo -e "\033[0;31mНеизвестная цель: $TARGET${RESET}"
        echo "Использование: ./build.sh [iso|disk|all]"
        exit 1
        ;;
esac

BUILD_ISO=false
BUILD_DISK=false
[ "$TARGET" = "iso" ]  && BUILD_ISO=true
[ "$TARGET" = "disk" ] && BUILD_DISK=true
[ "$TARGET" = "all" ]  && { BUILD_ISO=true; BUILD_DISK=true; }

# ISO теперь строится ИЗ disk.img, поэтому Stage 3 нужен в любом случае
NEED_DISK_IMG=false
$BUILD_ISO  && NEED_DISK_IMG=true
$BUILD_DISK && NEED_DISK_IMG=true

# ==============================================
# Конфигурация
# ==============================================

CROSS=""
CC="${CROSS}g++"
AS="${CROSS}gcc -m32 -c -x assembler"
LD="${CROSS}ld"

BUILD_DIR="build"
ISO_DIR="isodir"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
ISO_FILE="Terminus_OS.iso"

BOOT_DIR="boot"
DISK_IMG="disk.img"
STAGE1_BIN="$BUILD_DIR/stage1.bin"
STAGE2_BIN="$BUILD_DIR/stage2.bin"
KERN_LBA=66         # 0x7C00 + 66*512 = 0x10000 — выровнено с ELF_BUFFER
                     # для бесшовной преднагрузки через El-Torito no-emul
STAGE2_SECTS=65      # Зарезервировано под Stage 2 (до сектора, где начинается ядро)

CFLAGS="-m32 -ffreestanding -O2 -std=gnu++17 -fno-exceptions -fno-rtti"
LDFLAGS="-m elf_i386 -T src/linker.ld -nostdlib --no-warn-execstack"

# Количество параллельных задач = число ядер процессора
NUM_JOBS=$(nproc 2>/dev/null || echo 4)
CURRENT_JOBS=0

# Таймер
BUILD_START=$(date +%s)

# Функция для форматирования времени
format_time() {
    local seconds=$1
    local hours=$((seconds / 3600))
    local minutes=$(((seconds % 3600) / 60))
    local secs=$((seconds % 60))

    if [ $hours -gt 0 ]; then
        printf "%02d:%02d:%02d" $hours $minutes $secs
    else
        printf "%02d:%02d" $minutes $secs
    fi
}

# Функция для красивого заголовка
print_header() {
    echo -e "${BOLD}${CYAN}════════════════════════════════════════════${RESET}"
    echo -e "${BOLD}${CYAN}║ $1${RESET}"
    echo -e "${BOLD}${CYAN}════════════════════════════════════════════${RESET}"
}

echo -e "${BOLD}${CYAN}Build goal: ${YELLOW}$TARGET${RESET}"
if [ -f "./Terminus_OS.iso" ]; then
    echo -e "${YELLOW}⚠️  detected old ISO file, removing...${RESET}"
    rm -rf Terminus_OS.iso
    echo -e "${GREEN}✓ Old ISO removed.${RESET}"
fi

# ==============================================
# Stage 1: Сборка ядра (параллельная компиляция)
# ==============================================

print_header "🔨 Stage 1: Building Kernel (${NUM_JOBS} cores)"

STAGE1_START=$(date +%s)

mkdir -p $BUILD_DIR/{kernel,lib,fs,shell,drivers/commands}

CPP_FILES=$(find src -name "*.cpp" -type f)
ASM_FILES=$(find src -name "*.s" -type f)

OBJ_LIST=""

# Функция для ожидания свободного слота
wait_for_slot() {
    while [ $CURRENT_JOBS -ge $NUM_JOBS ]; do
        wait -n 2>/dev/null || true
        CURRENT_JOBS=$((CURRENT_JOBS - 1))
    done
}

# Компилируем .s файлы (ассемблер) параллельно
ASM_COUNT=$(echo $ASM_FILES | wc -w)
CPP_COUNT=$(echo $CPP_FILES | wc -w)
TOTAL_FILES=$((ASM_COUNT + CPP_COUNT))

echo -e "${YELLOW}📦 Files to compile: ${BOLD}${TOTAL_FILES}${RESET} (${ASM_COUNT} asm + ${CPP_COUNT} cpp)"
echo -e "${BLUE}⚙️  Starting compilation...${RESET}"
echo ""

ASM_DONE=0
for SRC_FILE in $ASM_FILES; do
    OBJ_FILE=$(echo $SRC_FILE | sed "s|^src/|$BUILD_DIR/|;s|\.s$|\.o|")
    mkdir -p "$(dirname "$OBJ_FILE")"

    wait_for_slot
    $AS $SRC_FILE -o $OBJ_FILE &
    CURRENT_JOBS=$((CURRENT_JOBS + 1))
    OBJ_LIST="$OBJ_LIST $OBJ_FILE"
    ASM_DONE=$((ASM_DONE + 1))
done

CPP_DONE=0
for SRC_FILE in $CPP_FILES; do
    OBJ_FILE=$(echo $SRC_FILE | sed "s|^src/|$BUILD_DIR/|;s|\.cpp$|\.o|")
    mkdir -p "$(dirname "$OBJ_FILE")"

    wait_for_slot
    $CC $CFLAGS -c $SRC_FILE -o $OBJ_FILE &
    CURRENT_JOBS=$((CURRENT_JOBS + 1))
    OBJ_LIST="$OBJ_LIST $OBJ_FILE"
    CPP_DONE=$((CPP_DONE + 1))
done

# Ждём завершения всех компиляций
echo -e "${YELLOW}⏳ Waiting for compilation to finish...${RESET}"
wait

# Линковка ядра
echo -e "${YELLOW}🔗 Linking kernel...${RESET}"
$LD $LDFLAGS $OBJ_LIST -o $KERNEL_ELF

STAGE1_END=$(date +%s)
STAGE1_TIME=$((STAGE1_END - STAGE1_START))

echo -e "${GREEN}✓ Kernel built: $KERNEL_ELF${RESET}"
echo -e "${GREEN}✓ Stage 1 time: $(format_time $STAGE1_TIME)${RESET}"

# ==============================================
# Stage 2: Создание disk.img (свой MBR-загрузчик)
# ==============================================
# Это основа всего — и для прямой HDD/USB загрузки,
# и для ISO/CD-ROM (встраивается через El Torito ниже).

STAGE2_TIME=0
if $NEED_DISK_IMG; then
    echo ""
    print_header "🖴 Stage 2: Creating Disk Image (MBR bootloader)"

    STAGE2_START=$(date +%s)

    if ! command -v nasm &>/dev/null; then
        echo -e "${RED}✗ ERROR: nasm не установлен (sudo apt install nasm)${RESET}"
        exit 1
    fi

    if [ ! -f "$BOOT_DIR/stage1.asm" ] || [ ! -f "$BOOT_DIR/stage2.asm" ]; then
        echo -e "${RED}✗ ERROR: Не найдены $BOOT_DIR/stage1.asm / stage2.asm${RESET}"
        exit 1
    fi

    # Размер ядра → сколько секторов читать Stage 2
    KERN_SIZE=$(stat -c%s "$KERNEL_ELF")
    KERN_SECTS=$(( (KERN_SIZE + 511) / 512 ))

    echo -e "${BLUE}⚙️  Assembling Stage 1...${RESET}"
    nasm -f bin "$BOOT_DIR/stage1.asm" -o "$STAGE1_BIN"

    S1_SIZE=$(stat -c%s "$STAGE1_BIN")
    if [ "$S1_SIZE" -ne 512 ]; then
        echo -e "${RED}✗ ERROR: stage1.bin должен быть 512 байт (сейчас $S1_SIZE)${RESET}"
        exit 1
    fi

    echo -e "${BLUE}⚙️  Assembling Stage 2 (kernel = ${KERN_SECTS} sectors)...${RESET}"
    nasm -f bin -D KERNEL_SECTORS=$KERN_SECTS "$BOOT_DIR/stage2.asm" -o "$STAGE2_BIN"

    S2_SIZE=$(stat -c%s "$STAGE2_BIN")
    S2_SECTS=$(( (S2_SIZE + 511) / 512 ))
    if [ "$S2_SECTS" -gt "$STAGE2_SECTS" ]; then
        echo -e "${RED}✗ ERROR: stage2.bin слишком большой (${S2_SIZE} байт, лимит $((STAGE2_SECTS*512)))${RESET}"
        echo -e "${RED}  Увеличь STAGE2_SECTS в build.sh (и соответствующий KERN_LBA в stage2.asm)${RESET}"
        exit 1
    fi

    echo -e "${BLUE}🖴 Writing $DISK_IMG...${RESET}"
    TOTAL_SECTS=$(( 1 + STAGE2_SECTS + KERN_SECTS + 32 ))

    dd if=/dev/zero of="$DISK_IMG" bs=512 count=$TOTAL_SECTS status=none

    dd if="$STAGE1_BIN" of="$DISK_IMG" bs=512 count=1 \
       conv=notrunc status=none

    dd if="$STAGE2_BIN" of="$DISK_IMG" bs=512 seek=1 \
       conv=notrunc status=none

    dd if="$KERNEL_ELF" of="$DISK_IMG" bs=512 seek=$KERN_LBA \
       conv=notrunc status=none

    STAGE2_END=$(date +%s)
    STAGE2_TIME=$((STAGE2_END - STAGE2_START))

    echo -e "${GREEN}✓ Disk image created: $DISK_IMG${RESET}"
    echo -e "${GREEN}✓ Stage 2 time: $(format_time $STAGE2_TIME)${RESET}"
fi

# ==============================================
# Stage 3: Создание ISO/CD-ROM (без GRUB)
# ==============================================
# disk.img встраивается в ISO как El Torito boot image в режиме
# "no emulation": BIOS грузит ВЕСЬ disk.img одним куском в RAM
# на 0x7C00 (через -boot-load-size), без единого int13h-вызова
# во время загрузки. Stage 1/Stage 2 сами определяют по магическим
# меткам в памяти, что данные уже на месте, и просто продолжают
# выполнение — это обходит ограничение SeaBIOS/QEMU, где режим
# "hard-disk emulation" не поддерживает повторные диско-вызовы
# после самого первого сектора.
#
# ВАЖНО: флаг -boot-info-table НЕ используется — он патчит байты
# 8-63 загрузочного образа служебными данными (конвенция isolinux),
# а наш raw-код этого не ожидает и был бы испорчен таким патчем.

STAGE3_TIME=0
if $BUILD_ISO; then
    echo ""
    print_header "💿 Stage 3: Creating ISO/CD-ROM Image"

    STAGE3_START=$(date +%s)

    if ! command -v xorriso &>/dev/null; then
        echo -e "${RED}✗ ERROR: xorriso не установлен (sudo apt install xorriso)${RESET}"
        exit 1
    fi

    DISK_SECTS=$(( $(stat -c%s "$DISK_IMG") / 512 ))
    if [ "$DISK_SECTS" -gt 65535 ]; then
        echo -e "${RED}✗ ERROR: disk.img слишком большой для -boot-load-size (макс. 65535 секторов = 32MB)${RESET}"
        exit 1
    fi

    rm -rf $ISO_DIR
    mkdir -p $ISO_DIR/boot

    echo -e "${BLUE}📁 Copying files...${RESET}"
    cp $DISK_IMG   $ISO_DIR/boot/disk.img
    # kernel.elf отдельно НЕ кладём — он уже встроен внутри disk.img,
    # дублирование только раздувало размер ISO без всякой пользы.

    echo -e "${BLUE}📀 Building ISO (no-emulation boot)${RESET}"
    xorriso -as mkisofs \
    -R -J \
    -isohybrid-mbr "$STAGE1_BIN" \
    -b boot/disk.img \
    -no-emul-boot \
    -boot-load-size $DISK_SECTS \
    -partition_offset 16 \
    -o $ISO_FILE \
    $ISO_DIR 2>/dev/null

    STAGE3_END=$(date +%s)
    STAGE3_TIME=$((STAGE3_END - STAGE3_START))

    echo -e "${GREEN}✓ ISO created: $ISO_FILE${RESET}"
    echo -e "${GREEN}✓ Stage 3 time: $(format_time $STAGE3_TIME)${RESET}"
fi

# ==============================================
# Финальная статистика
# ==============================================

BUILD_END=$(date +%s)
BUILD_TOTAL=$((BUILD_END - BUILD_START))

echo ""
print_header "📊 Build Statistics"

KERNEL_SIZE=$(du -h $KERNEL_ELF | cut -f1)

rm -rf $BUILD_DIR
rm -rf $ISO_DIR

rm -f $OBJ_LIST
rm -f $KERNEL_ELF
echo -e "${RED}🗑️  Build artifacts cleaned up.${RESET}"

echo -e "${CYAN}📈 Files compiled:${RESET}        ${BOLD}$TOTAL_FILES${RESET}"
echo -e "${CYAN}📦 Kernel size:${RESET}           ${BOLD}$KERNEL_SIZE${RESET}"

if $BUILD_DISK; then
    DISK_SIZE=$(du -h $DISK_IMG | cut -f1)
    echo -e "${CYAN}🖴 Disk image size:${RESET}       ${BOLD}$DISK_SIZE${RESET}"
    echo -e "${GREEN}✓ $DISK_IMG сохранён — можно писать на USB через Etcher/dd${RESET}"
else
    # disk.img нужен только как промежуточный артефакт для ISO — удаляем
    rm -f $DISK_IMG
fi

if $BUILD_ISO; then
    ISO_SIZE=$(du -h $ISO_FILE | cut -f1)
    echo -e "${CYAN}💿 ISO size:${RESET}              ${BOLD}$ISO_SIZE${RESET}"
fi

echo ""
echo -e "${CYAN}⏱️  Stage 1 (compilation):${RESET}  ${BOLD}$(format_time $STAGE1_TIME)${RESET}"
$NEED_DISK_IMG && echo -e "${CYAN}⏱️  Stage 2 (disk.img):${RESET}    ${BOLD}$(format_time $STAGE2_TIME)${RESET}"
$BUILD_ISO     && echo -e "${CYAN}⏱️  Stage 3 (ISO):${RESET}         ${BOLD}$(format_time $STAGE3_TIME)${RESET}"
echo -e "${CYAN}⏱️  Total build time:${RESET}      ${BOLD}$(format_time $BUILD_TOTAL)${RESET}"
echo ""
