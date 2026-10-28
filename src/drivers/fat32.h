#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Глобальное состояние FAT32
extern uint32_t current_dir_cluster;

// ===== Константы FAT32 =====
#define FAT32_RESERVED_SECTORS    32      // стандарт: 32 reserved sectors
#define FAT32_NUMBER_OF_FATS      2
#define FAT32_SECTORS_PER_CLUSTER 8       // 8 * 512 = 4096 bytes per cluster
#define FAT32_BYTES_PER_SECTOR    512
#define FAT32_ENTRY_SIZE          32      // directory entry size
#define FAT32_ENTRIES_PER_SECTOR  16      // 512 / 32 = 16 dir entries per sector

// FAT32 entries per FAT sector: 512 / 4 = 128
#define FAT32_ENTRIES_PER_FAT_SECTOR  128

// FAT32 занимает (макс_кластеров / 128) секторов
// Для диска ~2GB: ~512K кластеров по 4K → 4096 секторов FAT
// Для простоты используем 512 секторов FAT (покрывает ~65536 кластеров = 256MB)
#define FAT32_SECTORS_PER_FAT     512

// Специальные значения FAT32
#define FAT32_EOC           0x0FFFFFFF   // End of chain
#define FAT32_BAD           0x0FFFFFF7   // Bad cluster
#define FAT32_FREE          0x00000000   // Free cluster
#define FAT32_MEDIA         0x0FFFFFF8   // Media descriptor

// Маска для чтения FAT32 (28 бит)
#define FAT32_MASK          0x0FFFFFFF

// Вычисленные LBA адреса
#define FAT32_FAT_START     (FAT32_RESERVED_SECTORS)
#define FAT32_DATA_START    (FAT32_FAT_START + (FAT32_NUMBER_OF_FATS * FAT32_SECTORS_PER_FAT))
#define FAT32_ROOT_CLUSTER  2            // Корень начинается с кластера 2

// ===== BPB (Boot Parameter Block) FAT32 =====
struct FAT32_BPB {
    // Общая часть (совместима с FAT16)
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;    // 0 для FAT32
    uint16_t total_sectors_16;    // 0 для FAT32
    uint8_t  media;
    uint16_t fat_size_16;         // 0 для FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32-специфичная расширенная часть (offset 36)
    uint32_t fat_size_32;         // Секторов на FAT
    uint16_t ext_flags;
    uint16_t fs_version;          // 0x0000
    uint32_t root_cluster;        // Первый кластер корневой директории (обычно 2)
    uint16_t fs_info;             // Сектор FSInfo (обычно 1)
    uint16_t backup_boot_sector;  // Сектор резервной копии (обычно 6)
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;      // 0x29
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];          // "FAT32   "
} __attribute__((packed));

extern FAT32_BPB fat32_bpb;

// ===== Запись директории FAT32 (те же 32 байта, что и FAT16) =====
struct FAT32_DirEntry {
    char     name[8];
    char     ext[3];
    uint8_t  attributes;
    uint8_t  nt_reserved;       // Зарезервировано Windows NT
    uint8_t  crt_time_tenth;    // Сотые доли секунды создания
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t cluster_hi;        // Старшие 16 бит номера кластера (FAT32!)
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;        // Младшие 16 бит номера кластера
    uint32_t file_size;
} __attribute__((packed));

// ===== Результат поиска =====
struct FAT32_FindResult {
    bool            found;
    FAT32_DirEntry  entry;
    uint32_t        lba;
    int             index;
    FAT32_DirEntry* entry_ptr;
};

// Вспомогательный макрос для получения полного 32-битного кластера из записи директории
#define FAT32_GET_CLUSTER(e) \
    (((uint32_t)(e)->cluster_hi << 16) | (uint32_t)(e)->cluster_lo)

#define FAT32_SET_CLUSTER(e, c) \
    do { \
        (e)->cluster_hi = (uint16_t)(((c) >> 16) & 0xFFFF); \
        (e)->cluster_lo = (uint16_t)((c) & 0xFFFF); \
    } while(0)

extern "C" {

// ===== ИНИЦИАЛИЗАЦИЯ =====
void fat32_init();

// ===== БАЗОВЫЕ ФУНКЦИИ =====
uint32_t fat32_cluster_to_lba(uint32_t cluster);
uint32_t fat32_get_next_cluster(uint32_t current_cluster);
uint32_t fat32_read_file_to_buffer(uint32_t start_cluster, uint32_t file_size,
                                    uint8_t* buf, uint32_t buf_max);
void fat32_lfn_accumulate(const uint8_t* raw, char* lfn_buf, int* lfn_len, int lfn_buf_size);
uint32_t fat32_find_free_cluster();
void     fat32_set_entry(uint32_t cluster, uint32_t value);
void     fat32_write_cluster_data(uint32_t cluster, const uint8_t* data,
                                  uint32_t size, uint32_t offset);
void     fat32_format_name(const char* input, char* output_8_3);

// ===== ПОИСК =====
FAT32_FindResult  fat32_find_entry(const char* name, uint8_t target_attr);
FAT32_DirEntry*   fat32_find_entry_for_modification(const char* name,
                      uint32_t* lba_out, int* index_out, uint8_t target_attr);
FAT32_DirEntry*   fat32_find_free_dir_entry(uint32_t* lba_out);

// Разбирает путь (абсолютный "/a/b/c" или относительный "a/b/c") на кластер
// СОДЕРЖАЩЕЙ директории + последний сегмент (leaf, буфер >= 128 байт).
// Не меняет current_dir_cluster на выходе — вызывающий сам переключает его
// на *out_dir_cluster перед операцией с leaf и восстанавливает после.
// Возвращает false, если промежуточный сегмент пути не найден или не директория.
bool fat32_resolve_path(const char* path, uint32_t* out_dir_cluster, char* out_leaf);

// Реальные (рантайм) параметры раскладки — используйте их, а не
// FAT32_SECTORS_PER_FAT/FAT32_DATA_START напрямую: они меняются в
// зависимости от реального размера диска (см. fat32_init()).
uint32_t fat32_get_sectors_per_fat();
uint32_t fat32_get_max_cluster();
// Что вернул ata_identify() при последнем форматировании (0, если диск был
// уже отформатирован по старой схеме и его реальный размер не спрашивали,
// либо IDENTIFY не сработал).
uint32_t fat32_get_identified_disk_sectors();

// Переносит запись (файл/каталог) из одной директории в другую (в т.ч. с
// переименованием). src_dir_cluster/dst_dir_cluster обычно получены через
// fat32_resolve_path. Возвращает false, если источник не найден, назначение
// уже существует, или не хватило места на запись в целевой директории.
bool fat32_move_entry(uint32_t src_dir_cluster, const char* src_leaf,
                       uint32_t dst_dir_cluster, const char* dst_leaf);

// ===== УДАЛЕНИЕ =====
void fat32_delete_cluster_chain(uint32_t start_cluster);
void fat32_delete_entry(const char* name, uint8_t target_attr);
int  fat32_tab_complete(const char* prefix, char results[][128], int max);
void fat32_debug_cluster_state(uint32_t cluster);
void fat32_debug_check_chain(uint32_t start_cluster);

// ===== СОЗДАНИЕ =====
bool fat32_create_file(const char* name, const char* content, uint32_t size);
void fat32_create_dir(char* name);
void fat32_rename_entry(char* old_name, char* new_name);

} // extern "C"

#endif // FAT32_H