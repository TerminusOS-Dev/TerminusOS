#include "atapi.h"
#include "disk.h"         // inb / outb / inw / outw
#include "../lib/screen.h"
#include "../lib/string.h"
#include <stdint.h>

// ===== Порты вторичного канала ATA =====
#define ATAPI_DATA          0x170
#define ATAPI_ERROR         0x171
#define ATAPI_FEATURES      0x171
#define ATAPI_INT_REASON    0x172   // Interrupt Reason (sector count reg)
#define ATAPI_LBA_LO        0x173
#define ATAPI_BYTE_LO       0x174   // Cylinder Low  = byte count low
#define ATAPI_BYTE_HI       0x175   // Cylinder High = byte count high
#define ATAPI_DRIVE_SEL     0x176
#define ATAPI_STATUS        0x177
#define ATAPI_COMMAND       0x177
#define ATAPI_CONTROL       0x376

// ATA/ATAPI статусные биты
#define ATA_SR_BSY          0x80    // Busy
#define ATA_SR_DRQ          0x08    // Data Request
#define ATA_SR_ERR          0x01    // Error
#define ATA_SR_CHK          0x01    // Check (same bit for ATAPI)

// ATA команды
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET          0xA0
#define ATA_CMD_SOFT_RESET      0x08

// ATAPI SCSI-команды (12-байтный пакет)
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_READ_CAPACITY      0x25
#define SCSI_READ_12            0xA8

bool atapi_disk_present = false;

// ===== Вспомогательные функции =====

static void atapi_delay() {
    // 400нс задержка — читаем статус 4 раза (каждый inb ~100нс)
    inb(ATAPI_STATUS);
    inb(ATAPI_STATUS);
    inb(ATAPI_STATUS);
    inb(ATAPI_STATUS);
}

static bool atapi_wait_bsy(int timeout_loops) {
    for (int i = 0; i < timeout_loops; i++) {
        uint8_t st = inb(ATAPI_STATUS);
        if (!(st & ATA_SR_BSY)) return true;
        // небольшая задержка
        for (volatile int d = 0; d < 100; d++);
    }
    return false; // timeout
}

static bool atapi_wait_drq(int timeout_loops) {
    for (int i = 0; i < timeout_loops; i++) {
        uint8_t st = inb(ATAPI_STATUS);
        if (st & ATA_SR_ERR) return false;   // ошибка
        if (st & ATA_SR_DRQ) return true;    // готов к данным
        for (volatile int d = 0; d < 100; d++);
    }
    return false;
}

// ===== Отправка ATAPI-пакета и получение данных =====
// packet    — 12 байт SCSI-команды
// out_buf   — буфер для данных ответа (или NULL если ответ не нужен)
// out_size  — сколько байт ожидаем прочитать
static bool atapi_send_packet(const uint8_t* packet, uint8_t* out_buf, uint16_t out_size) {
    // Выбираем привод 0 (master) на вторичном канале
    outb(ATAPI_DRIVE_SEL, 0xA0);
    atapi_delay();

    if (!atapi_wait_bsy(100000)) return false;

    // Устанавливаем byte count (ожидаемый размер ответа)
    outb(ATAPI_FEATURES, 0x00);         // PIO mode, non-DMA
    outb(ATAPI_BYTE_LO,  (uint8_t)(out_size & 0xFF));
    outb(ATAPI_BYTE_HI,  (uint8_t)((out_size >> 8) & 0xFF));

    // Отправляем команду PACKET
    outb(ATAPI_COMMAND, ATA_CMD_PACKET);
    atapi_delay();

    // Ждём DRQ — привод готов принять пакет
    if (!atapi_wait_drq(100000)) return false;

    // Передаём 12-байтный пакет словами (6 слов по 2 байта)
    const uint16_t* pw = (const uint16_t*)packet;
    for (int i = 0; i < 6; i++) outw(ATAPI_DATA, pw[i]);

    // Если нет выходного буфера — всё
    if (!out_buf || out_size == 0) {
        atapi_wait_bsy(500000);
        return true;
    }

    // Ждём DRQ — привод готов отдать данные
    if (!atapi_wait_drq(1000000)) return false;

    // Проверяем реальный byte count от привода
    uint16_t actual = ((uint16_t)inb(ATAPI_BYTE_HI) << 8) | inb(ATAPI_BYTE_LO);
    if (actual == 0) actual = out_size;
    if (actual > out_size) actual = out_size;

    // Читаем данные словами
    uint16_t* dp = (uint16_t*)out_buf;
    uint16_t words = actual / 2;
    for (uint16_t i = 0; i < words; i++) dp[i] = inw(ATAPI_DATA);
    if (actual & 1) {
        // нечётный байт
        uint16_t last = inw(ATAPI_DATA);
        out_buf[actual - 1] = (uint8_t)(last & 0xFF);
    }

    atapi_wait_bsy(500000);
    return true;
}

// ===== Публичный API =====

bool atapi_detect() {
    // Выбираем master на вторичном канале
    outb(ATAPI_DRIVE_SEL, 0xA0);
    atapi_delay();

    // Soft reset вторичного канала
    outb(ATAPI_CONTROL, 0x04); // SRST=1
    for (volatile int i = 0; i < 1000; i++);
    outb(ATAPI_CONTROL, 0x00); // SRST=0
    atapi_delay();

    if (!atapi_wait_bsy(200000)) return false;

    // Признак ATAPI после сброса: LBA_LO=0x14, LBA_HI=0xEB
    uint8_t lo = inb(ATAPI_LBA_LO + 1); // 0x174
    uint8_t hi = inb(ATAPI_BYTE_HI);    // 0x175
    if (lo != 0x14 || hi != 0xEB) {
        // Нет ATAPI-устройства
        return false;
    }

    // Отправляем IDENTIFY PACKET DEVICE для подтверждения
    outb(ATAPI_COMMAND, ATA_CMD_IDENTIFY_PACKET);
    atapi_delay();

    if (!atapi_wait_bsy(100000)) return false;
    uint8_t st = inb(ATAPI_STATUS);
    if (!(st & ATA_SR_DRQ)) return false;

    // Читаем 256 слов (512 байт) идентификации
    uint16_t ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(ATAPI_DATA);

    // Слово 0 бит 15:14 = 10 означает ATAPI; бит 8:7 — тип устройства (CD-ROM = 101)
    uint16_t w0 = ident[0];
    if ((w0 >> 14) != 2) return false; // не ATAPI

    return true;
}

bool atapi_check_disk() {
    // TEST UNIT READY — стандартная SCSI-команда проверки наличия диска
    uint8_t pkt[12] = { SCSI_TEST_UNIT_READY, 0,0,0,0,0,0,0,0,0,0,0 };
    bool ok = atapi_send_packet(pkt, nullptr, 0);

    uint8_t st = inb(ATAPI_STATUS);
    if (!ok || (st & ATA_SR_ERR)) {
        atapi_disk_present = false;
        return false;
    }
    atapi_disk_present = true;
    return true;
}

bool atapi_read_sector(uint32_t lba, uint8_t* buf) {
    // READ (12) — читаем 1 сектор (2048 байт)
    uint8_t pkt[12];
    pkt[0]  = SCSI_READ_12;
    pkt[1]  = 0;                        // RelAdr=0, LUN=0
    pkt[2]  = (uint8_t)(lba >> 24);
    pkt[3]  = (uint8_t)(lba >> 16);
    pkt[4]  = (uint8_t)(lba >> 8);
    pkt[5]  = (uint8_t)(lba);
    pkt[6]  = 0;                        // Transfer Length (MSB) = 0
    pkt[7]  = 0;
    pkt[8]  = 0;
    pkt[9]  = 1;                        // Transfer Length = 1 сектор
    pkt[10] = 0;
    pkt[11] = 0;

    return atapi_send_packet(pkt, buf, ATAPI_SECTOR_SIZE);
}