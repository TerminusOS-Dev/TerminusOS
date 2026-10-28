/*
 * tfsutil — host-side tool for Terminus OS disk images (.img / .vhd)
 *
 * Cross-platform build:
 *   Linux / WSL (Native):  g++ -std=c++17 -O2 -o tfsutil tfsutil.cpp
 *   MinGW (for Windows):   x86_64-w64-mingw32-g++ -std=c++17 -O2 -o tfsutil.exe tfsutil.cpp
 *   MSVC (Windows):        cl /EHsc /std:c++17 /O2 tfsutil.cpp /Fe:tfsutil.exe
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cerrno>

// ── Cross-platform headers & directory handling ──────────────────────────────
#ifdef _WIN32
    #include <direct.h>
    #include <io.h>
    #define make_dir(path) _mkdir(path)
#else
    #include <sys/stat.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define make_dir(path) mkdir(path, 0755)
#endif

// ── constants matching fat32.h ───────────────────────────────────────────────
static constexpr uint32_t TFS_MAGIC              = 0x31534654u; // "TFS1" LE
static constexpr uint32_t RESERVED_SECTORS       = 32;
static constexpr uint32_t NUMBER_OF_FATS         = 2;
static constexpr uint32_t SECTORS_PER_CLUSTER    = 8;
static constexpr uint32_t BYTES_PER_SECTOR       = 512;
static constexpr uint32_t CLUSTER_BYTES          = SECTORS_PER_CLUSTER * BYTES_PER_SECTOR; // 4096
static constexpr uint32_t ENTRIES_PER_FAT_SECTOR = 128;
static constexpr uint32_t ENTRY_SIZE             = 32;
static constexpr uint32_t ENTRIES_PER_SECTOR     = 16;
static constexpr uint32_t FAT_START              = RESERVED_SECTORS; // 32
static constexpr uint32_t ROOT_CLUSTER           = 2;
static constexpr uint32_t FAT32_EOC              = 0x0FFFFFFF;
static constexpr uint32_t FAT32_FREE             = 0x00000000;
static constexpr uint32_t FAT32_MASK             = 0x0FFFFFFF;
static constexpr uint32_t DEFAULT_SPF            = 512; // old fixed layout

#pragma pack(push, 1)
struct TerminusFsHeader {
    uint32_t magic;
    uint32_t sectors_per_fat;
    uint32_t total_disk_sectors;
    uint32_t reserved[125];
};
static_assert(sizeof(TerminusFsHeader) == 512, "header must be 1 sector");

struct DirEntry {
    char     name[8];
    char     ext[3];
    uint8_t  attributes;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_lo;
    uint32_t file_size;
};
static_assert(sizeof(DirEntry) == 32, "dir entry must be 32 bytes");
#pragma pack(pop)

static inline uint32_t get_cluster(const DirEntry& e) {
    return ((uint32_t)e.cluster_hi << 16) | e.cluster_lo;
}
static inline void set_cluster(DirEntry& e, uint32_t c) {
    e.cluster_hi = (uint16_t)((c >> 16) & 0xFFFF);
    e.cluster_lo = (uint16_t)(c & 0xFFFF);
}

// ── Image ────────────────────────────────────────────────────────────────────
struct Image {
    FILE*    f = nullptr;
    uint64_t size_bytes = 0;
    uint32_t total_sectors = 0;
    uint32_t sectors_per_fat = DEFAULT_SPF;
    uint32_t data_start = 0;
    uint32_t max_cluster = 0;
    bool     has_tfs_header = false;

    ~Image() { if (f) fclose(f); }

    bool open(const char* path, bool writable) {
        f = fopen(path, writable ? "r+b" : "rb");
        if (!f) {
            fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
            return false;
        }
        fseek(f, 0, SEEK_END);
        size_bytes = (uint64_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        total_sectors = (uint32_t)(size_bytes / BYTES_PER_SECTOR);
        return load_header();
    }

    bool load_header() {
        TerminusFsHeader hdr{};
        if (!read_sector(0, (uint8_t*)&hdr)) return false;

        if (hdr.magic == TFS_MAGIC && hdr.sectors_per_fat >= 1) {
            has_tfs_header = true;
            sectors_per_fat = hdr.sectors_per_fat;
            if (hdr.total_disk_sectors)
                total_sectors = hdr.total_disk_sectors;
        } else {
            // old scheme without header — fixed 512 SPF
            has_tfs_header = false;
            sectors_per_fat = DEFAULT_SPF;
        }
        data_start  = RESERVED_SECTORS + NUMBER_OF_FATS * sectors_per_fat;
        max_cluster = sectors_per_fat * ENTRIES_PER_FAT_SECTOR;
        return true;
    }

    bool read_sector(uint32_t lba, uint8_t* buf) {
        if ((uint64_t)lba * BYTES_PER_SECTOR + BYTES_PER_SECTOR > size_bytes) {
            memset(buf, 0, BYTES_PER_SECTOR);
            return false;
        }
        fseek(f, (long)lba * BYTES_PER_SECTOR, SEEK_SET);
        return fread(buf, 1, BYTES_PER_SECTOR, f) == BYTES_PER_SECTOR;
    }

    bool write_sector(uint32_t lba, const uint8_t* buf) {
        if ((uint64_t)lba * BYTES_PER_SECTOR + BYTES_PER_SECTOR > size_bytes) {
            fprintf(stderr, "write beyond image size (LBA %u)\n", lba);
            return false;
        }
        fseek(f, (long)lba * BYTES_PER_SECTOR, SEEK_SET);
        return fwrite(buf, 1, BYTES_PER_SECTOR, f) == BYTES_PER_SECTOR;
    }

    uint32_t cluster_to_lba(uint32_t cluster) const {
        if (cluster < 2) return data_start;
        return data_start + (cluster - 2) * SECTORS_PER_CLUSTER;
    }

    uint32_t get_fat_entry(uint32_t cluster) {
        uint32_t fat_lba = FAT_START + (cluster / ENTRIES_PER_FAT_SECTOR);
        uint32_t idx     = cluster % ENTRIES_PER_FAT_SECTOR;
        uint8_t  sec[512];
        if (!read_sector(fat_lba, sec)) return FAT32_EOC;
        uint32_t* fat = (uint32_t*)sec;
        return fat[idx] & FAT32_MASK;
    }

    void set_fat_entry(uint32_t cluster, uint32_t value) {
        uint32_t fat_lba = FAT_START + (cluster / ENTRIES_PER_FAT_SECTOR);
        uint32_t idx     = cluster % ENTRIES_PER_FAT_SECTOR;
        uint8_t  sec[512];
        read_sector(fat_lba, sec);
        uint32_t* fat = (uint32_t*)sec;
        fat[idx] = (fat[idx] & 0xF0000000) | (value & FAT32_MASK);
        write_sector(fat_lba, sec);
        // mirror
        if (NUMBER_OF_FATS > 1)
            write_sector(fat_lba + sectors_per_fat, sec);
    }

    uint32_t find_free_cluster() {
        for (uint32_t c = 3; c < max_cluster; c++) {
            if (get_fat_entry(c) == FAT32_FREE)
                return c;
        }
        return 0;
    }

    // read whole file content into vector
    std::vector<uint8_t> read_file_data(uint32_t start_cluster, uint32_t file_size) {
        std::vector<uint8_t> out;
        out.reserve(file_size);
        uint32_t cluster = start_cluster;
        uint32_t got = 0;
        while ((cluster & FAT32_MASK) >= 2 &&
               (cluster & FAT32_MASK) < (FAT32_EOC & FAT32_MASK) &&
               got < file_size) {
            uint32_t lba = cluster_to_lba(cluster);
            for (uint32_t s = 0; s < SECTORS_PER_CLUSTER && got < file_size; s++) {
                uint8_t sec[512];
                read_sector(lba + s, sec);
                uint32_t n = std::min<uint32_t>(BYTES_PER_SECTOR, file_size - got);
                out.insert(out.end(), sec, sec + n);
                got += n;
            }
            cluster = get_fat_entry(cluster);
        }
        return out;
    }

    void write_cluster_data(uint32_t cluster, const uint8_t* data, uint32_t size) {
        uint32_t lba = cluster_to_lba(cluster);
        uint32_t written = 0;
        for (uint32_t s = 0; s < SECTORS_PER_CLUSTER; s++) {
            uint8_t sec[512];
            memset(sec, 0, 512);
            if (written < size) {
                uint32_t n = std::min<uint32_t>(512, size - written);
                memcpy(sec, data + written, n);
                written += n;
            }
            write_sector(lba + s, sec);
        }
    }

    void delete_cluster_chain(uint32_t start) {
        uint32_t c = start;
        while ((c & FAT32_MASK) >= 2 && (c & FAT32_MASK) < (FAT32_EOC & FAT32_MASK)) {
            uint32_t next = get_fat_entry(c);
            set_fat_entry(c, FAT32_FREE);
            c = next;
        }
    }
};

// ── LFN helpers ──────────────────────────────────────────────────────────────
static void lfn_accumulate(const uint8_t* raw, char* buf, int* len, int bufsize) {
    char chunk[14]; int ci = 0;
    auto take = [&](int off) {
        if (raw[off] != 0xFF && raw[off] != 0x00) chunk[ci++] = (char)raw[off];
        else if (raw[off] == 0x00) chunk[ci++] = 0;
    };
    for (int k = 1; k <= 9; k += 2) take(k);
    for (int k = 14; k <= 24; k += 2) take(k);
    for (int k = 28; k <= 30; k += 2) take(k);
    int chlen = 0;
    while (chlen < 13 && chunk[chlen]) chlen++;
    if (*len + chlen < bufsize - 1) {
        for (int k = *len - 1; k >= 0; k--) buf[k + chlen] = buf[k];
        for (int k = 0; k < chlen; k++) buf[k] = chunk[k];
        *len += chlen;
    }
    buf[*len] = 0;
}

static std::string decode_83(const DirEntry& e) {
    std::string s;
    for (int i = 0; i < 8 && e.name[i] != ' '; i++)
        s += (char)(e.name[i] | 0x20); // lower
    bool has_ext = false;
    for (int i = 0; i < 3; i++) if (e.ext[i] != ' ') { has_ext = true; break; }
    if (has_ext) {
        s += '.';
        for (int i = 0; i < 3 && e.ext[i] != ' '; i++)
            s += (char)(e.ext[i] | 0x20);
    }
    return s;
}

static void format_83(const char* input, char out[11]) {
    memset(out, ' ', 11);
    int si = 0, di = 0;
    while (input[si] && input[si] != '.' && di < 8) {
        char c = input[si++];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[di++] = c;
    }
    while (input[si] && input[si] != '.') si++;
    if (input[si] == '.') si++;
    di = 8;
    while (input[si] && di < 11) {
        char c = input[si++];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[di++] = c;
    }
}

static bool str_ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return !*a && !*b;
}

static bool needs_lfn(const char* name) {
    int len = 0;
    bool has_dot = false;
    int name_part = 0, ext_part = 0;
    for (const char* p = name; *p; p++) {
        len++;
        char c = *p;
        if (c == '.') {
            if (has_dot) return true;
            has_dot = true;
            continue;
        }
        if (c >= 'a' && c <= 'z') return true; // mixed case → LFN
        if (!( (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '~' || c == '!' || c == '#' ||
               c == '$' || c == '%' || c == '&' || c == '\'' || c == '(' ||
               c == ')' || c == '@' || c == '^' || c == '`' || c == '{' ||
               c == '}' || c == '~' ))
            return true;
        if (!has_dot) name_part++; else ext_part++;
    }
    if (name_part > 8 || ext_part > 3 || len > 12) return true;
    return false;
}

static uint8_t lfn_checksum(const char name11[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name11[i];
    return sum;
}

static void write_lfn_chunk(uint8_t* raw, int ord, bool last, const char* name,
                            int char_start, uint8_t checksum) {
    memset(raw, 0, 32);
    raw[0]  = (uint8_t)(ord | (last ? 0x40 : 0));
    raw[11] = 0x0F;
    raw[13] = checksum;
    int name_len = (int)strlen(name);
    static const int pos[13] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    bool ended = false;
    for (int k = 0; k < 13; k++) {
        int ch_idx = char_start + k;
        int off = pos[k];
        if (!ended && ch_idx < name_len) {
            raw[off] = (uint8_t)name[ch_idx];
            raw[off+1] = 0;
        } else if (!ended) {
            raw[off] = 0; raw[off+1] = 0;
            ended = true;
        } else {
            raw[off] = 0xFF; raw[off+1] = 0xFF;
        }
    }
}

// ── Directory walk ───────────────────────────────────────────────────────────
struct FoundEntry {
    bool      found = false;
    DirEntry  entry{};
    uint32_t  lba = 0;          // LBA of the short (8.3) entry
    int       index = 0;        // index within that sector
    std::string long_name;
    std::vector<std::pair<uint32_t, int>> all_slots;
};

static FoundEntry find_in_dir(Image& img, uint32_t dir_cluster, const char* name) {
    FoundEntry res;
    char name83[11];
    format_83(name, name83);
    char lfn_buf[256]; int lfn_len = 0;
    std::vector<std::pair<uint32_t, int>> pending_lfns;
    uint32_t c = dir_cluster;

    while ((c & FAT32_MASK) >= 2 && (c & FAT32_MASK) < 0x0FFFFFF8) {
        uint32_t base = img.cluster_to_lba(c);
        for (uint32_t sec = 0; sec < SECTORS_PER_CLUSTER; sec++) {
            uint8_t buf[512];
            img.read_sector(base + sec, buf);
            for (int i = 0; i < ENTRIES_PER_SECTOR; i++) {
                DirEntry* e = (DirEntry*)(buf + i * ENTRY_SIZE);
                if (e->name[0] == 0x00) return res; // end
                if ((uint8_t)e->name[0] == 0xE5) {
                    lfn_len = 0;
                    pending_lfns.clear();
                    continue;
                }
                if (e->attributes == 0x0F) {
                    lfn_accumulate((uint8_t*)e, lfn_buf, &lfn_len, sizeof(lfn_buf));
                    pending_lfns.emplace_back(base + sec, i);
                    continue;
                }
                bool matched = false;
                if (lfn_len > 0 && str_ieq(lfn_buf, name)) matched = true;
                if (!matched && memcmp(e->name, name83, 11) == 0) matched = true;
                if (matched) {
                    res.found = true;
                    res.entry = *e;
                    res.lba = base + sec;
                    res.index = i;
                    res.long_name = lfn_len > 0 ? std::string(lfn_buf) : decode_83(*e);
                    res.all_slots = std::move(pending_lfns);
                    res.all_slots.emplace_back(base + sec, i);
                    return res;
                }
                lfn_len = 0;
                pending_lfns.clear();
            }
        }
        uint32_t next = img.get_fat_entry(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) break;
        c = next;
    }
    return res;
}

static void delete_dir_entry(Image& img, const FoundEntry& fe) {
    if (!fe.found) return;
    for (const auto& slot : fe.all_slots) {
        uint8_t sec[512];
        if (!img.read_sector(slot.first, sec)) continue;
        sec[slot.second * ENTRY_SIZE] = 0xE5;
        img.write_sector(slot.first, sec);
    }
}

struct ListItem {
    std::string name;
    bool is_dir = false;
    uint32_t size = 0;
    uint32_t cluster = 0;
};

static std::vector<ListItem> list_dir(Image& img, uint32_t dir_cluster) {
    std::vector<ListItem> items;
    char lfn_buf[256]; int lfn_len = 0;
    uint32_t c = dir_cluster;

    while ((c & FAT32_MASK) >= 2 && (c & FAT32_MASK) < 0x0FFFFFF8) {
        uint32_t base = img.cluster_to_lba(c);
        for (uint32_t sec = 0; sec < SECTORS_PER_CLUSTER; sec++) {
            uint8_t buf[512];
            img.read_sector(base + sec, buf);
            for (int i = 0; i < ENTRIES_PER_SECTOR; i++) {
                DirEntry* e = (DirEntry*)(buf + i * ENTRY_SIZE);
                if (e->name[0] == 0x00) return items;
                if ((uint8_t)e->name[0] == 0xE5) { lfn_len = 0; continue; }
                if (e->attributes == 0x0F) {
                    lfn_accumulate((uint8_t*)e, lfn_buf, &lfn_len, sizeof(lfn_buf));
                    continue;
                }
                if (e->name[0] == '.') { lfn_len = 0; continue; }
                ListItem it;
                it.name = lfn_len > 0 ? std::string(lfn_buf) : decode_83(*e);
                it.is_dir = (e->attributes & 0x10) != 0;
                it.size = e->file_size;
                it.cluster = get_cluster(*e);
                items.push_back(it);
                lfn_len = 0;
            }
        }
        uint32_t next = img.get_fat_entry(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) break;
        c = next;
    }
    return items;
}

static bool resolve_path(Image& img, const char* path,
                         uint32_t* out_dir, std::string* out_leaf) {
    if (!path || !*path) return false;
    uint32_t cluster;
    int i = 0;
    if (path[0] == '/') { cluster = ROOT_CLUSTER; i = 1; }
    else cluster = ROOT_CLUSTER;

    while (path[i] == '/') i++;

    while (true) {
        char seg[128]; int si = 0;
        while (path[i] && path[i] != '/' && si < 127) seg[si++] = path[i++];
        seg[si] = 0;
        while (path[i] == '/') i++;

        if (!path[i]) {
            *out_leaf = seg;
            *out_dir = cluster;
            return true;
        }
        if (si == 0) continue;

        FoundEntry fe = find_in_dir(img, cluster, seg);
        if (!fe.found || !(fe.entry.attributes & 0x10)) return false;
        uint32_t next = get_cluster(fe.entry);
        cluster = (next < 2) ? ROOT_CLUSTER : next;
    }
}

static bool alloc_dir_slots(Image& img, uint32_t dir_cluster, int count,
                            uint32_t* lba_arr, int* idx_arr) {
    if (count > 17) return false;
    uint32_t win_lba[17]; int win_idx[17]; int win_n = 0;
    uint32_t c = dir_cluster;

    while (true) {
        uint32_t base = img.cluster_to_lba(c);
        for (uint32_t sec = 0; sec < SECTORS_PER_CLUSTER; sec++) {
            uint8_t buf[512];
            img.read_sector(base + sec, buf);
            for (int i = 0; i < ENTRIES_PER_SECTOR; i++) {
                DirEntry* e = (DirEntry*)(buf + i * ENTRY_SIZE);
                bool free = (e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5);
                if (free) {
                    win_lba[win_n] = base + sec;
                    win_idx[win_n] = i;
                    win_n++;
                    if (win_n == count) {
                        for (int k = 0; k < count; k++) {
                            lba_arr[k] = win_lba[k];
                            idx_arr[k] = win_idx[k];
                        }
                        return true;
                    }
                } else {
                    win_n = 0;
                }
            }
        }
        uint32_t next = img.get_fat_entry(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) {
            uint32_t nc = img.find_free_cluster();
            if (!nc) return false;
            img.set_fat_entry(c, nc);
            img.set_fat_entry(nc, FAT32_EOC);
            uint8_t empty[512] = {0};
            uint32_t nlba = img.cluster_to_lba(nc);
            for (uint32_t s = 0; s < SECTORS_PER_CLUSTER; s++)
                img.write_sector(nlba + s, empty);
            c = nc;
            continue;
        }
        c = next;
    }
}

static void collect_short_names(Image& img, uint32_t dir_cluster,
                                std::vector<std::string>& out) {
    uint32_t c = dir_cluster;
    while ((c & FAT32_MASK) >= 2 && (c & FAT32_MASK) < 0x0FFFFFF8) {
        uint32_t base = img.cluster_to_lba(c);
        for (uint32_t sec = 0; sec < SECTORS_PER_CLUSTER; sec++) {
            uint8_t buf[512];
            img.read_sector(base + sec, buf);
            for (int i = 0; i < ENTRIES_PER_SECTOR; i++) {
                DirEntry* e = (DirEntry*)(buf + i * ENTRY_SIZE);
                if (e->name[0] == 0x00) return;
                if ((uint8_t)e->name[0] == 0xE5) continue;
                if (e->attributes == 0x0F) continue;
                if (e->name[0] == '.') continue;
                out.emplace_back(e->name, 11);
            }
        }
        uint32_t next = img.get_fat_entry(c);
        if ((next & FAT32_MASK) >= (FAT32_EOC & FAT32_MASK)) break;
        c = next;
    }
}

static void make_unique_short_name(Image& img, uint32_t dir_cluster,
                                   const char* name, bool force_tilde,
                                   char out[11]) {
    format_83(name, out);
    if (!force_tilde) {
        std::vector<std::string> used;
        collect_short_names(img, dir_cluster, used);
        bool clash = false;
        for (const auto& u : used)
            if (memcmp(u.data(), out, 11) == 0) { clash = true; break; }
        if (!clash) return;
        force_tilde = true;
    }

    char base[8];
    memcpy(base, out, 8);
    char ext[3];
    memcpy(ext, out + 8, 3);

    std::vector<std::string> used;
    collect_short_names(img, dir_cluster, used);

    for (int n = 1; n <= 999999; n++) {
        char candidate[11];
        memset(candidate, ' ', 11);
        int digits = 1;
        int tmp = n;
        while (tmp >= 10) { digits++; tmp /= 10; }
        int name_len = 8 - 1 - digits;
        if (name_len < 1) name_len = 1;
        for (int i = 0; i < name_len && base[i] != ' '; i++)
            candidate[i] = base[i];
        candidate[name_len] = '~';
        char dig[8];
        snprintf(dig, sizeof(dig), "%d", n);
        for (int i = 0; dig[i]; i++)
            candidate[name_len + 1 + i] = dig[i];
        memcpy(candidate + 8, ext, 3);

        bool clash = false;
        for (const auto& u : used)
            if (memcmp(u.data(), candidate, 11) == 0) { clash = true; break; }
        if (!clash) {
            memcpy(out, candidate, 11);
            return;
        }
    }
    out[6] = '~'; out[7] = '1';
}

static bool write_named_entry(Image& img, uint32_t dir_cluster,
                              const char* name, uint8_t attr,
                              uint32_t cluster, uint32_t file_size) {
    bool need = needs_lfn(name);
    char short_name[11];
    int n_lfn = 0;
    int name_len = (int)strlen(name);

    make_unique_short_name(img, dir_cluster, name, need, short_name);

    if (need) {
        n_lfn = (name_len + 12) / 13;
        if (n_lfn < 1) n_lfn = 1;
        if (n_lfn > 16) n_lfn = 16;
    }

    int total = n_lfn + 1;
    uint32_t slot_lba[17]; int slot_idx[17];
    if (!alloc_dir_slots(img, dir_cluster, total, slot_lba, slot_idx))
        return false;

    uint8_t checksum = lfn_checksum(short_name);
    for (int p = 0; p < n_lfn; p++) {
        int ord = n_lfn - p;
        bool last = (ord == n_lfn);
        int char_start = (ord - 1) * 13;
        uint8_t sec[512];
        img.read_sector(slot_lba[p], sec);
        write_lfn_chunk(sec + slot_idx[p] * 32, ord, last, name, char_start, checksum);
        img.write_sector(slot_lba[p], sec);
    }

    uint8_t sec[512];
    img.read_sector(slot_lba[n_lfn], sec);
    DirEntry* e = (DirEntry*)(sec + slot_idx[n_lfn] * 32);
    memset(e, 0, 32);
    memcpy(e->name, short_name, 8);
    memcpy(e->ext, short_name + 8, 3);
    e->attributes = attr;
    e->file_size = file_size;
    set_cluster(*e, cluster);
    img.write_sector(slot_lba[n_lfn], sec);
    return true;
}

// ── commands ─────────────────────────────────────────────────────────────────
static int cmd_info(Image& img) {
    printf("Image size      : %llu bytes (%u sectors)\n",
           (unsigned long long)img.size_bytes, img.total_sectors);
    printf("TFS header      : %s\n", img.has_tfs_header ? "yes" : "no (old fixed layout)");
    printf("sectors_per_fat : %u\n", img.sectors_per_fat);
    printf("data_start LBA  : %u\n", img.data_start);
    printf("max_cluster     : %u  (~%.1f MiB data)\n",
           img.max_cluster,
           (img.max_cluster - 2) * CLUSTER_BYTES / (1024.0 * 1024.0));
    printf("root cluster    : %u\n", ROOT_CLUSTER);

    uint32_t free_c = 0, used_c = 0;
    uint32_t limit = std::min(img.max_cluster, 4096u);
    for (uint32_t c = 2; c < limit; c++) {
        uint32_t v = img.get_fat_entry(c);
        if (v == FAT32_FREE) free_c++;
        else used_c++;
    }
    printf("clusters 2..%u  : free=%u used=%u (sampled)\n", limit - 1, free_c, used_c);
    return 0;
}

static int cmd_ls(Image& img, const char* path) {
    uint32_t dir = ROOT_CLUSTER;
    if (path && path[0] && !(path[0] == '/' && path[1] == 0)) {
        uint32_t parent; std::string leaf;
        if (!resolve_path(img, path, &parent, &leaf)) {
            fprintf(stderr, "path not found: %s\n", path);
            return 1;
        }
        if (leaf.empty()) {
            dir = parent;
        } else {
            FoundEntry fe = find_in_dir(img, parent, leaf.c_str());
            if (!fe.found) {
                fprintf(stderr, "not found: %s\n", path);
                return 1;
            }
            if (!(fe.entry.attributes & 0x10)) {
                printf("%-40s %10u  %s\n",
                       fe.long_name.c_str(), fe.entry.file_size, "FILE");
                return 0;
            }
            dir = get_cluster(fe.entry);
            if (dir < 2) dir = ROOT_CLUSTER;
        }
    }

    auto items = list_dir(img, dir);
    std::sort(items.begin(), items.end(),
              [](const ListItem& a, const ListItem& b) {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  return a.name < b.name;
              });
    for (auto& it : items) {
        printf("%-40s %10u  %s\n",
               it.name.c_str(), it.size, it.is_dir ? "DIR" : "FILE");
    }
    printf("(%zu entries)\n", items.size());
    return 0;
}

static int cmd_cat(Image& img, const char* path) {
    uint32_t parent; std::string leaf;
    if (!resolve_path(img, path, &parent, &leaf) || leaf.empty()) {
        fprintf(stderr, "bad path\n");
        return 1;
    }
    FoundEntry fe = find_in_dir(img, parent, leaf.c_str());
    if (!fe.found) {
        fprintf(stderr, "not found: %s\n", path);
        return 1;
    }
    if (fe.entry.attributes & 0x10) {
        fprintf(stderr, "is a directory\n");
        return 1;
    }
    auto data = img.read_file_data(get_cluster(fe.entry), fe.entry.file_size);
    fwrite(data.data(), 1, data.size(), stdout);
    return 0;
}

static int extract_one(Image& img, uint32_t dir_cluster, const std::string& name,
                       const std::string& outdir) {
    FoundEntry fe = find_in_dir(img, dir_cluster, name.c_str());
    if (!fe.found) return 1;

    std::string outpath = outdir + "/" + (fe.long_name.empty() ? name : fe.long_name);

    if (fe.entry.attributes & 0x10) {
        make_dir(outpath.c_str()); // Кроссплатформенный вызов
        auto children = list_dir(img, get_cluster(fe.entry));
        for (auto& ch : children)
            extract_one(img, get_cluster(fe.entry), ch.name, outpath);
    } else {
        auto data = img.read_file_data(get_cluster(fe.entry), fe.entry.file_size);
        FILE* out = fopen(outpath.c_str(), "wb");
        if (!out) {
            fprintf(stderr, "cannot create %s\n", outpath.c_str());
            return 1;
        }
        fwrite(data.data(), 1, data.size(), out);
        fclose(out);
        printf("extracted %s (%u bytes)\n", outpath.c_str(), fe.entry.file_size);
    }
    return 0;
}

static int cmd_extract(Image& img, const char* path, const char* outdir) {
    make_dir(outdir); // Кроссплатформенный вызов
    if (!path || (path[0] == '/' && path[1] == 0)) {
        auto items = list_dir(img, ROOT_CLUSTER);
        for (auto& it : items)
            extract_one(img, ROOT_CLUSTER, it.name, outdir);
        return 0;
    }
    uint32_t parent; std::string leaf;
    if (!resolve_path(img, path, &parent, &leaf)) {
        fprintf(stderr, "path not found\n");
        return 1;
    }
    return extract_one(img, parent, leaf, outdir);
}

static uint32_t compute_spf(uint32_t total_sectors) {
    if (total_sectors <= RESERVED_SECTORS) return DEFAULT_SPF;
    uint32_t usable = total_sectors - RESERVED_SECTORS;
    uint32_t approx_clusters = usable / SECTORS_PER_CLUSTER;
    uint32_t spf = (approx_clusters + ENTRIES_PER_FAT_SECTOR - 1) / ENTRIES_PER_FAT_SECTOR;
    if (spf < 1) spf = 1;
    if (spf > 65536) spf = 65536;
    return spf;
}

static int cmd_format(const char* path, uint32_t size_mb) {
    uint64_t bytes = (uint64_t)size_mb * 1024 * 1024;
    uint32_t total_sectors = (uint32_t)(bytes / BYTES_PER_SECTOR);

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot create %s\n", path);
        return 1;
    }
    if (fseek(f, (long)bytes - 1, SEEK_SET) != 0) {
        fprintf(stderr, "seek failed\n");
        fclose(f);
        return 1;
    }
    fputc(0, f);
    fclose(f);

    Image img;
    if (!img.open(path, true)) return 1;

    uint32_t spf = compute_spf(total_sectors);
    img.sectors_per_fat = spf;
    img.data_start = RESERVED_SECTORS + NUMBER_OF_FATS * spf;
    img.max_cluster = spf * ENTRIES_PER_FAT_SECTOR;
    img.total_sectors = total_sectors;

    uint8_t zero[512] = {0};
    for (uint32_t lba = 0; lba < img.data_start + SECTORS_PER_CLUSTER * 2; lba++)
        img.write_sector(lba, zero);

    TerminusFsHeader hdr{};
    hdr.magic = TFS_MAGIC;
    hdr.sectors_per_fat = spf;
    hdr.total_disk_sectors = total_sectors;
    img.write_sector(0, (uint8_t*)&hdr);

    uint8_t fatsec[512] = {0};
    uint32_t* fat = (uint32_t*)fatsec;
    fat[0] = 0x0FFFFFF8;
    fat[1] = FAT32_EOC;
    fat[2] = FAT32_EOC;
    img.write_sector(FAT_START, fatsec);
    if (NUMBER_OF_FATS > 1)
        img.write_sector(FAT_START + spf, fatsec);

    printf("formatted %s: %u MiB, spf=%u, data_start=%u, max_cluster=%u\n",
           path, size_mb, spf, img.data_start, img.max_cluster);
    return 0;
}

static int cmd_add(Image& img, const char* hostfile, const char* path) {
    FILE* in = fopen(hostfile, "rb");
    if (!in) {
        fprintf(stderr, "cannot open %s\n", hostfile);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    long fsz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (fsz < 0) { fclose(in); return 1; }
    std::vector<uint8_t> data((size_t)fsz);
    if (fsz > 0) {
        size_t n = fread(data.data(), 1, (size_t)fsz, in);
        if (n != (size_t)fsz) {
            fprintf(stderr, "short read of %s\n", hostfile);
            fclose(in);
            return 1;
        }
    }
    fclose(in);

    uint32_t parent; std::string leaf;
    if (!resolve_path(img, path, &parent, &leaf) || leaf.empty()) {
        fprintf(stderr, "bad path\n");
        return 1;
    }

    FoundEntry old = find_in_dir(img, parent, leaf.c_str());
    if (old.found) {
        if (old.entry.attributes & 0x10) {
            fprintf(stderr, "target is a directory\n");
            return 1;
        }
        img.delete_cluster_chain(get_cluster(old.entry));
        delete_dir_entry(img, old);
    }

    uint32_t num_clusters = ((uint32_t)fsz + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
    if (num_clusters == 0) num_clusters = 1;
    if (num_clusters > 4096) {
        fprintf(stderr, "file too large for this tool\n");
        return 1;
    }

    std::vector<uint32_t> clusters(num_clusters);
    for (uint32_t i = 0; i < num_clusters; i++) {
        clusters[i] = img.find_free_cluster();
        if (!clusters[i]) {
            fprintf(stderr, "no free clusters\n");
            return 1;
        }
        img.set_fat_entry(clusters[i], FAT32_EOC);
    }
    for (uint32_t i = 0; i + 1 < num_clusters; i++)
        img.set_fat_entry(clusters[i], clusters[i + 1]);
    img.set_fat_entry(clusters.back(), FAT32_EOC);

    uint32_t remaining = (uint32_t)fsz;
    uint32_t written = 0;
    for (uint32_t i = 0; i < num_clusters && remaining; i++) {
        uint32_t to_write = std::min(remaining, CLUSTER_BYTES);
        img.write_cluster_data(clusters[i], data.data() + written, to_write);
        written += to_write;
        remaining -= to_write;
    }

    if (!write_named_entry(img, parent, leaf.c_str(), 0x20, clusters[0], (uint32_t)fsz)) {
        fprintf(stderr, "no free directory slot\n");
        for (auto c : clusters) img.set_fat_entry(c, FAT32_FREE);
        return 1;
    }
    printf("added %s → %s (%ld bytes)\n", hostfile, path, fsz);
    return 0;
}

static int cmd_mkdir(Image& img, const char* path) {
    uint32_t parent; std::string leaf;
    if (!resolve_path(img, path, &parent, &leaf) || leaf.empty()) {
        fprintf(stderr, "bad path\n");
        return 1;
    }
    if (find_in_dir(img, parent, leaf.c_str()).found) {
        fprintf(stderr, "already exists\n");
        return 1;
    }

    uint32_t nc = img.find_free_cluster();
    if (!nc) {
        fprintf(stderr, "no free clusters\n");
        return 1;
    }
    img.set_fat_entry(nc, FAT32_EOC);

    uint8_t dirbuf[512] = {0};
    DirEntry* dot = (DirEntry*)dirbuf;
    DirEntry* dotdot = (DirEntry*)(dirbuf + 32);
    memset(dot->name, ' ', 11);
    memset(dotdot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attributes = 0x10;
    set_cluster(*dot, nc);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attributes = 0x10;
    set_cluster(*dotdot, parent);

    uint32_t lba = img.cluster_to_lba(nc);
    img.write_sector(lba, dirbuf);
    uint8_t empty[512] = {0};
    for (uint32_t s = 1; s < SECTORS_PER_CLUSTER; s++)
        img.write_sector(lba + s, empty);

    if (!write_named_entry(img, parent, leaf.c_str(), 0x10, nc, 0)) {
        img.delete_cluster_chain(nc);
        fprintf(stderr, "no free directory slot\n");
        return 1;
    }
    printf("mkdir %s\n", path);
    return 0;
}

static int cmd_rm(Image& img, const char* path) {
    uint32_t parent; std::string leaf;
    if (!resolve_path(img, path, &parent, &leaf) || leaf.empty()) {
        fprintf(stderr, "bad path\n");
        return 1;
    }
    FoundEntry fe = find_in_dir(img, parent, leaf.c_str());
    if (!fe.found) {
        fprintf(stderr, "not found\n");
        return 1;
    }
    if (fe.entry.attributes & 0x10) {
        auto children = list_dir(img, get_cluster(fe.entry));
        if (!children.empty()) {
            fprintf(stderr, "directory not empty\n");
            return 1;
        }
    }
    img.delete_cluster_chain(get_cluster(fe.entry));
    delete_dir_entry(img, fe);
    printf("removed %s\n", path);
    return 0;
}

// ── main ─────────────────────────────────────────────────────────────────────
static void usage() {
    fprintf(stderr,
        "tfsutil — Terminus FS image tool\n"
        "\n"
        "  tfsutil info    <disk.img|vhd>\n"
        "  tfsutil ls      <disk.img|vhd> [path]\n"
        "  tfsutil cat     <disk.img|vhd> <path>\n"
        "  tfsutil extract <disk.img|vhd> <path|/ > <outdir>\n"
        "  tfsutil format  <disk.img|vhd> <size_mb>\n"
        "  tfsutil add     <disk.img|vhd> <hostfile> <path_in_img>\n"
        "  tfsutil mkdir   <disk.img|vhd> <path>\n"
        "  tfsutil rm      <disk.img|vhd> <path>\n"
        "\n"
        "Paths are absolute from root (start with /).\n"
    );
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    const char* cmd = argv[1];
    const char* imgpath = argv[2];

    if (strcmp(cmd, "format") == 0) {
        if (argc < 4) { usage(); return 1; }
        return cmd_format(imgpath, (uint32_t)atoi(argv[3]));
    }

    bool writable = (strcmp(cmd, "add") == 0 || strcmp(cmd, "mkdir") == 0 ||
                     strcmp(cmd, "rm") == 0);
    Image img;
    if (!img.open(imgpath, writable)) return 1;

    if (strcmp(cmd, "info") == 0) return cmd_info(img);
    if (strcmp(cmd, "ls") == 0)
        return cmd_ls(img, argc >= 4 ? argv[3] : "/");
    if (strcmp(cmd, "cat") == 0) {
        if (argc < 4) { usage(); return 1; }
        return cmd_cat(img, argv[3]);
    }
    if (strcmp(cmd, "extract") == 0) {
        if (argc < 5) { usage(); return 1; }
        return cmd_extract(img, argv[3], argv[4]);
    }
    if (strcmp(cmd, "add") == 0) {
        if (argc < 5) { usage(); return 1; }
        return cmd_add(img, argv[3], argv[4]);
    }
    if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 4) { usage(); return 1; }
        return cmd_mkdir(img, argv[3]);
    }
    if (strcmp(cmd, "rm") == 0) {
        if (argc < 4) { usage(); return 1; }
        return cmd_rm(img, argv[3]);
    }

    usage();
    return 1;
}