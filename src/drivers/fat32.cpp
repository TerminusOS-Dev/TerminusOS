#include "fat32.h"
#include "../drivers/disk.h"
#include "../lib/screen.h"
#include "../lib/string.h"
#include <stdint.h>

// ===== Глобальное состояние =====
uint32_t current_dir_cluster = FAT32_ROOT_CLUSTER;
FAT32_BPB fat32_bpb;

// **********************************************
// ===== Динамический размер FAT (снятие потолка 256MB) =====
// **********************************************
//
// Раньше FAT32_SECTORS_PER_FAT было зашито в компиляции (512 секторов =
// 65536 кластеров = 256MB), независимо от реального размера диска. Теперь
// размер FAT считается один раз при первом форматировании — исходя из
// РЕАЛЬНОГО размера диска (ata_identify) — и сохраняется в служебном
// заголовке на LBA 0, чтобы каждая следующая загрузка читала РОВНО ТЕ ЖЕ
// адреса (иначе пересчёт с нуля мог бы "уехать" и раскорраптить уже
// записанные данные, если бы значение вдруг оказалось другим).
#define TFS_MAGIC 0x31534654u // "TFS1" в LE

struct TerminusFsHeader {
    uint32_t magic;
    uint32_t sectors_per_fat;
    uint32_t total_disk_sectors; // информационно — что вернул IDENTIFY при форматировании
    uint32_t reserved[125];
} __attribute__((packed));
// 4*4 + 125*4 = 512 байт — ровно один сектор

static uint32_t g_sectors_per_fat = FAT32_SECTORS_PER_FAT; // дефолт = старое поведение,
static uint32_t g_data_start      = FAT32_DATA_START;      // пока fat32_init() не пересчитает
static uint32_t g_max_cluster     = FAT32_SECTORS_PER_FAT * FAT32_ENTRIES_PER_FAT_SECTOR;
static uint32_t g_identified_disk_sectors = 0; // что вернул ata_identify() при последнем форматировании

extern "C" {

uint32_t fat32_get_sectors_per_fat()         { return g_sectors_per_fat; }
uint32_t fat32_get_max_cluster()              { return g_max_cluster; }
uint32_t fat32_get_identified_disk_sectors()  { return g_identified_disk_sectors; }

// **********************************************
// ===== Core Helpers =====
// **********************************************

void fat32_format_name(const char* input, char* output_8_3) {
    for (int i = 0; i < 11; i++) output_8_3[i] = ' ';

    int src_i  = 0;
    int dest_i = 0;

    // Имя (до 8 символов)
    while (input[src_i] != 0 && input[src_i] != '.' && dest_i < 8) {
        char c = input[src_i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        output_8_3[dest_i++] = c;
    }

    // Расширение (до 3 символов)
    while (input[src_i] != 0 && input[src_i] != '.') src_i++;
    if (input[src_i] == '.') src_i++;
    dest_i = 8;
    while (input[src_i] != 0 && dest_i < 11) {
        char c = input[src_i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        output_8_3[dest_i++] = c;
    }
}

// Кластер → LBA
// FAT32: data area начинается после зарезервированных секторов + FAT-таблицы
// Кластер 2 — первый кластер данных
uint32_t fat32_cluster_to_lba(uint32_t cluster) {
    if (cluster < 2) return g_data_start; // защита
    return g_data_start + (cluster - 2) * FAT32_SECTORS_PER_CLUSTER;
}

// Читает следующий кластер из FAT32
// FAT32: 4 байта на запись, 128 записей на сектор
uint32_t fat32_get_next_cluster(uint32_t current_cluster) {
    if (current_cluster < 2) return FAT32_EOC;

    uint32_t fat_sector_lba   = FAT32_FAT_START + (current_cluster / FAT32_ENTRIES_PER_FAT_SECTOR);
    uint32_t fat_entry_index  = current_cluster % FAT32_ENTRIES_PER_FAT_SECTOR;

    ata_read_sector(fat_sector_lba);
    uint32_t* fat_table  = (uint32_t*)sector_buffer;
    uint32_t  next       = fat_table[fat_entry_index] & FAT32_MASK;

    return next;
}

// Читает файл, начиная с start_cluster, в buf — до min(file_size, buf_max) байт.
// Общий кусок, который раньше был отдельно продублирован в cmd_cp и других
// местах (везде один и тот же цикл: сектор за сектором, кластер за кластером).
// Возвращает реально прочитано байт.
uint32_t fat32_read_file_to_buffer(uint32_t start_cluster, uint32_t file_size,
                                    uint8_t* buf, uint32_t buf_max) {
    uint32_t cluster = start_cluster;
    uint32_t got = 0;
    uint32_t limit = (file_size < buf_max) ? file_size : buf_max;
    while ((cluster & FAT32_MASK) >= 2 &&
           (cluster & FAT32_MASK) < (FAT32_EOC & FAT32_MASK) &&
           got < limit) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        for (int s = 0; s < FAT32_SECTORS_PER_CLUSTER && got < limit; s++) {
            ata_read_sector(lba + s);
            for (int b = 0; b < FAT32_BYTES_PER_SECTOR && got < limit; b++)
                buf[got++] = sector_buffer[b];
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return got;
}

// Инициализация FAT32: если FAT пустая — заполняем служебные записи
// Считает разумный размер FAT (в секторах) под реальный размер диска.
// Не пытается быть идеально оптимальным (не вычитает место под сам FAT
// из доступных данных) — немного занижает ёмкость, но безопасно и просто.
static uint32_t fat32_compute_sectors_per_fat(uint32_t total_disk_sectors) {
    if (total_disk_sectors <= FAT32_RESERVED_SECTORS) return FAT32_SECTORS_PER_FAT; // защита
    uint32_t usable          = total_disk_sectors - FAT32_RESERVED_SECTORS;
    uint32_t approx_clusters = usable / FAT32_SECTORS_PER_CLUSTER;
    uint32_t sectors_per_fat = (approx_clusters + FAT32_ENTRIES_PER_FAT_SECTOR - 1)
                               / FAT32_ENTRIES_PER_FAT_SECTOR;
    // БАГ БЫЛ ЗДЕСЬ: раньше тут стояло
    //   if (sectors_per_fat < FAT32_SECTORS_PER_FAT) sectors_per_fat = FAT32_SECTORS_PER_FAT;
    // FAT32_SECTORS_PER_FAT (512) — это старая фиксированная раскладка ровно
    // под 256MB (512 * 128 entries/sector * 4096 байт/кластер = 268 435 456).
    // Эта строка подпирала СНИЗУ любой реальный диск до 256MB "на всякий
    // случай" — а значит для любого диска МЕНЬШЕ 256MB таблица FAT адресовала
    // кластеры, которых физически не существует. При записи в такие кластеры
    // на растущих (sparse) образах — как раз то, что и раздувало disk.img до
    // 256MB, хотя создавался он меньше. Разметка должна честно следовать
    // реальному размеру диска, без искусственного минимума.
    if (sectors_per_fat < 1) sectors_per_fat = 1; // защита от вырожденного нуля на совсем крошечных дисках
    if (sectors_per_fat > 65536) sectors_per_fat = 65536; // потолок ~32GB — с запасом достаточно для хобби-ОС
    return sectors_per_fat;
}

void fat32_init() {
    println("FAT32: Loading ...");

    // ── Шаг 1: ищем НАШ служебный заголовок на LBA 0 ────────────────────────
    ata_read_sector(0);
    TerminusFsHeader hdr;
    for (int i = 0; i < 512; i++) ((uint8_t*)&hdr)[i] = sector_buffer[i];

    if (hdr.magic == TFS_MAGIC && hdr.sectors_per_fat >= FAT32_SECTORS_PER_FAT) {
        // Уже размечено по новой схеме — просто читаем сохранённые параметры.
        // ВАЖНО: пересчитывать НЕЛЬЗЯ, даже если IDENTIFY сейчас вернёт другое
        // число — раскладка должна оставаться той же, что была при форматировании.
        g_sectors_per_fat = hdr.sectors_per_fat;
        g_data_start      = FAT32_RESERVED_SECTORS + FAT32_NUMBER_OF_FATS * g_sectors_per_fat;
        g_max_cluster     = g_sectors_per_fat * FAT32_ENTRIES_PER_FAT_SECTOR;
        g_identified_disk_sectors = hdr.total_disk_sectors;
        current_dir_cluster = FAT32_ROOT_CLUSTER;
        println("FAT32: Existing filesystem recognized.");
        return;
    }

    // ── Шаг 2: заголовка нет — проверяем, не диск ли это, отформатированный
    //    СТАРЫМ кодом (фиксированные 512 секторов/256MB, без заголовка на LBA 0) ──
    g_sectors_per_fat = FAT32_SECTORS_PER_FAT;
    g_data_start      = FAT32_RESERVED_SECTORS + FAT32_NUMBER_OF_FATS * g_sectors_per_fat;
    g_max_cluster     = g_sectors_per_fat * FAT32_ENTRIES_PER_FAT_SECTOR;

    ata_read_sector(FAT32_FAT_START);
    uint32_t* fat_table = (uint32_t*)sector_buffer;
    bool old_scheme_has_data = ((fat_table[0] & FAT32_MASK) != 0x00000000);

    uint32_t total_sectors = 0;
    bool have_real_size = ata_identify(&total_sectors);
    g_identified_disk_sectors = have_real_size ? total_sectors : 0;

    if (old_scheme_has_data) {
        // Диск уже используется по старой (256MB) схеме — НЕ трогаем его
        // раскладку и не форматируем повторно, чтобы не потерять данные.
        // Просто дописываем заголовок, чтобы в следующий раз узнать его сразу.
        println("FAT32: Existing pre-header filesystem detected, preserving layout.");
    } else {
        // Диск реально пустой — теперь можно посчитать РЕАЛЬНЫЙ размер и
        // отформатировать с учётом полного объёма диска, а не потолка в 256MB.
        if (have_real_size) {
            g_sectors_per_fat = fat32_compute_sectors_per_fat(total_sectors);
            g_data_start      = FAT32_RESERVED_SECTORS + FAT32_NUMBER_OF_FATS * g_sectors_per_fat;
            g_max_cluster     = g_sectors_per_fat * FAT32_ENTRIES_PER_FAT_SECTOR;
        }
        // Если IDENTIFY не сработал (например, необычный образ диска) — остаёмся
        // на дефолтных значениях выше (старое поведение, 256MB), это не хуже, чем раньше.

        ata_read_sector(FAT32_FAT_START);
        uint32_t* ft = (uint32_t*)sector_buffer;
        ft[0] = 0x0FFFFFF8;  // Media descriptor
        ft[1] = FAT32_EOC;   // EOC для FAT ID
        ft[2] = FAT32_EOC;   // Корневая директория (кластер 2) — EOF
        ata_write_sector(FAT32_FAT_START, sector_buffer);
        if (FAT32_NUMBER_OF_FATS > 1) {
            ata_write_sector(FAT32_FAT_START + g_sectors_per_fat, sector_buffer);
        }

        uint8_t empty[FAT32_BYTES_PER_SECTOR] = {0};
        uint32_t root_lba = fat32_cluster_to_lba(FAT32_ROOT_CLUSTER);
        for (uint8_t s = 0; s < FAT32_SECTORS_PER_CLUSTER; s++) {
            ata_write_sector(root_lba + s, empty);
        }
        println("FAT32: Disk formatted and initialized.");
    }

    // ── Шаг 3: в обоих случаях (старая схема ИЛИ свежий формат) записываем
    //    заголовок на LBA 0, чтобы следующая загрузка узнала раскладку сразу ──
    TerminusFsHeader new_hdr;
    for (int i = 0; i < (int)sizeof(new_hdr); i++) ((uint8_t*)&new_hdr)[i] = 0;
    new_hdr.magic              = TFS_MAGIC;
    new_hdr.sectors_per_fat    = g_sectors_per_fat;
    new_hdr.total_disk_sectors = have_real_size ? total_sectors : 0;
    for (int i = 0; i < 512; i++) sector_buffer[i] = ((uint8_t*)&new_hdr)[i];
    ata_write_sector(0, sector_buffer);

    current_dir_cluster = FAT32_ROOT_CLUSTER;
}

// Устанавливает значение в FAT32 (оба экземпляра FAT)
// Важно: сохраняем старшие 4 бита записи (спецификация FAT32)
void fat32_set_entry(uint32_t cluster, uint32_t value) {
    if (cluster < 2) {
        println("FAT32 ERROR: Cannot modify reserved clusters");
        return;
    }

    uint32_t fat_sector_lba  = FAT32_FAT_START + (cluster / FAT32_ENTRIES_PER_FAT_SECTOR);
    uint32_t fat_entry_index = cluster % FAT32_ENTRIES_PER_FAT_SECTOR;

    static uint8_t fat_buf[FAT32_BYTES_PER_SECTOR];

    ata_read_sector(fat_sector_lba);
    for (int i = 0; i < FAT32_BYTES_PER_SECTOR; i++) fat_buf[i] = sector_buffer[i];

    uint32_t* fat_table = (uint32_t*)fat_buf;

    // Сохраняем верхние 4 бита, пишем только нижние 28
    fat_table[fat_entry_index] = (fat_table[fat_entry_index] & 0xF0000000)
                                | (value & FAT32_MASK);

    for (int i = 0; i < FAT32_BYTES_PER_SECTOR; i++) sector_buffer[i] = fat_buf[i];

    ata_write_sector(fat_sector_lba, sector_buffer);

    // Зеркало FAT
    if (FAT32_NUMBER_OF_FATS > 1) {
        ata_write_sector(fat_sector_lba + g_sectors_per_fat, sector_buffer);
    }
}

// Форвард-декларации LFN-хелперов (определены ниже, но используются в
// fat32_delete_entry, который стоит по файлу раньше их определения)
static uint8_t fat32_lfn_checksum(const char* short_name11);
static void fat32_erase_lfn_before(uint32_t dir_cluster, uint32_t short_lba,
                                    int short_idx, uint8_t checksum);

// Ищет свободный кластер (возвращает 0 если нет свободных)
uint32_t fat32_find_free_cluster() {
    // Начинаем с кластера 3 (2 — корень)
    for (uint32_t cluster = 3; cluster < g_max_cluster; cluster++) {
        uint32_t fat_sector_lba  = FAT32_FAT_START + (cluster / FAT32_ENTRIES_PER_FAT_SECTOR);
        uint32_t fat_entry_index = cluster % FAT32_ENTRIES_PER_FAT_SECTOR;

        ata_read_sector(fat_sector_lba);
        uint32_t* fat_table = (uint32_t*)sector_buffer;

        if ((fat_table[fat_entry_index] & FAT32_MASK) == FAT32_FREE) {
            return cluster;
        }
    }
    return 0; // Нет свободных кластеров
}

// **********************************************
// ===== Запись данных в кластер =====
// **********************************************

void fat32_write_cluster_data(uint32_t cluster, const uint8_t* data,
                               uint32_t size, uint32_t offset) {
    if (cluster < 2) {
        println("FAT32 ERROR: Invalid cluster in write");
        return;
    }

    uint32_t cluster_lba       = fat32_cluster_to_lba(cluster);
    uint32_t bytes_per_cluster = FAT32_SECTORS_PER_CLUSTER * FAT32_BYTES_PER_SECTOR;

    if (offset >= bytes_per_cluster) {
        println("FAT32 ERROR: Offset exceeds cluster size");
        return;
    }

    uint32_t bytes_to_write = size;
    if (offset + size > bytes_per_cluster)
        bytes_to_write = bytes_per_cluster - offset;

    static uint8_t cluster_buf[FAT32_BYTES_PER_SECTOR];

    for (uint8_t sec = 0; sec < FAT32_SECTORS_PER_CLUSTER; sec++) {
        uint32_t sector_offset = (uint32_t)sec * FAT32_BYTES_PER_SECTOR;

        // Проверяем, попадает ли данный сектор в диапазон записи
        if (sector_offset + FAT32_BYTES_PER_SECTOR <= offset) continue;
        if (sector_offset >= offset + bytes_to_write) continue;

        ata_read_sector(cluster_lba + sec);
        for (int i = 0; i < FAT32_BYTES_PER_SECTOR; i++) cluster_buf[i] = sector_buffer[i];

        for (uint32_t i = 0; i < FAT32_BYTES_PER_SECTOR; i++) {
            uint32_t file_pos = sector_offset + i;
            if (file_pos >= offset && file_pos < offset + bytes_to_write) {
                cluster_buf[i] = data[file_pos - offset];
            }
        }

        for (int i = 0; i < FAT32_BYTES_PER_SECTOR; i++) sector_buffer[i] = cluster_buf[i];
        ata_write_sector(cluster_lba + sec, sector_buffer);
    }
}

// **********************************************
// ===== Directory Operations =====
// **********************************************

// Внутренняя функция обхода директории по кластеру
// Вызывает callback для каждой записи; возвращает entry + lba + index если callback вернул true
// Сравнение строк без учёта регистра (для сопоставления с LFN-именами)
static bool fat32_str_ieq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

// Разбирает одну LFN-запись каталога (raw — сырые 32 байта записи) и
// добавляет извлечённый кусок имени (до 13 символов) В НАЧАЛО lfn_buf/lfn_len
// — LFN-записи в FAT32 идут в обратном порядке, от последней части имени к
// первой. Общий кусок: раньше был отдельно продублирован посимвольно
// одинаково в fat32_find_entry (эта функция), fat32_tab_complete, а также
// в cmd_ls_disk (builtin.cpp) и fs_read_dir (cmd_gite.cpp).
void fat32_lfn_accumulate(const uint8_t* raw, char* lfn_buf, int* lfn_len, int lfn_buf_size) {
    char chunk[13]; int ci = 0;
    for (int k = 1; k <= 9; k += 2) {
        if (raw[k] != 0xFF && raw[k] != 0x00) chunk[ci++] = (char)raw[k];
        else if (raw[k] == 0x00) chunk[ci++] = 0;
    }
    for (int k = 14; k <= 24; k += 2) {
        if (raw[k] != 0xFF && raw[k] != 0x00) chunk[ci++] = (char)raw[k];
        else if (raw[k] == 0x00) chunk[ci++] = 0;
    }
    for (int k = 28; k <= 30; k += 2) {
        if (raw[k] != 0xFF && raw[k] != 0x00) chunk[ci++] = (char)raw[k];
        else if (raw[k] == 0x00) chunk[ci++] = 0;
    }
    int chlen = 0;
    while (chlen < 13 && chunk[chlen] != 0) chlen++;
    if (*lfn_len + chlen < lfn_buf_size - 1) {
        for (int k = *lfn_len - 1; k >= 0; k--) lfn_buf[k + chlen] = lfn_buf[k];
        for (int k = 0; k < chlen; k++) lfn_buf[k] = chunk[k];
        *lfn_len += chlen;
    }
    lfn_buf[*lfn_len] = 0;
}

// raw_name — исходное (не приведённое к 8.3) имя, нужно для сравнения с LFN.
// Если raw_name == NULL, сравниваем только по короткому 8.3-имени (name_8_3).
static FAT32_DirEntry* scan_dir_cluster(uint32_t dir_cluster,
                                         const char* name_8_3,
                                         const char* raw_name,
                                         uint8_t target_attr,
                                         bool find_free,
                                         uint32_t* lba_out,
                                         int* index_out) {
    uint32_t c = dir_cluster;
    char lfn_buf[256]; int lfn_len = 0;
    uint32_t safety = 0; // защита от зацикленной (повреждённой) цепочки кластеров

    while ((c & FAT32_MASK) >= 2 && (c & FAT32_MASK) < 0x0FFFFFF8) {
        if (++safety > 100000) {
            println("FAT32 ERROR: scan_dir_cluster cluster-chain loop guard tripped (corrupt/cyclic chain?)");
            return NULL;
        }
        uint32_t base_lba = fat32_cluster_to_lba(c);

        for (uint8_t sec = 0; sec < FAT32_SECTORS_PER_CLUSTER; sec++) {
            uint32_t lba = base_lba + sec;
            ata_read_sector(lba);

            for (int i = 0; i < FAT32_ENTRIES_PER_SECTOR; i++) {
                FAT32_DirEntry* e = (FAT32_DirEntry*)(sector_buffer + i * FAT32_ENTRY_SIZE);

                if (find_free) {
                    // Ищем свободную запись
                    if (e->name[0] == 0x00 || e->name[0] == (char)0xE5) {
                        *lba_out   = lba;
                        *index_out = i;
                        return e;
                    }
                    continue;
                }

                // Ищем запись по имени
                if (e->name[0] == 0x00) return NULL; // Конец каталога
                if (e->name[0] == (char)0xE5) { lfn_len = 0; continue; } // Удалённая запись

                if (e->attributes == 0x0F) {
                    // LFN-запись — копим кусок длинного имени
                    uint8_t* raw = (uint8_t*)e;
                    fat32_lfn_accumulate(raw, lfn_buf, &lfn_len, sizeof(lfn_buf));
                    continue;
                }

                if (target_attr != 0 && !(e->attributes & target_attr)) { lfn_len = 0; continue; }

                bool matched = false;
                if (raw_name && lfn_len > 0) matched = fat32_str_ieq(lfn_buf, raw_name);
                if (!matched && name_8_3) matched = (strncmp(e->name, name_8_3, 11) == 0);
                lfn_len = 0;

                if (matched) {
                    *lba_out   = lba;
                    *index_out = i;
                    return e;
                }
            }
        }

        // Переходим к следующему кластеру цепочки
        uint32_t next = fat32_get_next_cluster(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) break;
        c = next;
    }

    return NULL;
}

FAT32_DirEntry* fat32_find_entry_for_modification(const char* name,
                                                   uint32_t* lba_out,
                                                   int* index_out,
                                                   uint8_t target_attr) {
    char name_8_3[11];

    if (strcmp(name, ".") == 0) {
        for (int i = 0; i < 11; i++) name_8_3[i] = ' ';
        name_8_3[0] = '.';
        return scan_dir_cluster(current_dir_cluster, name_8_3, NULL, target_attr,
                                 false, lba_out, index_out);
    } else if (strcmp(name, "..") == 0) {
        for (int i = 0; i < 11; i++) name_8_3[i] = ' ';
        name_8_3[0] = '.';
        name_8_3[1] = '.';
        return scan_dir_cluster(current_dir_cluster, name_8_3, NULL, target_attr,
                                 false, lba_out, index_out);
    }

    fat32_format_name(name, name_8_3);
    // raw_name = исходное имя (до 8.3-фолдинга) — нужно, чтобы находить файлы
    // с длинными/смешанного регистра именами по их настоящему (LFN) имени.
    return scan_dir_cluster(current_dir_cluster, name_8_3, name, target_attr,
                             false, lba_out, index_out);
}

FAT32_FindResult fat32_find_entry(const char* name, uint8_t target_attr) {
    FAT32_FindResult result = {0};
    uint32_t lba = 0;
    int index    = 0;

    FAT32_DirEntry* e = fat32_find_entry_for_modification(name, &lba, &index, target_attr);

    if (e) {
        result.found     = true;
        result.entry     = *e;
        result.lba       = lba;
        result.index     = index;
        result.entry_ptr = e;
    }

    return result;
}

FAT32_DirEntry* fat32_find_free_dir_entry(uint32_t* lba_out) {
    int dummy = 0;

    // Ищем свободную запись в текущей директории
    FAT32_DirEntry* e = scan_dir_cluster(current_dir_cluster, NULL, NULL, 0,
                                          true, lba_out, &dummy);
    if (e) return e;

    // Свободных нет — расширяем директорию новым кластером
    uint32_t new_cluster = fat32_find_free_cluster();
    if (new_cluster == 0) {
        println("FAT32 ERROR: No free clusters to expand directory");
        return NULL;
    }

    // Очищаем новый кластер
    uint8_t empty[FAT32_BYTES_PER_SECTOR] = {0};
    uint32_t new_lba = fat32_cluster_to_lba(new_cluster);
    for (uint8_t s = 0; s < FAT32_SECTORS_PER_CLUSTER; s++) {
        ata_write_sector(new_lba + s, empty);
    }

    // Связываем новый кластер с концом цепочки директории
    uint32_t c = current_dir_cluster;
    while (true) {
        uint32_t next = fat32_get_next_cluster(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) break;
        c = next;
    }
    fat32_set_entry(c, new_cluster);
    fat32_set_entry(new_cluster, FAT32_EOC);

    // Первая запись нового кластера — свободная
    ata_read_sector(new_lba);
    *lba_out = new_lba;
    return (FAT32_DirEntry*)sector_buffer;
}

// **********************************************
// ===== Deletion =====
// **********************************************

void fat32_delete_cluster_chain(uint32_t start_cluster) {
    if (start_cluster < 2) {
        println("FAT32 ERROR: Cannot delete reserved cluster");
        return;
    }

    uint32_t current = start_cluster;
    uint32_t count   = 0;

    while ((current & FAT32_MASK) >= 2 &&
           (current & FAT32_MASK) < (FAT32_EOC & FAT32_MASK)) {
        uint32_t next = fat32_get_next_cluster(current);
        fat32_set_entry(current, FAT32_FREE);
        current = next;
        count++;

        if (count > 100000) {
            println("FAT32 ERROR: Chain too long, possible loop");
            break;
        }
    }
}

void fat32_delete_entry(const char* name, uint8_t target_attr) {
    uint32_t lba = 0;
    int      idx = 0;

    FAT32_DirEntry* entry = fat32_find_entry_for_modification(name, &lba, &idx, target_attr);
    if (!entry) {
        println("FAT32 ERROR: Entry not found");
        return;
    }

    // Проверка на . и ..
    if (strncmp(entry->name, ".          ", 11) == 0 ||
        strncmp(entry->name, "..         ", 11) == 0) {
        println("FAT32 ERROR: Cannot delete . or ..");
        return;
    }

    uint32_t start_cluster = FAT32_GET_CLUSTER(entry);
    uint8_t  attributes    = entry->attributes;
    uint8_t  checksum      = fat32_lfn_checksum((const char*)entry); // забираем ДО дальнейших чтений диска

    // Для директорий — проверяем пустоту
    if (attributes & 0x10) {
        uint32_t c = start_cluster;
        while ((c & FAT32_MASK) < (FAT32_EOC & FAT32_MASK)) {
            uint32_t dir_lba = fat32_cluster_to_lba(c);
            for (uint8_t sec = 0; sec < FAT32_SECTORS_PER_CLUSTER; sec++) {
                ata_read_sector(dir_lba + sec);
                for (int i = 0; i < FAT32_ENTRIES_PER_SECTOR; i++) {
                    FAT32_DirEntry* de = (FAT32_DirEntry*)(sector_buffer + i * FAT32_ENTRY_SIZE);
                    if (de->name[0] == 0x00) goto done_check;
                    if (de->name[0] == (char)0xE5) continue;
                    if (strncmp(de->name, ".          ", 11) == 0 ||
                        strncmp(de->name, "..         ", 11) == 0) continue;
                    println("FAT32 ERROR: Directory not empty");
                    return;
                }
            }
            c = fat32_get_next_cluster(c);
        }
        done_check:;
    }

    // Сохраняем сектор директории (fat32_delete_cluster_chain изменит sector_buffer)
    static uint8_t dir_backup[FAT32_BYTES_PER_SECTOR];
    ata_read_sector(lba);
    for (int i = 0; i < FAT32_BYTES_PER_SECTOR; i++) dir_backup[i] = sector_buffer[i];

    // Удаляем цепочку кластеров
    if (start_cluster >= 2) fat32_delete_cluster_chain(start_cluster);

    // Восстанавливаем сектор и помечаем запись удалённой
    for (int i = 0; i < FAT32_BYTES_PER_SECTOR; i++) sector_buffer[i] = dir_backup[i];

    FAT32_DirEntry* e_in_buf = (FAT32_DirEntry*)(sector_buffer + idx * FAT32_ENTRY_SIZE);
    e_in_buf->name[0] = (char)0xE5;

    ata_write_sector(lba, sector_buffer);
    fat32_erase_lfn_before(current_dir_cluster, lba, idx, checksum);
    //println("FAT32: Entry deleted");
}

// **********************************************
// ===== LFN (длинные имена, разный регистр) =====
// **********************************************

// Нужен ли LFN для этого имени (не укладывается в классическое 8.3)?
static bool fat32_needs_lfn(const char* name) {
    int dot = -1, len = 0, dots = 0;
    while (name[len]) {
        char c = name[len];
        if (c >= 'a' && c <= 'z') return true;   // строчные буквы -> нужен LFN
        if (c == ' ') return true;               // пробел внутри имени -> нужен LFN
        if (c == '.') { dot = len; dots++; }
        len++;
    }
    if (dots > 1) return true;                   // больше одной точки
    int base_len = (dot >= 0) ? dot : len;
    int ext_len  = (dot >= 0) ? (len - dot - 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) return true;
    return false;
}

// Контрольная сумма short-имени для LFN-записей (алгоритм из спецификации FAT)
static uint8_t fat32_lfn_checksum(const char* short_name11) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short_name11[i]);
    return sum;
}

// Генерирует короткий алиас вида "IMENA~1.TXT" для длинного/смешанного имени,
// подбирая числовой хвост так, чтобы не столкнуться с уже существующим файлом.
static void fat32_make_short_alias(const char* name, char out11[11]) {
    char base[7]; int bi = 0;
    char ext[4];  int ei = 0;
    int i = 0;

    while (name[i] && name[i] != '.' && bi < 6) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ') continue;
        base[bi++] = c;
    }
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') i++;
    while (name[i] && ei < 3) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == ' ') continue;
        ext[ei++] = c;
    }
    if (bi == 0) { base[0] = '_'; bi = 1; } // на случай "чистого" имени вроде ".env"

    for (int n = 1; n <= 9; n++) {
        char cand[11];
        for (int k = 0; k < 11; k++) cand[k] = ' ';
        int p = 0;
        for (int k = 0; k < bi && p < 7; k++) cand[p++] = base[k];
        cand[p++] = '~';
        cand[p++] = (char)('0' + n);
        for (int k = 0; k < ei; k++) cand[8 + k] = ext[k];

        uint32_t lba_d; int idx_d;
        if (!scan_dir_cluster(current_dir_cluster, cand, NULL, 0, false, &lba_d, &idx_d)) {
            for (int k = 0; k < 11; k++) out11[k] = cand[k];
            return;
        }
    }
    // >9 коллизий подряд — крайний случай, берём хвост ~9 как есть
    for (int k = 0; k < 11; k++) out11[k] = ' ';
    int p = 0;
    for (int k = 0; k < bi && p < 7; k++) out11[p++] = base[k];
    out11[p++] = '~'; out11[p++] = '9';
    for (int k = 0; k < ei; k++) out11[8 + k] = ext[k];
}

// Заполняет одну 32-байтную LFN-запись сырыми байтами (raw — указатель на 32 байта в sector_buffer)
static void fat32_write_lfn_chunk(uint8_t* raw, int ord, bool last,
                                   const char* name, int char_start, uint8_t checksum) {
    raw[0]  = (uint8_t)(ord | (last ? 0x40 : 0));
    raw[11] = 0x0F;      // attributes: LFN
    raw[12] = 0x00;      // type
    raw[13] = checksum;
    raw[26] = 0x00; raw[27] = 0x00; // "cluster" всегда 0 у LFN-записей

    int name_len = 0; while (name[name_len]) name_len++;
    static const int pos[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    bool ended = false;
    for (int k = 0; k < 13; k++) {
        int ch_idx = char_start + k;
        int off = pos[k];
        if (!ended && ch_idx < name_len) {
            raw[off] = (uint8_t)name[ch_idx];
            raw[off + 1] = 0x00;
        } else if (!ended) {
            raw[off] = 0x00; raw[off + 1] = 0x00; // терминатор строки
            ended = true;
        } else {
            raw[off] = 0xFF; raw[off + 1] = 0xFF; // паддинг
        }
    }
}

// Выделяет `count` ПОДРЯД идущих свободных слотов в текущей директории
// (для LFN-цепочки + короткой записи). При нехватке места расширяет
// директорию новым кластером, как и старый fat32_find_free_dir_entry.
static bool fat32_alloc_dir_slots(int count, uint32_t* lba_arr, int* idx_arr) {
    if (count > 17) return false;
    uint32_t win_lba[17]; int win_idx[17]; int win_n = 0;

    uint32_t c = current_dir_cluster;
    while (true) {
        uint32_t base_lba = fat32_cluster_to_lba(c);
        for (uint8_t sec = 0; sec < FAT32_SECTORS_PER_CLUSTER; sec++) {
            uint32_t lba = base_lba + sec;
            ata_read_sector(lba);
            for (int i = 0; i < FAT32_ENTRIES_PER_SECTOR; i++) {
                FAT32_DirEntry* e = (FAT32_DirEntry*)(sector_buffer + i * FAT32_ENTRY_SIZE);
                bool is_free = (e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5);
                if (is_free) {
                    win_lba[win_n] = lba; win_idx[win_n] = i; win_n++;
                    if (win_n == count) {
                        for (int k = 0; k < count; k++) { lba_arr[k] = win_lba[k]; idx_arr[k] = win_idx[k]; }
                        return true;
                    }
                } else {
                    win_n = 0; // слоты должны быть строго последовательными
                }
            }
        }

        uint32_t next = fat32_get_next_cluster(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) {
            // Расширяем директорию новым кластером
            uint32_t new_cluster = fat32_find_free_cluster();
            if (new_cluster == 0) return false;
            uint8_t empty[FAT32_BYTES_PER_SECTOR] = {0};
            uint32_t new_lba = fat32_cluster_to_lba(new_cluster);
            for (uint8_t s = 0; s < FAT32_SECTORS_PER_CLUSTER; s++) ata_write_sector(new_lba + s, empty);
            fat32_set_entry(c, new_cluster);
            fat32_set_entry(new_cluster, FAT32_EOC);
            c = new_cluster;
            continue;
        }
        c = next;
    }
}

// Пишет короткую запись (+ LFN-цепочку, если имя не укладывается в 8.3)
// для уже готового кластера/размера. Используется create_file/create_dir/rename.
static bool fat32_write_named_entry(const char* name, uint8_t attributes,
                                     uint32_t cluster, uint32_t file_size) {
    bool need_lfn = fat32_needs_lfn(name);
    char short_name[11];
    int n_lfn = 0;
    int name_len = 0; while (name[name_len]) name_len++;

    if (need_lfn) {
        fat32_make_short_alias(name, short_name);
        n_lfn = (name_len + 12) / 13;
        if (n_lfn < 1)  n_lfn = 1;
        if (n_lfn > 16) n_lfn = 16; // защита от переполнения на совсем безумных именах
    } else {
        fat32_format_name(name, short_name);
    }

    int total_slots = n_lfn + 1;
    uint32_t slot_lba[17]; int slot_idx[17];
    if (!fat32_alloc_dir_slots(total_slots, slot_lba, slot_idx)) return false;

    uint8_t checksum = fat32_lfn_checksum(short_name);
    for (int p = 0; p < n_lfn; p++) {
        int ord = n_lfn - p;
        bool last = (ord == n_lfn);
        int char_start = (ord - 1) * 13;
        ata_read_sector(slot_lba[p]);
        uint8_t* raw = sector_buffer + slot_idx[p] * FAT32_ENTRY_SIZE;
        fat32_write_lfn_chunk(raw, ord, last, name, char_start, checksum);
        ata_write_sector(slot_lba[p], sector_buffer);
    }

    uint32_t dir_lba = slot_lba[n_lfn];
    int dir_idx = slot_idx[n_lfn];
    ata_read_sector(dir_lba);
    FAT32_DirEntry* entry = (FAT32_DirEntry*)(sector_buffer + dir_idx * FAT32_ENTRY_SIZE);

    for (int k = 0; k < 8; k++) entry->name[k] = short_name[k];
    for (int k = 0; k < 3; k++) entry->ext[k]  = short_name[8 + k];
    entry->attributes     = attributes;
    entry->nt_reserved    = 0;
    entry->crt_time_tenth = 0;
    entry->crt_time       = 0;
    entry->crt_date       = 0;
    entry->lst_acc_date   = 0;
    entry->wrt_time       = 0;
    entry->wrt_date       = 0;
    entry->file_size      = file_size;
    FAT32_SET_CLUSTER(entry, cluster);

    ata_write_sector(dir_lba, sector_buffer);
    return true;
}

// Стирает LFN-цепочку, идущую непосредственно ПЕРЕД короткой записью по адресу
// (short_lba, short_idx). Идёт назад по записям директории и снимает их, пока
// не встретит запись с установленным битом 0x40 (последняя/первая физически
// LFN-запись данного файла) или чужую запись (несовпадение чек-суммы).
// Ограничение: не переходит назад через границу кластера директории (редкий
// edge-case для имён, у которых LFN-цепочка начинается в предыдущем кластере).
static void fat32_erase_lfn_before(uint32_t dir_cluster, uint32_t short_lba,
                                    int short_idx, uint8_t checksum) {
    uint32_t c = dir_cluster;
    uint32_t cluster_lba = 0;
    int sec = -1;
    while (true) {
        uint32_t base = fat32_cluster_to_lba(c);
        if (short_lba >= base && short_lba < base + FAT32_SECTORS_PER_CLUSTER) {
            cluster_lba = base;
            sec = (int)(short_lba - base);
            break;
        }
        uint32_t next = fat32_get_next_cluster(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) return; // не должно случиться
        c = next;
    }

    int idx = short_idx;
    while (true) {
        idx--;
        if (idx < 0) {
            sec--;
            if (sec < 0) return; // граница кластера — останавливаемся (см. комментарий выше)
            idx = FAT32_ENTRIES_PER_SECTOR - 1;
        }
        uint32_t lba = cluster_lba + sec;
        ata_read_sector(lba);
        uint8_t* raw = sector_buffer + idx * FAT32_ENTRY_SIZE;
        if (raw[11] != 0x0F)   return; // не LFN — чужая запись
        if (raw[13] != checksum) return; // чек-сумма не совпала — чужая запись
        bool last = (raw[0] & 0x40) != 0;
        raw[0] = (uint8_t)0xE5;
        ata_write_sector(lba, sector_buffer);
        if (last) return;
    }
}

// **********************************************
// ===== File Creation =====
// **********************************************

bool fat32_create_file(const char* name, const char* content, uint32_t size) {
    uint32_t cluster_size       = FAT32_SECTORS_PER_CLUSTER * FAT32_BYTES_PER_SECTOR;
    uint32_t num_clusters       = (size + cluster_size - 1) / cluster_size;
    if (num_clusters == 0) num_clusters = 1;

    if (num_clusters > 256) {
        println("FAT32 ERROR: File too large");
        return false;
    }

    uint32_t clusters[256];
    for (uint32_t i = 0; i < num_clusters; i++) {
        clusters[i] = fat32_find_free_cluster();
        if (clusters[i] == 0) {
            println("FAT32 ERROR: Not enough free clusters");
            return false;
        }
        // Временно помечаем как занятый чтобы find_free не вернул тот же кластер
        fat32_set_entry(clusters[i], FAT32_EOC);
    }

    // Строим цепочку FAT
    for (uint32_t i = 0; i < num_clusters - 1; i++) {
        fat32_set_entry(clusters[i], clusters[i + 1]);
    }
    fat32_set_entry(clusters[num_clusters - 1], FAT32_EOC);

    // Пишем данные. content==nullptr — специальный режим: просто зануляем
    // все кластеры, не требуя, чтобы весь файл целиком лежал в ОЗУ разом
    // (нужно для резервирования места под большие файлы порциями, например
    // под <имя>.temp в постраничном nano — см. cmd_nano.cpp).
    uint32_t remaining = size;
    uint32_t written   = 0;
    if (content == nullptr) {
        static uint8_t zero_cluster[FAT32_SECTORS_PER_CLUSTER * FAT32_BYTES_PER_SECTOR] = {0};
        for (uint32_t i = 0; i < num_clusters && remaining > 0; i++) {
            uint32_t to_write = (remaining > cluster_size) ? cluster_size : remaining;
            fat32_write_cluster_data(clusters[i], zero_cluster, to_write, 0);
            written   += to_write;
            remaining -= to_write;
        }
    } else {
        for (uint32_t i = 0; i < num_clusters && remaining > 0; i++) {
            uint32_t to_write = (remaining > cluster_size) ? cluster_size : remaining;
            fat32_write_cluster_data(clusters[i], (const uint8_t*)content + written, to_write, 0);
            written   += to_write;
            remaining -= to_write;
        }
    }

    // Создаём запись директории (+ LFN-цепочку, если имя не укладывается в 8.3)
    if (!fat32_write_named_entry(name, 0x20 /* ARCHIVE */, clusters[0], size)) {
        println("FAT32 ERROR: No free directory entry, rolling back");
        for (uint32_t i = 0; i < num_clusters; i++) fat32_set_entry(clusters[i], FAT32_FREE);
        return false;
    }
    //println("FAT32: File created");
    return true;
}

// **********************************************
// ===== Directory Creation =====
// **********************************************

void fat32_create_dir(char* name) {
    uint32_t new_cluster = fat32_find_free_cluster();
    if (new_cluster == 0) {
        println("FAT32 ERROR: No free clusters");
        return;
    }
    fat32_set_entry(new_cluster, FAT32_EOC);

    // Инициализируем кластер новой директории
    uint8_t dir_buf[FAT32_BYTES_PER_SECTOR] = {0};
    FAT32_DirEntry* dot    = (FAT32_DirEntry*)dir_buf;
    FAT32_DirEntry* dotdot = (FAT32_DirEntry*)(dir_buf + FAT32_ENTRY_SIZE);

    // Запись "."
    for (int i = 0; i < 8; i++) dot->name[i] = ' ';
    for (int i = 0; i < 3; i++) dot->ext[i]  = ' ';
    dot->name[0] = '.';
    dot->attributes = 0x10;
    FAT32_SET_CLUSTER(dot, new_cluster);

    // Запись ".."
    for (int i = 0; i < 8; i++) dotdot->name[i] = ' ';
    for (int i = 0; i < 3; i++) dotdot->ext[i]  = ' ';
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attributes = 0x10;
    FAT32_SET_CLUSTER(dotdot, current_dir_cluster);

    uint32_t new_dir_lba = fat32_cluster_to_lba(new_cluster);
    ata_write_sector(new_dir_lba, dir_buf);

    // Очищаем оставшиеся секторы кластера
    uint8_t empty[FAT32_BYTES_PER_SECTOR] = {0};
    for (uint8_t s = 1; s < FAT32_SECTORS_PER_CLUSTER; s++) {
        ata_write_sector(new_dir_lba + s, empty);
    }

    // Добавляем запись в родительскую директорию (+ LFN, если нужно)
    if (!fat32_write_named_entry(name, 0x10 /* DIRECTORY */, new_cluster, 0)) {
        fat32_delete_cluster_chain(new_cluster);
        println("FAT32 ERROR: Parent directory full");
        return;
    }
    //println("FAT32: Directory created");
}

// **********************************************
// ===== Rename =====
// **********************************************

void fat32_rename_entry(char* old_name, char* new_name) {
    uint32_t lba = 0;
    int      idx = 0;

    FAT32_DirEntry* entry = fat32_find_entry_for_modification(old_name, &lba, &idx, 0x00);
    if (!entry) {
        println("FAT32 ERROR: Item not found");
        return;
    }

    if (fat32_find_entry(new_name, 0x00).found) {
        println("FAT32 ERROR: Name already exists");
        return;
    }

    // entry указывает внутрь sector_buffer — забираем данные, пока их не затёрли
    uint32_t cluster     = FAT32_GET_CLUSTER(entry);
    uint32_t file_size   = entry->file_size;
    uint8_t  attributes  = entry->attributes;
    uint8_t  old_checksum = fat32_lfn_checksum((const char*)entry);

    // Удаляем старую короткую запись и её LFN-цепочку (если была)
    entry->name[0] = (char)0xE5;
    ata_write_sector(lba, sector_buffer);
    fat32_erase_lfn_before(current_dir_cluster, lba, idx, old_checksum);

    // Пишем новую запись с тем же кластером/размером/атрибутами
    if (!fat32_write_named_entry(new_name, attributes, cluster, file_size)) {
        println("FAT32 ERROR: Rename failed (no space for new entry)");
        // Примечание: старая запись уже помечена удалённой, а новую создать не
        // удалось (директория переполнена) — данные кластера при этом не теряются,
        // но временно окажутся без записи в каталоге. Редкий крайний случай.
    }
    //println("FAT32: Renamed successfully");
}

// Переносит запись между директориями (в т.ч. переименовывая). Безопасен
// относительно перезатирания sector_buffer промежуточными вызовами: все
// нужные поля (кластер/размер/атрибуты/чексумма) забираются из src ДО
// какого-либо чтения диска, которое могло бы затереть буфер.
bool fat32_move_entry(uint32_t src_dir_cluster, const char* src_leaf,
                       uint32_t dst_dir_cluster, const char* dst_leaf) {
    if (src_dir_cluster == dst_dir_cluster && fat32_str_ieq(src_leaf, dst_leaf))
        return true; // источник и назначение — одно и то же, ничего не делаем

    uint32_t saved = current_dir_cluster;

    current_dir_cluster = src_dir_cluster;
    uint32_t src_lba; int src_idx;
    FAT32_DirEntry* src_entry = fat32_find_entry_for_modification(src_leaf, &src_lba, &src_idx, 0x00);
    if (!src_entry) { current_dir_cluster = saved; return false; }

    uint32_t cluster     = FAT32_GET_CLUSTER(src_entry);
    uint32_t file_size   = src_entry->file_size;
    uint8_t  attributes  = src_entry->attributes;
    uint8_t  checksum    = fat32_lfn_checksum((const char*)src_entry);
    // src_entry дальше не используем — следующий вызов перезатрёт sector_buffer

    current_dir_cluster = dst_dir_cluster;
    bool dst_exists = fat32_find_entry(dst_leaf, 0x00).found;
    if (dst_exists) { current_dir_cluster = saved; return false; }

    // Удаляем старую запись в исходной директории (пересчитываем адрес сектора
    // на диске — sector_buffer уже был перезатёрт проверкой коллизии выше)
    current_dir_cluster = src_dir_cluster;
    ata_read_sector(src_lba);
    FAT32_DirEntry* e2 = (FAT32_DirEntry*)(sector_buffer + src_idx * FAT32_ENTRY_SIZE);
    e2->name[0] = (char)0xE5;
    ata_write_sector(src_lba, sector_buffer);
    fat32_erase_lfn_before(src_dir_cluster, src_lba, src_idx, checksum);

    // Пишем новую запись в целевой директории с тем же кластером/размером/атрибутами
    current_dir_cluster = dst_dir_cluster;
    bool ok = fat32_write_named_entry(dst_leaf, attributes, cluster, file_size);

    current_dir_cluster = saved;
    return ok;
}

// **********************************************
// ===== Разбор путей ("/a/b/c") =====
// **********************************************

// Разбирает путь на кластер содержащей директории + последний сегмент (leaf).
// "/a/b/c"  -> заходим в a, затем в b (обе должны существовать и быть директориями),
//              *out_dir_cluster = кластер b, out_leaf = "c"
// "a/b/"    -> leaf будет пустой строкой ("") — путь указывает на саму директорию b
//              (используется, например, в mv, когда цель — существующая папка)
// Относительный путь (без ведущего "/") стартует от current_dir_cluster.
bool fat32_resolve_path(const char* path, uint32_t* out_dir_cluster, char* out_leaf) {
    if (!path || path[0] == 0) return false;

    uint32_t cluster;
    int i;
    if (path[0] == '/') { cluster = FAT32_ROOT_CLUSTER; i = 1; }
    else                { cluster = current_dir_cluster; i = 0; }

    while (path[i] == '/') i++;

    while (true) {
        char seg[128]; int si = 0;
        while (path[i] && path[i] != '/' && si < 127) seg[si++] = path[i++];
        while (path[i] && path[i] != '/') i++;   // остаток слишком длинного сегмента — пропускаем
        seg[si] = 0;

        while (path[i] == '/') i++;

        if (!path[i]) {
            // Последний сегмент пути — это leaf (может быть и пустым, см. комментарий выше)
            int k = 0; while (seg[k]) { out_leaf[k] = seg[k]; k++; }
            out_leaf[k] = 0;
            *out_dir_cluster = cluster;
            return true;
        }

        if (si == 0) continue; // защитный случай (практически недостижим)

        uint32_t saved = current_dir_cluster;
        current_dir_cluster = cluster;
        FAT32_FindResult r = fat32_find_entry(seg, 0x10);
        current_dir_cluster = saved;

        if (!r.found || !(r.entry.attributes & 0x10)) return false; // нет такой директории

        uint32_t next_cluster = FAT32_GET_CLUSTER(&r.entry);
        cluster = (next_cluster < 2) ? FAT32_ROOT_CLUSTER : next_cluster; // ".." из подкаталога рута
    }
}

// **********************************************
// ===== Debug =====
// **********************************************

void fat32_debug_cluster_state(uint32_t cluster) {
    if (cluster < 2) { println("FAT32 DEBUG: Reserved cluster"); return; }

    uint32_t fat_sector_lba  = FAT32_FAT_START + (cluster / FAT32_ENTRIES_PER_FAT_SECTOR);
    uint32_t fat_entry_index = cluster % FAT32_ENTRIES_PER_FAT_SECTOR;

    ata_read_sector(fat_sector_lba);
    uint32_t* fat_table = (uint32_t*)sector_buffer;
    uint32_t  value     = fat_table[fat_entry_index] & FAT32_MASK;

    print("Cluster 0x");
    print_hex_byte((cluster >> 24) & 0xFF);
    print_hex_byte((cluster >> 16) & 0xFF);
    print_hex_byte((cluster >> 8)  & 0xFF);
    print_hex_byte(cluster         & 0xFF);
    print(": 0x");
    print_hex_byte((value >> 24) & 0xFF);
    print_hex_byte((value >> 16) & 0xFF);
    print_hex_byte((value >> 8)  & 0xFF);
    print_hex_byte(value         & 0xFF);

    if (value == FAT32_FREE) println(" (FREE)");
    else if ((value & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) println(" (EOC/EOF)");
    else if ((value & FAT32_MASK) == (FAT32_BAD & FAT32_MASK)) println(" (BAD)");
    else println("");
}

void fat32_debug_check_chain(uint32_t start_cluster) {
    println("FAT32 DEBUG: Checking cluster chain");
    uint32_t current = start_cluster;
    uint32_t count   = 0;

    while ((current & FAT32_MASK) >= 2 &&
           (current & FAT32_MASK) < (FAT32_EOC & FAT32_MASK) &&
           count < 200) {
        current = fat32_get_next_cluster(current);
        count++;
    }

    print("  Total clusters: ");
    print_hex_byte((count >> 8) & 0xFF);
    print_hex_byte(count & 0xFF);
    print_char('\n');
}

// Возвращает список имён в текущей директории, начинающихся на prefix.
// results — массив строк, max — максимум совпадений.
// Возвращает количество найденных.
int fat32_tab_complete(const char* prefix, char results[][128], int max) {
    int prefix_len = 0;
    while (prefix[prefix_len]) prefix_len++;

    int count = 0;
    uint32_t c = current_dir_cluster;
    char lfn_buf[128]; int lfn_len = 0;

    while ((c & FAT32_MASK) >= 2 && (c & FAT32_MASK) < 0x0FFFFFF8 && count < max) {
        uint32_t base_lba = fat32_cluster_to_lba(c);
        for (uint8_t sec = 0; sec < FAT32_SECTORS_PER_CLUSTER && count < max; sec++) {
            ata_read_sector(base_lba + sec);
            FAT32_DirEntry* entries = (FAT32_DirEntry*)sector_buffer;
            for (int i = 0; i < 16 && count < max; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) goto done;
                if (first == 0xE5) { lfn_len = 0; continue; }

                if (entries[i].attributes == 0x0F) {
                    // Копим кусок LFN-имени
                    uint8_t* raw = (uint8_t*)&entries[i];
                    fat32_lfn_accumulate(raw, lfn_buf, &lfn_len, sizeof(lfn_buf));
                    continue;
                }
                if (entries[i].name[0] == '.') { lfn_len = 0; continue; } // . и ..

                // Имя для подсказки: настоящее (LFN), если было, иначе декодированное 8.3
                char decoded[128];
                if (lfn_len > 0) {
                    int k = 0; while (k < lfn_len && k < 127) { decoded[k] = lfn_buf[k]; k++; }
                    decoded[k] = 0;
                } else {
                    int di = 0;
                    for (int n = 0; n < 8 && entries[i].name[n] != ' '; n++)
                        decoded[di++] = entries[i].name[n] | 32; // в нижний регистр
                    bool has_ext = false;
                    for (int n = 0; n < 3; n++) if (entries[i].ext[n] != ' ') { has_ext = true; break; }
                    if (has_ext) {
                        decoded[di++] = '.';
                        for (int n = 0; n < 3 && entries[i].ext[n] != ' '; n++)
                            decoded[di++] = entries[i].ext[n] | 32;
                    }
                    decoded[di] = 0;
                }
                lfn_len = 0;

                // Проверяем совпадение с префиксом
                bool match = true;
                for (int n = 0; n < prefix_len; n++) {
                    char a = prefix[n], b = decoded[n];
                    if (a >= 'A' && a <= 'Z') a += 32;
                    if (b >= 'A' && b <= 'Z') b += 32;
                    if (!b || a != b) { match = false; break; }
                }
                if (match) {
                    int k = 0;
                    while (decoded[k]) { results[count][k] = decoded[k]; k++; }
                    results[count][k] = 0;
                    count++;
                }
            }
        }
        c = fat32_get_next_cluster(c);
    }
done:
    return count;
}

} // extern "C"