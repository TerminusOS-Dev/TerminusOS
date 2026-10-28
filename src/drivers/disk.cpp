#include "disk.h"
#include "../lib/screen.h"

// ── ATA Primary порты ────────────────────────────────────────────────────────
#define ATA_DATA         0x1F0
#define ATA_ERROR        0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW      0x1F3
#define ATA_LBA_MID      0x1F4
#define ATA_LBA_HIGH     0x1F5
#define ATA_DRIVE_HEAD   0x1F6
#define ATA_STATUS       0x1F7   // read = status, write = command
#define ATA_COMMAND      0x1F7
#define ATA_ALT_STATUS   0x3F6   // alternate status (read) / device control (write)
#define ATA_DEV_CONTROL  0x3F6

// ── ATA статусные биты ───────────────────────────────────────────────────────
#define ATA_SR_BSY  0x80   // Busy
#define ATA_SR_DRDY 0x40   // Drive Ready
#define ATA_SR_DRQ  0x08   // Data Request
#define ATA_SR_ERR  0x01   // Error
#define ATA_SR_DF   0x20   // Drive Fault

// ── ATA команды ──────────────────────────────────────────────────────────────
#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_SRST       0x04   // Software Reset (в DEV_CONTROL)

// ── Таймаут ──────────────────────────────────────────────────────────────────
// Раньше было 100000 (~100-300мс при ~1мкс/inb на реальном железе — см. оценку
// автора ниже). В QEMU виртуальный диск отвечает почти мгновенно, так что там
// этого хватало всегда — а вот на настоящем (тем более старом/потрёпанном)
// механическом винте раскрутка и поиск после сброса легко может занять секунды.
// Итерационный счётчик — по своей природе не "реальное время", а грубая оценка,
// зависящая от скорости порта/шины; чтобы не сдаваться раньше, чем диск
// реально успел ответить, бюджет увеличен на порядок (~5с на том же расчёте).
#define ATA_TIMEOUT 5000000


// **********************************************
// ===== I/O Functions (порты ввода/вывода) =====
// **********************************************

uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

void outb(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "dN"(port));
}

uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

void outw(uint16_t port, uint16_t data) {
    asm volatile("outw %0, %1" : : "a"(data), "dN"(port));
}


// **********************************************
// ===== ATA Helper functions ==================
// **********************************************

uint8_t sector_buffer[512];

// 400нс задержка — читаем alternate status 4 раза (каждый inb ~100нс на реальном железе)
static void ata_400ns_delay() {
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
}

// Ждём пока BSY не снимется. Возвращает false при таймауте.
static bool ata_wait_bsy() {
    bool warned = false;
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(ATA_STATUS) & ATA_SR_BSY))
            return true;
        if (!warned && i == ATA_TIMEOUT / 4) { println("[ata] wait_bsy: still polling (25%)..."); warned = true; }
    }
    println("ATA: timeout waiting for BSY=0");
    return false;
}

// Ждём BSY=0 И DRDY=1 (диск готов принять команду). Возвращает false при таймауте.
static bool ata_wait_ready() {
    bool warned = false;
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY))
            return true;
        if (!warned && i == ATA_TIMEOUT / 4) { println("[ata] wait_ready: still polling (25%)..."); warned = true; }
    }
    println("ATA: timeout waiting for DRDY=1");
    return false;
}

// Ждём DRQ=1 (данные готовы к передаче). Возвращает false при ошибке/таймауте.
static bool ata_wait_drq() {
    bool warned = false;
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR || status & ATA_SR_DF) {
            println("ATA: error/fault bit set while waiting for DRQ");
            return false;
        }
        if (status & ATA_SR_DRQ)
            return true;
        if (!warned && i == ATA_TIMEOUT / 4) { println("[ata] wait_drq: still polling (25%)..."); warned = true; }
    }
    println("ATA: timeout waiting for DRQ=1");
    return false;
}

// Software reset — приводит контроллер в чистое состояние
// Нужен особенно на реальном железе после BIOS/GRUB
static void ata_software_reset() {
    outb(ATA_DEV_CONTROL, ATA_CMD_SRST); // SRST bit
    ata_400ns_delay();
    outb(ATA_DEV_CONTROL, 0x00);         // снимаем reset
    ata_400ns_delay();
    ata_wait_bsy();                       // ждём окончания reset
}


// **********************************************
// ===== ATA Driver (Чтение/Запись диска) =======
// **********************************************

// Инициализация ATA — вызывать один раз при старте (например из fat32_init)
void ata_init() {
    println("ATA: Loading ...");
    ata_software_reset();

    // Выбираем master-диск, проверяем что он есть
    outb(ATA_DRIVE_HEAD, 0xA0); // master, CHS/LBA28
    ata_400ns_delay();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0xFF) {
        // 0xFF — нет диска / контроллера (floating bus)
        println("ATA: no disk detected (status=0xFF)");
    }
    else if (status == 0x00) {
        // 0x00 — нет диска, но контроллер есть (floating bus с pull-up)
        println("ATA: no disk detected (status=0x00)");
    }
    else if (status & ATA_SR_DRDY)
    {
        println("ATA: disk detected");
    }
    return;
}

// Спрашивает у контроллера реальный размер диска командой IDENTIFY DEVICE (0xEC).
// Нужен, чтобы FAT32 мог посчитать реальный размер FAT под конкретный диск,
// а не жить с зашитым на этапе компиляции потолком в 256MB.
bool ata_identify(uint32_t* out_total_sectors) {
    if (!ata_wait_bsy()) return false;

    outb(ATA_DRIVE_HEAD, 0xA0); // master; отдельный LBA-бит для IDENTIFY не нужен
    ata_400ns_delay();

    // Обнуляем регистры параметров — так рекомендует спецификация перед IDENTIFY
    outb(ATA_SECTOR_COUNT, 0);
    outb(ATA_LBA_LOW,  0);
    outb(ATA_LBA_MID,  0);
    outb(ATA_LBA_HIGH, 0);

    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) return false; // диска нет вовсе (floating bus)

    if (!ata_wait_bsy()) return false;

    // ATAPI (CD-ROM) и прочие не-ATA устройства обычно сразу выставляют ERR
    if (inb(ATA_STATUS) & ATA_SR_ERR) return false;

    if (!ata_wait_drq()) return false;

    uint16_t id_data[256];
    for (int i = 0; i < 256; i++) id_data[i] = inw(ATA_DATA);

    // Слова 60-61 IDENTIFY DEVICE: общее число адресуемых LBA28-секторов
    uint32_t total = ((uint32_t)id_data[61] << 16) | id_data[60];
    if (total == 0) return false;

    *out_total_sectors = total;
    return true;
}

bool ata_read_sector(uint32_t lba) {
    for (int attempt = 0; attempt < 4; attempt++) {
        // 1. Ждём пока диск не освободится
        if (!ata_wait_bsy()) continue;

        // 2. Выбираем диск + старшие 4 бита LBA
        outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));

        // 3. ОБЯЗАТЕЛЬНАЯ задержка 400нс после смены диска/режима
        ata_400ns_delay();

        // 4. Ждём BSY=0 и DRDY=1 (диск готов принять команду)
        if (!ata_wait_ready()) continue;

        // 5. Загружаем параметры
        outb(ATA_SECTOR_COUNT, 1);
        outb(ATA_LBA_LOW,  (uint8_t)(lba));
        outb(ATA_LBA_MID,  (uint8_t)(lba >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));

        // 6. Отправляем команду READ PIO
        outb(ATA_COMMAND, ATA_CMD_READ_PIO);

        // 7. 400нс задержка после команды (даём контроллеру время обновить статус)
        ata_400ns_delay();

        // 8. Ждём BSY=0 затем DRQ=1 — при транзиентной ошибке не сдаёмся сразу,
        //    а пробуем ещё раз (см. attempt) вместо мгновенного отказа
        if (!ata_wait_bsy()) continue;
        if (!ata_wait_drq()) continue;

        // 9. Читаем 256 слов (512 байт)
        for (int i = 0; i < 256; i++) {
            ((uint16_t*)sector_buffer)[i] = inw(ATA_DATA);
        }
        return true;
    }
    println("ATA: read failed after retries");
    return false;
}

void ata_write_sector(uint32_t lba, const uint8_t* buffer) {
    // 1. Ждём освобождения
    if (!ata_wait_bsy()) return;

    // 2. Выбираем диск
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    ata_400ns_delay();

    // 3. Ждём готовности
    if (!ata_wait_ready()) return;

    // 4. Параметры
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW,  (uint8_t)(lba));
    outb(ATA_LBA_MID,  (uint8_t)(lba >> 8));
    outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));

    // 5. Команда WRITE PIO
    outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);
    ata_400ns_delay();

    // 6. Ждём DRQ=1 — диск ждёт данные
    if (!ata_wait_bsy()) return;
    if (!ata_wait_drq()) return;

    // 7. Пишем 256 слов
    for (int i = 0; i < 256; i++) {
        outw(ATA_DATA, ((uint16_t*)buffer)[i]);
    }

    // 8. Flush cache — говорим диску записать буфер на пластину
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_400ns_delay();
    ata_wait_bsy();

    // 9. Проверяем ошибки
    uint8_t status = inb(ATA_STATUS);
    if (status & ATA_SR_ERR || status & ATA_SR_DF) {
        println("ATA: write error/fault");
    }
}