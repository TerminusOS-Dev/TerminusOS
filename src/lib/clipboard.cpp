#include "clipboard.h"
#include "../drivers/fat32.h"
#include <stddef.h>

// Доступ к диску напрямую (как делает shell/builtin.cpp) — без отдельного
// заголовка disk.h, чтобы не тянуть конфликтующие определения.
extern uint8_t sector_buffer[512];
extern "C" void ata_read_sector(uint32_t lba);
extern uint32_t current_dir_cluster;

#define CLIP_FILE "clipboard.dat"

static char clip_hist[CLIP_SLOTS][CLIP_MAXLEN+1];
static int  clip_n = 0; // сколько слотов реально заполнено

static bool clip_streq(const char* a, const char* b){
    int i=0; for(;;i++){ if(a[i]!=b[i]) return false; if(a[i]==0) return true; }
}

// ─── персист на диск: корень диска, простой бинарный формат ────────────────
// запись = [1 байт длины][длина байт текста, могут быть \n], без разделителей
static void clipboard_save_locked(){
    uint32_t saved=current_dir_cluster;
    current_dir_cluster=FAT32_ROOT_CLUSTER;
    static char buf[CLIP_SLOTS*(CLIP_MAXLEN+1)];
    int p=0;
    for(int i=0;i<clip_n;i++){
        int n=0; while(clip_hist[i][n] && n<CLIP_MAXLEN) n++;
        buf[p++]=(char)n;
        for(int k=0;k<n;k++) buf[p++]=clip_hist[i][k];
    }
    FAT32_FindResult ex=fat32_find_entry(CLIP_FILE,0x00);
    if(ex.found) fat32_delete_entry(CLIP_FILE,0x00);
    if(p>0) fat32_create_file(CLIP_FILE, buf, (uint32_t)p);
    current_dir_cluster=saved;
}

void clipboard_push(const char* text){
    if(!text || !text[0]) return;
    if(clip_n>0 && clip_streq(clip_hist[0], text)) return; // не дублируем подряд идущий повтор
    // сдвигаем всё вниз, освобождая слот 0
    int last = (clip_n<CLIP_SLOTS) ? clip_n : CLIP_SLOTS-1;
    for(int i=last;i>0;i--){
        for(int k=0;k<=CLIP_MAXLEN;k++) clip_hist[i][k]=clip_hist[i-1][k];
    }
    int n=0; while(text[n] && n<CLIP_MAXLEN){ clip_hist[0][n]=text[n]; n++; }
    clip_hist[0][n]=0;
    if(clip_n<CLIP_SLOTS) clip_n++;
    clipboard_save_locked();
}

void clipboard_delete(int idx){
    if(idx<0 || idx>=clip_n) return;
    for(int i=idx;i<clip_n-1;i++)
        for(int k=0;k<=CLIP_MAXLEN;k++) clip_hist[i][k]=clip_hist[i+1][k];
    clip_n--;
    clip_hist[clip_n][0]=0;
    clipboard_save_locked();
}

void clipboard_clear(){
    clip_n=0;
    clipboard_save_locked();
}

void clipboard_load(){
    uint32_t saved=current_dir_cluster;
    current_dir_cluster=FAT32_ROOT_CLUSTER;
    clip_n=0;
    FAT32_FindResult res=fat32_find_entry(CLIP_FILE,0x00);
    if(res.found && !(res.entry.attributes&0x10)){
        uint32_t size=res.entry.file_size;
        uint32_t cluster=FAT32_GET_CLUSTER(&res.entry);
        static uint8_t raw[CLIP_SLOTS*(CLIP_MAXLEN+1)];
        uint32_t got=0;
        while((cluster&FAT32_MASK)>=2 && (cluster&FAT32_MASK)<(FAT32_EOC&FAT32_MASK) &&
              got<size && got<sizeof(raw)){
            uint32_t lba=fat32_cluster_to_lba(cluster);
            for(int s=0;s<FAT32_SECTORS_PER_CLUSTER && got<size && got<sizeof(raw);s++){
                ata_read_sector(lba+s);
                for(int b=0;b<FAT32_BYTES_PER_SECTOR && got<size && got<sizeof(raw);b++)
                    raw[got++]=sector_buffer[b];
            }
            cluster=fat32_get_next_cluster(cluster);
        }
        uint32_t p=0;
        while(p<got && clip_n<CLIP_SLOTS){
            uint8_t n=raw[p++]; if(n>CLIP_MAXLEN) break;
            if(p+n>got) break;
            for(uint8_t k=0;k<n;k++) clip_hist[clip_n][k]=(char)raw[p+k];
            clip_hist[clip_n][n]=0;
            p+=n; clip_n++;
        }
    }
    current_dir_cluster=saved;
}

int clipboard_count(){ return clip_n; }

const char* clipboard_get(int idx){
    if(idx<0 || idx>=clip_n) return "";
    return clip_hist[idx];
}
