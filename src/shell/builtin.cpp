#include "builtin.h"
#include "../lib/screen.h"
#include "../kernel/keyboard.h"
#include "../lib/cmd_settings.h"
#include "../drivers/disk.h"
#include "../drivers/fat32.h"
#include "../drivers/atapi.h"
#include "../drivers/iso9660.h"
#include "../lib/string.h"
#include "../lib/utils.h"
#include "../lib/cdrom_path.h"
#include "shell.h"
#include "../commands/cmd_info.h"
#include "../commands/cmd_nano.h"
#include "../commands/cmd_help.h"
#include "../commands/cmd_clear.h"
#include "../commands/cmd_wc.h"
#include "../commands/cmd_banner.h"
#include "../commands/cmd_uptime.h"
#include "../commands/cmd_hexdump.h"
#include "../commands/cmd_head.h"
#include "../commands/cmd_calc.h"
#include "../commands/cmd_panic.h"
#include "../drivers/rtc.h"

static uint8_t cp_buffer[1024*1024]; // 1MB — раньше было 16KB, не хватало даже на небольшой disk.img

int my_atoi(const char* str) {
    int res = 0;
    for (int i = 0; str[i] != '\0'; ++i)
        if (str[i] >= '0' && str[i] <= '9') res = res * 10 + str[i] - '0';
    return res;
}

void cmd_pwd() { println(current_path); }

void cmd_history() {
    if (history_count == 0) { println("History is empty."); return; }
    int start_idx = HISTORY_SIZE - history_count;
    for (int i = 0; i < history_count; i++) {
        // Номер
        int n = i + 1;
        char nbuf[8];
        int ni = 0;
        if (n >= 10) nbuf[ni++] = (char)('0' + n / 10);
        nbuf[ni++] = (char)('0' + n % 10);
        nbuf[ni++] = ' '; nbuf[ni++] = ' '; nbuf[ni] = 0;
        print(nbuf);
        println(history[start_idx + i]);
    }
}

void cmd_fat_check() {
    println("=== FAT32 Table Check ===");
    for (uint32_t c = 0; c < 20; c++) fat32_debug_cluster_state(c);
    println("=== End Check ===");
}

// path_is_cdrom_prefixed / build_iso_path вынесены в lib/cdrom_path.h (общие с cmd_nano.cpp)

void cmd_cd(char* name) {
    if (strlen(name) == 0) { println("Usage: cd <path>"); return; }
    if (strcmp(name, ".") == 0) { println("Directory remains the same."); return; }

    // ── Мы сейчас внутри /cdrom — своя навигация по ISO9660, не по FAT32 ────
    if (path_is_cdrom_prefixed(current_path)) {
        if (strcmp(name, "..") == 0) {
            // dirname одинаково хорошо режет и "/cdrom/SUB"->"/cdrom", и "/cdrom"->"/"
            // (в последнем случае мы тем самым естественно выходим обратно в FAT32)
            dirname(current_path);
            dirname(iso_current_path);
            println("Changed directory.");
            return;
        }
        if (strcmp(name, "/") == 0) {
            strcpy(current_path, "/"); // выход в корень FAT32
            println("Changed directory.");
            return;
        }
        if (name[0] != '/') {
            if (!iso_mounted) { println("Error: CD not mounted"); return; }
            char new_iso_path[ISO_MAX_PATH];
            int p = 0;
            for (int i = 0; iso_current_path[i] && p < ISO_MAX_PATH-2; i++) new_iso_path[p++] = iso_current_path[i];
            if (p == 0 || new_iso_path[p-1] != '/') new_iso_path[p++] = '/';
            for (int i = 0; name[i] && p < ISO_MAX_PATH-1; i++) new_iso_path[p++] = name[i];
            new_iso_path[p] = 0;

            uint32_t dlba, dsize;
            if (!iso_resolve_dir(new_iso_path, &dlba, &dsize)) { println("Error: Directory not found."); return; }
            strcpy(iso_current_path, new_iso_path);
            strcpy(current_path, "/cdrom");
            strcat(current_path, new_iso_path);
            println("Changed directory.");
            return;
        }
        // Абсолютный путь: если это снова "/cdrom..." — обработается веткой входа
        // в CD чуть ниже; если это другой абсолютный FAT32-путь — проваливаемся
        // в обычную логику в самом низу функции.
    }

    // ── Вход в /cdrom (из корня FAT32 коротким именем, либо абсолютным путём откуда угодно) ──
    if (strcmp(name, "/cdrom") == 0 ||
        (strcmp(name, "cdrom") == 0 && strcmp(current_path, "/") == 0)) {
        if (!iso_mounted) { println("Error: CD not mounted"); return; }
        strcpy(iso_current_path, "/");
        strcpy(current_path, "/cdrom");
        println("Changed directory.");
        return;
    }
    if (path_is_cdrom_prefixed(name)) {
        if (!iso_mounted) { println("Error: CD not mounted"); return; }
        const char* rest = name + 6; // после "/cdrom"
        char new_iso_path[ISO_MAX_PATH];
        if (*rest == 0) {
            new_iso_path[0] = '/'; new_iso_path[1] = 0;
        } else {
            int p = 0;
            for (int i = 0; rest[i] && p < ISO_MAX_PATH-1; i++) new_iso_path[p++] = rest[i];
            new_iso_path[p] = 0;
        }
        uint32_t dlba, dsize;
        if (!iso_resolve_dir(new_iso_path, &dlba, &dsize)) { println("Error: Directory not found."); return; }
        strcpy(iso_current_path, new_iso_path);
        strcpy(current_path, "/cdrom");
        strcat(current_path, new_iso_path);
        println("Changed directory.");
        return;
    }

    // ── Обычная FAT32-навигация (без изменений) ─────────────────────────────
    if (strcmp(name, "..") == 0 && current_dir_cluster == FAT32_ROOT_CLUSTER) {
        println("Already in root directory."); return;
    }

    char leaf[128]; uint32_t dir_cluster;
    if (!fat32_resolve_path(name, &dir_cluster, leaf)) { println("Error: Directory not found."); return; }

    uint32_t target_cluster;
    if (leaf[0] == 0) {
        // Путь заканчивался на "/" — сама dir_cluster уже и есть цель
        target_cluster = dir_cluster;
    } else {
        uint32_t saved = current_dir_cluster;
        current_dir_cluster = dir_cluster;
        FAT32_FindResult result = fat32_find_entry(leaf, 0x10);
        current_dir_cluster = saved;
        if (!result.found)                     { println("Error: Directory not found."); return; }
        if (!(result.entry.attributes & 0x10))  { println("Error: This is a file, not a directory!"); return; }
        uint32_t tc = FAT32_GET_CLUSTER(&result.entry);
        target_cluster = (tc < 2) ? FAT32_ROOT_CLUSTER : tc;
    }

    current_dir_cluster = target_cluster;

    // Обновляем отображаемый current_path. Точные случаи ("..", "/", простое
    // относительное имя без "/") ведут себя как раньше. Для настоящих
    // многосегментных путей путь обновляется как есть (косметически может
    // содержать "..", если пользователь его так и написал — на саму навигацию
    // это не влияет, только на то, что покажет `pwd`).
    if (strcmp(name, "..") == 0) {
        dirname(current_path);
    } else if (strcmp(name, "/") == 0) {
        strcpy(current_path, "/");
    } else if (name[0] == '/') {
        // current_path — всего 128 байт; обрезаем, чтобы не переполнить его
        // длинным путём (сама навигация по диску уже произошла и не зависит
        // от этой строки — пострадает только отображение в pwd/статус-баре)
        int k = 0;
        while (name[k] && k < 127) { current_path[k] = name[k]; k++; }
        current_path[k] = 0;
    } else {
        int cur_len = strlen(current_path);
        bool need_slash = (strcmp(current_path, "/") != 0);
        int room = 127 - cur_len - (need_slash ? 1 : 0);
        if (room > 0) {
            if (need_slash) strcat(current_path, "/");
            int k = 0;
            while (name[k] && k < room) { current_path[cur_len + (need_slash?1:0) + k] = name[k]; k++; }
            current_path[cur_len + (need_slash?1:0) + k] = 0;
        }
    }
    println("Changed directory.");
}

void cmd_mkdir(char* name) {
    if (strlen(name) == 0) { println("Usage: mkdir <name>"); return; }
    if (path_is_cdrom_prefixed(current_path) || path_is_cdrom_prefixed(name)) {
        println("Error: CD-ROM is read-only."); return;
    }
    char leaf[128]; uint32_t dir_cluster;
    if (!fat32_resolve_path(name, &dir_cluster, leaf)) { println("Error: Path not found."); return; }
    if (leaf[0] == 0) { println("Error: Invalid name."); return; }

    uint32_t saved = current_dir_cluster;
    current_dir_cluster = dir_cluster;
    if (fat32_find_entry(leaf, 0x00).found) { current_dir_cluster = saved; println("Error: Item already exists."); return; }
    fat32_create_dir(leaf);
    current_dir_cluster = saved;
}

void cmd_rm(char* name) {
    if (strcmp(name,".") == 0 || strcmp(name,"..") == 0 || strcmp(name,"/") == 0) {
        println("Error: Cannot remove special directories."); return;
    }
    if (path_is_cdrom_prefixed(current_path) || path_is_cdrom_prefixed(name)) {
        println("Error: CD-ROM is read-only."); return;
    }
    char leaf[128]; uint32_t dir_cluster;
    if (!fat32_resolve_path(name, &dir_cluster, leaf)) { println("Error: Path not found."); return; }
    if (leaf[0] == 0) { println("Error: Invalid name."); return; }

    uint32_t saved = current_dir_cluster;
    current_dir_cluster = dir_cluster;
    fat32_delete_entry(leaf, 0x00);
    current_dir_cluster = saved;
}

// Если dst_leaf указывает на СУЩЕСТВУЮЩУЮ директорию (внутри dst_dir) —
// перенаправляем ВНУТРЬ неё, подставляя leaf_name как итоговое имя (обычное
// поведение mv/cp в реальных ОС: "mv file /dir" кладёт file внутрь /dir, а не
// переименовывает саму /dir). Если dst_leaf уже пустой ("mv src /dir/" —
// путь оканчивался на "/") — leaf_name используется сразу, без проверки.
// Было продублировано отдельно в cmd_mv и дважды в cmd_cp.
static void resolve_move_destination(const char* leaf_name, uint32_t* dst_dir, char* dst_leaf) {
    if (dst_leaf[0] == 0) {
        int k = 0; while (leaf_name[k]) { dst_leaf[k] = leaf_name[k]; k++; } dst_leaf[k] = 0;
        return;
    }
    uint32_t saved = current_dir_cluster;
    current_dir_cluster = *dst_dir;
    FAT32_FindResult r = fat32_find_entry(dst_leaf, 0x10);
    current_dir_cluster = saved;
    if (r.found && (r.entry.attributes & 0x10)) {
        uint32_t tc = FAT32_GET_CLUSTER(&r.entry);
        *dst_dir = (tc < 2) ? FAT32_ROOT_CLUSTER : tc;
        int k = 0; while (leaf_name[k]) { dst_leaf[k] = leaf_name[k]; k++; } dst_leaf[k] = 0;
    }
}

void cmd_mv(char* args) {
    char old_path[128]={0}, new_path[128]={0};
    int len = strlen(args), sp = 0;
    while(sp < len && args[sp] != ' ') sp++;
    if (sp == len || sp == 0) { println("Usage: mv <old> <new>"); return; }
    for(int i=0;i<sp;i++) old_path[i]=args[i]; old_path[sp]=0;
    int ns=sp+1; for(int i=ns;i<len;i++) new_path[i-ns]=args[i];
    if (strlen(new_path)==0) { println("Usage: mv <old> <new>"); return; }

    if (path_is_cdrom_prefixed(current_path) ||
        path_is_cdrom_prefixed(old_path) || path_is_cdrom_prefixed(new_path)) {
        println("Error: CD-ROM is read-only."); return;
    }

    char src_leaf[128]; uint32_t src_dir;
    if (!fat32_resolve_path(old_path, &src_dir, src_leaf) || src_leaf[0] == 0) {
        println("Error: Source path not found."); return;
    }

    char dst_leaf[128]; uint32_t dst_dir;
    if (!fat32_resolve_path(new_path, &dst_dir, dst_leaf)) {
        println("Error: Destination path not found."); return;
    }

    resolve_move_destination(src_leaf, &dst_dir, dst_leaf);

    if (!fat32_move_entry(src_dir, src_leaf, dst_dir, dst_leaf)) {
        println("Error: Move failed (destination exists or no space).");
        return;
    }
    println("Moved successfully.");
}

void cmd_cp(char* args) {
    char src_path[128]={0}, dst_path[128]={0};
    int len=strlen(args), sp=0;
    while(sp<len && args[sp]!=' ') sp++;
    if (sp==len||sp==0) { println("Usage: cp <source> <dest>"); return; }
    for(int i=0;i<sp;i++) src_path[i]=args[i]; src_path[sp]=0;
    int ns=sp+1; for(int i=ns;i<len;i++) dst_path[i-ns]=args[i];
    if (strlen(dst_path)==0) { println("Usage: cp <source> <dest>"); return; }

    if (path_is_cdrom_prefixed(dst_path)) { println("Error: CD-ROM is read-only."); return; }
    if (path_is_cdrom_prefixed(current_path) || path_is_cdrom_prefixed(src_path)) {
        if (!iso_mounted) { println("Error: CD not mounted"); return; }

        char full_iso_path[ISO_MAX_PATH];
        build_iso_path(src_path, full_iso_path);
        ISO_FindResult r = iso_find_path(full_iso_path);
        if (!r.found) { println("Error: Source file not found."); return; }
        if (r.is_dir) { println("Error: Cannot copy directory."); return; }
        if (r.size > sizeof(cp_buffer)) { println("Error: File too large for buffer. Max: 1MB"); return; }

        uint32_t iso_bytes = iso_read_file(r.lba, r.size, cp_buffer, sizeof(cp_buffer));

        // Имя по умолчанию — последний сегмент ISO-пути (например "/BOOT/DISK.IMG" -> "DISK.IMG")
        char iso_leaf[128]; int leaf_len = 0;
        {
            int last_slash = -1;
            for (int i = 0; full_iso_path[i]; i++) if (full_iso_path[i] == '/') last_slash = i;
            const char* p = full_iso_path + last_slash + 1;
            while (*p && leaf_len < 127) iso_leaf[leaf_len++] = *p++;
            iso_leaf[leaf_len] = 0;
        }

        char dst_leaf[128]; uint32_t dst_dir;
        if (!fat32_resolve_path(dst_path, &dst_dir, dst_leaf)) {
            println("Error: Destination path not found."); return;
        }
        uint32_t saved_cd = current_dir_cluster;
        resolve_move_destination(iso_leaf, &dst_dir, dst_leaf);

        current_dir_cluster = dst_dir;
        bool dst_exists = fat32_find_entry(dst_leaf, 0x00).found;
        if (dst_exists) { current_dir_cluster = saved_cd; println("Error: Destination already exists."); return; }

        println("Copying from CD-ROM...");
        bool ok = fat32_create_file(dst_leaf, (char*)cp_buffer, iso_bytes);
        current_dir_cluster = saved_cd;
        if (ok) println("File copied successfully."); else println("Error: Failed to create destination file.");
        return;
    }

    char src_leaf[128]; uint32_t src_dir;
    if (!fat32_resolve_path(src_path, &src_dir, src_leaf) || src_leaf[0] == 0) {
        println("Error: Source path not found."); return;
    }

    uint32_t saved = current_dir_cluster;
    current_dir_cluster = src_dir;
    FAT32_FindResult res = fat32_find_entry(src_leaf, 0x00);
    current_dir_cluster = saved;
    if (!res.found) { println("Error: Source file not found."); return; }
    if (res.entry.attributes & 0x10) { println("Error: Cannot copy directory."); return; }

    uint32_t size = res.entry.file_size;
    if (size > sizeof(cp_buffer)) { println("Error: File too large for buffer. Max: 1MB"); return; }

    uint32_t cluster = FAT32_GET_CLUSTER(&res.entry);
    uint32_t bytes_read = fat32_read_file_to_buffer(cluster, size, cp_buffer, sizeof(cp_buffer));

    char dst_leaf[128]; uint32_t dst_dir;
    if (!fat32_resolve_path(dst_path, &dst_dir, dst_leaf)) {
        println("Error: Destination path not found."); return;
    }
    resolve_move_destination(src_leaf, &dst_dir, dst_leaf);

    current_dir_cluster = dst_dir;
    bool dst_exists = fat32_find_entry(dst_leaf, 0x00).found;
    if (dst_exists) { current_dir_cluster = saved; println("Error: Destination already exists."); return; }

    println("Copying...");
    bool ok = fat32_create_file(dst_leaf, (char*)cp_buffer, size);
    current_dir_cluster = saved;
    if (ok) println("File copied successfully."); else println("Error: Failed to create destination file.");
}

void cmd_ls_disk() {
    // ── Если мы внутри /cdrom — показываем ISO директорию ──────────────────
    if (path_is_cdrom_prefixed(current_path)) {
        if (!iso_mounted) { println("Error: CD not mounted"); return; }
        uint32_t dlba, dsize;
        if (!iso_resolve_dir(iso_current_path, &dlba, &dsize)) {
            println("Error: Cannot resolve CD directory");
            return;
        }
        println("--- CD-ROM Directory (ISO 9660) ---");
        iso_ls(dlba, dsize);
        return;
    }

    println("--- Disk Directory (FAT32) ---");

    // ── Если мы в корне FAT32 — показываем /cdrom как виртуальную папку ────
    if (strcmp(current_path, "/") == 0 && iso_mounted) {
        uint8_t attr_dir_virt = (theme_bg << 4) | (theme_dir & 0x0F);
        const char* cdrom_entry = "cdrom <DIR>";
        for (int i = 0; cdrom_entry[i]; i++) print_char_colored(cdrom_entry[i], attr_dir_virt);
        print_char('\n');
    }
    uint32_t c = current_dir_cluster;

    uint8_t attr_file = (theme_bg << 4) | (theme_file & 0x0F);
    uint8_t attr_dir  = (theme_bg << 4) | (theme_dir  & 0x0F);

    // Буфер для LFN (до 32 символов)
    char lfn_buf[33];
    int  lfn_len = 0;

    while ((c & FAT32_MASK) >= 2 && (c & FAT32_MASK) < (FAT32_EOC & FAT32_MASK)) {
        uint32_t base_lba = fat32_cluster_to_lba(c);
        for (uint8_t sec = 0; sec < FAT32_SECTORS_PER_CLUSTER; sec++) {
            ata_read_sector(base_lba + sec);
            for (int i = 0; i < FAT32_ENTRIES_PER_SECTOR; i++) {
                FAT32_DirEntry* e = (FAT32_DirEntry*)(sector_buffer + i * FAT32_ENTRY_SIZE);
                if (e->name[0] == 0x00) goto ls_end;
                if ((uint8_t)e->name[0] == 0xE5) { lfn_len = 0; continue; }
                if (e->attributes == 0x08) continue; // volume label

                // LFN запись
                if (e->attributes == 0x0F) {
                    uint8_t* raw = (uint8_t*)e;
                    fat32_lfn_accumulate(raw, lfn_buf, &lfn_len, sizeof(lfn_buf));
                    continue;
                }

                bool is_dir = (e->attributes & 0x10) != 0;
                uint8_t attr = is_dir ? attr_dir : attr_file;

                // Выводим имя: LFN если есть, иначе 8.3
                if (lfn_len > 0) {
                    for (int j = 0; j < lfn_len; j++)
                        print_char_colored(lfn_buf[j], attr);
                    lfn_len = 0;
                } else {
                    // 8.3 имя
                    for (int j = 0; j < 8; j++)
                        if (e->name[j] != ' ') print_char_colored(e->name[j], attr);
                    if (!is_dir) {
                        bool has_ext = false;
                        for (int j = 0; j < 3; j++) if (e->ext[j] != ' ') { has_ext = true; break; }
                        if (has_ext) {
                            print_char_colored('.', attr);
                            for (int j = 0; j < 3; j++)
                                if (e->ext[j] != ' ') print_char_colored(e->ext[j], attr);
                        }
                    }
                }

                // Тег <DIR> или размер
                if (is_dir) {
                    const char* tag = " <DIR>";
                    for (int j = 0; tag[j]; j++) print_char_colored(tag[j], attr);
                } else {
                    uint32_t sz = e->file_size;
                    char szbuf[12]; int si = 0;
                    if (sz == 0) { szbuf[si++] = '0'; }
                    else {
                        char tmp[12]; int ti = 0;
                        while (sz > 0) { tmp[ti++] = (char)('0' + sz % 10); sz /= 10; }
                        for (int j = ti - 1; j >= 0; j--) szbuf[si++] = tmp[j];
                    }
                    szbuf[si++] = 'B'; szbuf[si] = 0;
                    print_char_colored(' ', attr_file);
                    for (int j = 0; j < si; j++) print_char_colored(szbuf[j], attr_file);
                }
                print_char('\n');
            }
        }
        c = fat32_get_next_cluster(c);
    }
ls_end:
    println("--- End of Directory ---");
}

void cmd_disk_cat(char* name) {
    if (path_is_cdrom_prefixed(current_path) || path_is_cdrom_prefixed(name)) {
        if (!iso_mounted) { println("Error: CD not mounted"); return; }
        char full_path[ISO_MAX_PATH];
        build_iso_path(name, full_path);
        ISO_FindResult r = iso_find_path(full_path);
        if (!r.found) { println("Error: File not found."); return; }
        if (r.is_dir) { println("Error: This is a directory, not a file."); return; }
        println("--- Content ---");
        iso_cat(r.lba, r.size);
        println("--- End ---");
        return;
    }

    char leaf[128]; uint32_t dir_cluster;
    if (!fat32_resolve_path(name, &dir_cluster, leaf) || leaf[0] == 0) {
        println("Error: File not found."); return;
    }
    uint32_t saved = current_dir_cluster;
    current_dir_cluster = dir_cluster;
    FAT32_FindResult result = fat32_find_entry(leaf, 0x00);
    current_dir_cluster = saved;

    if (!result.found) { println("Error: File not found."); return; }
    if (result.entry.attributes & 0x10) { println("Error: This is a directory, not a file."); return; }

    uint32_t cluster  = FAT32_GET_CLUSTER(&result.entry);
    uint32_t filesize = result.entry.file_size;
    if (cluster < 2) { println("Error: Invalid cluster."); return; }

    println("--- Content ---");
    uint32_t bytes_read = 0;
    while ((cluster & FAT32_MASK) >= 2 &&
           (cluster & FAT32_MASK) < (FAT32_EOC & FAT32_MASK) &&
           bytes_read < filesize) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        for (int s = 0; s < FAT32_SECTORS_PER_CLUSTER && bytes_read < filesize; s++) {
            ata_read_sector(lba + s);
            for (int b = 0; b < FAT32_BYTES_PER_SECTOR && bytes_read < filesize; b++) {
                print_char(sector_buffer[b]); bytes_read++;
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    print_char('\n');
    println("--- End ---");
}

void cmd_read_disk() {
    println("Reading Sector 0 (Boot Sector)...");
    ata_read_sector(0);
    println("First 32 bytes (Hex):");
    for (int i = 0; i < 32; i++) {
        print_hex_byte(sector_buffer[i]); print_char(' ');
        if ((i+1)%16==0) print_char('\n');
    }
}

void cmd_create(char* name) {
    if (strlen(name) == 0) { println("Usage: create <filename>"); return; }
    if (path_is_cdrom_prefixed(current_path) || path_is_cdrom_prefixed(name)) {
        println("Error: CD-ROM is read-only."); return;
    }
    char leaf[128]; uint32_t dir_cluster;
    if (!fat32_resolve_path(name, &dir_cluster, leaf)) { println("Error: Path not found."); return; }
    if (leaf[0] == 0) { println("Error: Invalid name."); return; }

    uint32_t saved = current_dir_cluster;
    current_dir_cluster = dir_cluster;
    if (fat32_find_entry(leaf, 0x00).found) { current_dir_cluster = saved; println("Error: File already exists."); return; }
    char empty[1] = {0};
    bool ok = fat32_create_file(leaf, empty, 0);
    current_dir_cluster = saved;
    if (ok) { print("File '"); print(name); println("' created successfully."); }
    else println("Error: Failed to create file.");
}

void fat_format_disk() {
    println("FORMAT: Starting FAT32 format");
    uint8_t zero[FAT32_BYTES_PER_SECTOR] = {0};

    // 1. Очистка зарезервированных секторов
    for (uint32_t s = 0; s < FAT32_RESERVED_SECTORS; s++) ata_write_sector(s, zero);
    println("FORMAT: Reserved sectors cleared");

    // 2. Очистка FAT таблиц
    for (uint32_t fat = 0; fat < FAT32_NUMBER_OF_FATS; fat++) {
        uint32_t fat_base = FAT32_FAT_START + fat * FAT32_SECTORS_PER_FAT;
        for (uint32_t s = 0; s < FAT32_SECTORS_PER_FAT; s++) ata_write_sector(fat_base + s, zero);
    }
    println("FORMAT: FAT tables cleared");

    // 3. Служебные записи FAT32
    ata_read_sector(FAT32_FAT_START);
    uint32_t* fat32_tbl = (uint32_t*)sector_buffer;
    fat32_tbl[0] = 0x0FFFFFF8;
    fat32_tbl[1] = FAT32_EOC;
    fat32_tbl[2] = FAT32_EOC;  // Root cluster
    ata_write_sector(FAT32_FAT_START, sector_buffer);
    if (FAT32_NUMBER_OF_FATS > 1)
        ata_write_sector(FAT32_FAT_START + FAT32_SECTORS_PER_FAT, sector_buffer);
    println("FORMAT: FAT reserved clusters written");

    // 4. Очистка кластера корневой директории
    uint32_t root_lba = fat32_cluster_to_lba(FAT32_ROOT_CLUSTER);
    for (uint32_t s = 0; s < FAT32_SECTORS_PER_CLUSTER; s++) ata_write_sector(root_lba + s, zero);
    println("FORMAT: Root directory cleared");

    current_dir_cluster = FAT32_ROOT_CLUSTER;
    println("FORMAT: FAT32 format completed successfully");
}

void cmd_reboot() {
    println("Rebooting system...");
    uint8_t temp;
    do { temp = inb(0x64); if (temp & 1) inb(0x60); } while (temp & 2);
    outb(0x64, 0xFE);
    println("Error: Reboot failed.");
    while(1) asm volatile("hlt");
}

void cmd_shutdown() {
    println("Shutting down...");
    outw(0xB004, 0x2000);
    outw(0x604, 0x2000);
    println("It is now safe to turn off your computer.");
    while(1) { asm volatile("cli"); asm volatile("hlt"); }
}

void cmd_theme(char* arg) {
    if (strlen(arg) == 0) {
        println("Usage: theme <filename.thm> OR <preset>");
        println("Presets: matrix, ocean, amber, red");
        return;
    }

    if (strcmp(arg,"matrix")==0 || strcmp(arg,"ocean")==0 ||
        strcmp(arg,"amber")==0  || strcmp(arg,"red")==0 || strcmp(arg,"default")==0)  {
        set_theme(arg); goto apply_theme;
    }

    {
        FAT32_FindResult res = fat32_find_entry(arg, 0x00);
        if (!res.found) { println("Error: Theme file or preset not found."); return; }
        uint32_t size = res.entry.file_size;
        if (size > FAT32_BYTES_PER_SECTOR) { println("Error: Theme file too large."); return; }

        static uint8_t theme_buf[FAT32_BYTES_PER_SECTOR];
        uint32_t lba = fat32_cluster_to_lba(FAT32_GET_CLUSTER(&res.entry));
        ata_read_sector(lba);
        for(int i=0;i<FAT32_BYTES_PER_SECTOR;i++) theme_buf[i]=sector_buffer[i];
        theme_buf[size]=0;

        uint8_t bg=0, fg=7, bar_bg=7, bar_fg=0, cursor=7,
                cursor_bg=7, cursor_char=0, dir=11, file=7;
        char* ptr=(char*)theme_buf, *line_start=ptr;

        for(int i=0;i<=(int)size;i++) {
            if(ptr[i]=='\n'||ptr[i]==0) {
                ptr[i]=0;
                char* eq=0;
                for(char* p=line_start;*p;p++) if(*p=='=') eq=p;
                if(eq) {
                    *eq=0; char* key=line_start, *val=eq+1;
                    int vlen=strlen(val); if(vlen>0&&val[vlen-1]=='\r') val[vlen-1]=0;
                    int cv=my_atoi(val);
                    if     (strcmp(key,"BG"         )==0) bg          = cv;
                    else if(strcmp(key,"FG"         )==0) fg          = cv;
                    else if(strcmp(key,"BAR_BG"     )==0) bar_bg      = cv;
                    else if(strcmp(key,"BAR_FG"     )==0) bar_fg      = cv;
                    else if(strcmp(key,"CURSOR"     )==0) cursor      = cv;
                    else if(strcmp(key,"CURSOR_BG"  )==0) cursor_bg   = cv;
                    else if(strcmp(key,"CURSOR_CHAR")==0) cursor_char = cv;
                    else if(strcmp(key,"DIR"        )==0) dir         = cv;
                    else if(strcmp(key,"FILE"       )==0) file        = cv;
                }
                line_start=&ptr[i+1];
            }
        }
        set_custom_theme(bg, fg, bar_bg, bar_fg, cursor, cursor_bg, cursor_char, dir, file);
    }

apply_theme:
    uint8_t attr = get_theme_color();
    uint16_t blank = (attr << 8) | ' ';
    for(int i=80;i<80*25;i++) video_memory[i]=blank;
    cmd_clear();
    shell_draw_status_bar();
    println("Custom theme loaded.");
    print("\r");
}