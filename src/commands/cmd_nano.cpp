#include "cmd_nano.h"
#include "../lib/screen.h"
#include "../kernel/keyboard.h"
#include "../drivers/fat32.h"
#include "../drivers/disk.h"
#include "../lib/string.h"
#include "../lib/clipboard.h"

// Окно истории буфера обмена (Win+V) живёт в shell.cpp. shell.h тут не подключаем:
// он объявляет extern last_cursor_x/y, а у nano одноимённые static-переменные.
int shell_clipboard_pick();

#define EDITOR_MAX_LINES    1024
#define EDITOR_LINE_WIDTH   256
#define EDITOR_VISIBLE_ROWS 22
#undef  STATUS_BAR_ROW
#define STATUS_BAR_ROW      22
#define HELP_BAR_ROW        23
#define LINE_NUM_WIDTH       5   // 4 цифры (макс. 1024) + 1 пробел-разделитель

// ─── undo/redo ────────────────────────────────────────────────────────────────
// UNDO_MAX намеренно НЕ оставлен 64: каждый снимок — это полная копия
// editor_buffer (lines[EDITOR_MAX_LINES][EDITOR_LINE_WIDTH+1] ≈ 257 КБ).
// При 64 снимках это ~16.5 МБ статики только на undo-стек — многовато для
// голого .bss без страничной подкачки. 8 снимков — это ~2.1 МБ, что уже
// разумно. Если ОЗУ хватает с запасом — можно поднять обратно.
#define UNDO_MAX 8
struct UndoEntry {
    char lines[EDITOR_MAX_LINES][EDITOR_LINE_WIDTH + 1];
    int  total_lines, cur_line, cur_col;
};
static UndoEntry undo_stack[UNDO_MAX];
static int undo_top = -1, redo_top = -1;

// ─── буфер ───────────────────────────────────────────────────────────────────
static char editor_buffer[EDITOR_MAX_LINES][EDITOR_LINE_WIDTH + 1];
static int  current_line = 0, current_col = 0;
static int  total_lines  = 1, scroll_offset = 0;
static int  h_scroll     = 0; // горизонтальная прокрутка — строки теперь шире экрана
static char current_filename[64] = {0};
static bool is_dirty = false;
static int  last_cursor_x = 0, last_cursor_y = 0;

// ─── выделение ───────────────────────────────────────────────────────────────
static bool sel_active   = false;
static int  sel_line_start = 0, sel_col_start = 0;
static int  sel_line_end   = 0, sel_col_end   = 0;

// Буфер копирования. Раньше хватало 32 строк, но теперь есть Ctrl+A
// (выделить всё) + Ctrl+X (вырезать выделение) — вместе они могут вырезать
// хоть весь файл разом, так что размер = вместимость editor_buffer целиком.
#define CLIPBOARD_SIZE (EDITOR_MAX_LINES * (EDITOR_LINE_WIDTH + 1))
static char clipboard[CLIPBOARD_SIZE] = {0};
static int  clipboard_len = 0;

// ─── поиск ───────────────────────────────────────────────────────────────────
static char search_query[64] = {0};
static bool search_active    = false;
static int  search_results[EDITOR_MAX_LINES];
static int  search_result_count = 0, search_result_idx = 0, search_query_len = 0;
#define SEARCH_HIGHLIGHT_ATTR  0x0E   // жёлтый — используется как флаг «это совпадение» в редакторе
// SEL_ATTR вычисляется динамически через sel_get_attr()

// ─── автодополнение по Tab ────────────────────────────────────────────────────
// Полноценный выпадающий список (как в IDE) в текстовом режиме 80x25 без
// окон/мыши не сделать честно — вместо него подсказку показываем в нижней
// строке, а само дополнение работает как в шелле: Tab вставляет первый
// вариант, повторный Tab на том же месте — следующий по кругу.
#define AC_MAX_CANDIDATES 64
#define AC_MAX_WORD 40
static char ac_candidates[AC_MAX_CANDIDATES][AC_MAX_WORD];
static int  ac_count = 0, ac_index = -1;
static int  ac_line = -1, ac_start_col = -1, ac_prefix_len = 0;

extern uint8_t sector_buffer[512];
extern void draw_block_cursor(int x, int y);
extern void clear_block_cursor(int x, int y);

// ─── подсветка синтаксиса (базовая, без проверки ошибок) ─────────────────────
enum EditorLang : uint8_t { LANG_NONE, LANG_C, LANG_ASM };
enum HLClass    : uint8_t { HL_NONE, HL_KEYWORD, HL_STRING, HL_COMMENT, HL_PREPROC, HL_BRACKET, HL_NUMBER };
static EditorLang g_lang = LANG_NONE;
static uint8_t    g_hlclass[EDITOR_LINE_WIDTH]; // класс подсветки для каждой колонки текущей строки

static const char* c_keywords[] = {
    "int","char","short","long","float","double","void","unsigned","signed",
    "if","else","while","for","do","switch","case","default","break","continue",
    "return","goto","struct","union","enum","typedef","static","const","extern",
    "sizeof","volatile","register","inline","auto","class","public","private",
    "protected","new","delete","namespace","true","false","nullptr","bool",
    "uint8_t","uint16_t","uint32_t","int8_t","int16_t","int32_t","size_t",0
};
static const char* asm_keywords[] = {
    "mov","add","sub","mul","div","imul","idiv","inc","dec","cmp","test",
    "jmp","je","jne","jz","jnz","jg","jl","jge","jle","ja","jb","jae","jbe",
    "call","ret","push","pop","lea","nop","int","cli","sti","hlt","xor","and",
    "or","not","shl","shr","sal","sar","loop","in","out","rep","movsb","stosb",
    "cld","std","pushf","popf","iret","db","dw","dd","dq","section","global",
    "extern","times","org","bits",0
};
static const char* asm_registers[] = {
    "eax","ebx","ecx","edx","esi","edi","esp","ebp",
    "ax","bx","cx","dx","si","di","sp","bp",
    "al","bl","cl","dl","ah","bh","ch","dh",0
};

static inline bool is_ident_start(char c){ return c=='_'||(c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static inline bool is_ident_char(char c) { return is_ident_start(c)||(c>='0'&&c<='9'); }

// Сравнение line[start..start+len) со словом kw (без strncmp — его нет в string.h)
static bool word_eq(const char* line,int start,int len,const char* kw){
    int i=0;
    for(;i<len;i++){ if(kw[i]==0 || line[start+i]!=kw[i]) return false; }
    return kw[i]==0;
}
static bool ident_in_list(const char* line,int start,int len,const char** list){
    for(int k=0; list[k]; k++) if(word_eq(line,start,len,list[k])) return true;
    return false;
}

// Определяем язык по расширению имени файла — только это и нужно,
// полноценный разбор синтаксиса (и тем более проверка ошибок) не входит в задачу.
static EditorLang detect_lang(const char* filename){
    int len=strlen(filename);
    auto ends_with=[&](const char* ext)->bool{
        int el=strlen(ext);
        if(len<el) return false;
        for(int i=0;i<el;i++) if(filename[len-el+i]!=ext[i]) return false;
        return true;
    };
    if(ends_with(".c")||ends_with(".h")||ends_with(".cpp")||ends_with(".hpp")||ends_with(".cc")) return LANG_C;
    if(ends_with(".asm")||ends_with(".s")) return LANG_ASM;
    return LANG_NONE;
}

// Разбирает одну строку на классы подсветки — заполняет g_hlclass[0..len).
// Однострочные комментарии (// и ;), препроцессор (#...), строки/символы в
// кавычках, числа, скобки и ключевые слова/регистры. Многострочных
// комментариев (/* */) и проверки ошибок сознательно нет — это базовая
// подсветка, а не полноценный парсер.
static void classify_line(const char* line, EditorLang lang) {
    int len=strlen(line);
    for(int i=0;i<len;i++) g_hlclass[i]=HL_NONE;
    if(lang==LANG_NONE) return;

    int i=0;
    while(i<len){
        char ch=line[i];

        if(lang==LANG_C && ch=='/' && i+1<len && line[i+1]=='/'){
            for(int j=i;j<len;j++) g_hlclass[j]=HL_COMMENT;
            break;
        }
        if(lang==LANG_ASM && ch==';'){
            for(int j=i;j<len;j++) g_hlclass[j]=HL_COMMENT;
            break;
        }
        if(lang==LANG_C && ch=='#' && i==0){
            for(int j=i;j<len;j++) g_hlclass[j]=HL_PREPROC;
            break;
        }
        if(ch=='"' || ch=='\''){
            char q=ch; int j=i; g_hlclass[j]=HL_STRING; j++;
            while(j<len && line[j]!=q){ g_hlclass[j]=HL_STRING; j++; }
            if(j<len){ g_hlclass[j]=HL_STRING; j++; }
            i=j; continue;
        }
        if(ch=='('||ch==')'||ch=='{'||ch=='}'||ch=='['||ch==']'){
            g_hlclass[i]=HL_BRACKET; i++; continue;
        }
        if(ch>='0'&&ch<='9'){
            int j=i;
            while(j<len && ((line[j]>='0'&&line[j]<='9')||line[j]=='x'||line[j]=='X'||
                            (line[j]>='a'&&line[j]<='f')||(line[j]>='A'&&line[j]<='F'))) { g_hlclass[j]=HL_NUMBER; j++; }
            i=j; continue;
        }
        if(is_ident_start(ch)){
            int j=i; while(j<len && is_ident_char(line[j])) j++;
            int wlen=j-i;
            bool kw = (lang==LANG_C)
                ? ident_in_list(line,i,wlen,c_keywords)
                : (ident_in_list(line,i,wlen,asm_keywords) || ident_in_list(line,i,wlen,asm_registers));
            if(kw) for(int k=i;k<j;k++) g_hlclass[k]=HL_KEYWORD;
            i=j; continue;
        }
        i++;
    }
}
static inline uint8_t hl_attr(uint8_t cls){
    // БАГ БЫЛ ЗДЕСЬ: раньше фон был жёстко зашит "чёрным" (0x0_), что на
    // синей/другой теме давало чёрные прямоугольники вокруг подсвеченных
    // токенов. Фон теперь берём из текущей темы, как это уже делает sel_attr().
    uint8_t bg = theme_bg & 0x0F;
    uint8_t fg;
    switch(cls){
        case HL_KEYWORD: fg=0x0B; break; // ярко-голубой
        case HL_STRING:  fg=0x0A; break; // ярко-зелёный
        case HL_COMMENT: fg=0x08; break; // серый
        case HL_PREPROC: fg=0x0D; break; // ярко-пурпурный
        case HL_BRACKET: fg=0x0F; break; // белый
        case HL_NUMBER:  fg=0x09; break; // ярко-синий
        default:          fg=0x07; break;
    }
    if(fg==bg) fg=(fg^0x0F)&0x0F; // текст не должен сливаться с фоном
    return (uint8_t)((bg<<4)|fg);
}

// ─── helpers ─────────────────────────────────────────────────────────────────
static inline uint8_t text_attr() { return get_theme_color(); }
static inline uint8_t bar_attr()  { return (uint8_t)((theme_bar_bg << 4) | (theme_bar_fg & 0x0F)); }
// Цвет выделения = инверсия текущей темы (fg↔bg) — заметно на любой теме
static inline uint8_t sel_attr()  {
    uint8_t fg = theme_fg & 0x0F;
    uint8_t bg = theme_bg & 0x0F;
    // Если fg == bg (нечитаемо) — добавляем яркость к тексту
    if (fg == bg) fg = (fg ^ 0x0F) & 0x0F;
    return (uint8_t)((fg << 4) | bg);
}
// Цвет подсветки найденного текста — тоже завязан на фон темы, а не на
// жёстко зашитый чёрный (тот же баг, что чинили для hl_attr()).
static inline uint8_t search_attr() {
    uint8_t bg = theme_bg & 0x0F;
    uint8_t fg = 0x0E; // жёлтый
    if (fg == bg) fg = (fg ^ 0x0F) & 0x0F;
    return (uint8_t)((bg << 4) | fg);
}

static void print_char_at(char c, int x, int y, uint8_t color) {
    video_memory[y * 80 + x] = (uint16_t)((color << 8) | (uint8_t)c);
}
static void print_at(const char* str, int x, int y, uint8_t color) {
    int pos = y * 80 + x;
    for(int i = 0; str[i]; i++) video_memory[pos++] = (uint16_t)((color << 8) | (uint8_t)str[i]);
}
static void editor_clear_content() {
    uint8_t a = text_attr();
    for(int r = 0; r < EDITOR_VISIBLE_ROWS; r++)
        for(int c = 0; c < 80; c++)
            video_memory[r * 80 + c] = (uint16_t)((a << 8) | ' ');
}
static void editor_print_hex(uint32_t n) {
    const char* h = "0123456789ABCDEF";
    char buf[11]; buf[0]='0'; buf[1]='x';
    for(int i=0;i<8;i++){buf[9-i]=h[n&0xF];n>>=4;} buf[10]=0; print(buf);
}

// ─── выделение: нормализация (start <= end) ──────────────────────────────────
static void sel_normalize(int& sl, int& sc, int& el, int& ec) {
    sl = sel_line_start; sc = sel_col_start;
    el = sel_line_end;   ec = sel_col_end;
    // Если курсор "до" начала — переставляем
    if (sl > el || (sl == el && sc > ec)) {
        int tl=sl,tc=sc; sl=el; sc=ec; el=tl; ec=tc;
    }
}

// Проверяет, попадает ли символ (line, col) в выделение
static bool in_selection(int line, int col) {
    if (!sel_active) return false;
    int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);
    if (line < sl || line > el) return false;
    if (sl == el) return col >= sc && col < ec;  // однострочное
    if (line == sl) return col >= sc;             // первая строка: от sc до конца
    if (line == el) return col < ec;              // последняя строка: от начала до ec
    return true;                                  // средние строки: целиком
}

// ─── undo/redo ────────────────────────────────────────────────────────────────
static void undo_save() {
    if (undo_top >= UNDO_MAX - 1) {
        for(int i=0;i<UNDO_MAX-1;i++) undo_stack[i]=undo_stack[i+1];
        undo_top=UNDO_MAX-2;
    }
    undo_top++; redo_top=undo_top;
    UndoEntry* e=&undo_stack[undo_top];
    for(int i=0;i<total_lines;i++) strcpy(e->lines[i],editor_buffer[i]);
    e->total_lines=total_lines; e->cur_line=current_line; e->cur_col=current_col;
}
static void undo_restore(const UndoEntry* e) {
    for(int i=0;i<e->total_lines;i++) strcpy(editor_buffer[i],e->lines[i]);
    total_lines=e->total_lines; current_line=e->cur_line; current_col=e->cur_col;
    is_dirty=true; sel_active=false;
}
static void do_undo() { if(undo_top>0){ undo_top--; undo_restore(&undo_stack[undo_top]); } }
static void do_redo() { if(undo_top<redo_top){ undo_top++; undo_restore(&undo_stack[undo_top]); } }

// ─── прокрутка ───────────────────────────────────────────────────────────────
static void scroll_to_cursor() {
    if(current_line < scroll_offset) scroll_offset=current_line;
    else if(current_line >= scroll_offset+EDITOR_VISIBLE_ROWS)
        scroll_offset=current_line-EDITOR_VISIBLE_ROWS+1;

    // Строка (до 256 симв.) шире видимой области (80-LINE_NUM_WIDTH) —
    // подгоняем горизонтальный сдвиг под курсор так же, как вертикальный.
    int max_disp = 80-LINE_NUM_WIDTH;
    if(current_col < h_scroll) h_scroll=current_col;
    else if(current_col >= h_scroll+max_disp) h_scroll=current_col-max_disp+1;
}

// ─── статус-бар ──────────────────────────────────────────────────────────────
static void editor_draw_status_bar() {
    uint8_t bar=bar_attr(), txt=text_attr();
    for(int i=0;i<80;i++) print_char_at(' ',i,STATUS_BAR_ROW,bar);

    char status[80]; int len=0;
    const char* title=" NANO - ";
    for(int i=0;title[i];i++) status[len++]=title[i];
    for(int i=0;current_filename[i]&&len<55;i++) status[len++]=current_filename[i];
    if(is_dirty){status[len++]=' ';status[len++]='*';}
    if(sel_active){
        const char* s=" [SEL]";
        for(int i=0;s[i]&&len<70;i++) status[len++]=s[i];
    }

    char pos_str[20]; int pl=0;
    pos_str[pl++]='L'; pos_str[pl++]=':';
    int ln=current_line+1;
    if(ln>=1000) pos_str[pl++]=(char)('0'+ln/1000);
    if(ln>=100)  pos_str[pl++]=(char)('0'+(ln/100)%10);
    if(ln>=10)   pos_str[pl++]=(char)('0'+(ln/10)%10);
    pos_str[pl++]=(char)('0'+ln%10);
    pos_str[pl++]=' '; pos_str[pl++]='C'; pos_str[pl++]=':';
    int cc=current_col+1;
    if(cc>=100) pos_str[pl++]=(char)('0'+cc/100);
    if(cc>=10)  pos_str[pl++]=(char)('0'+(cc/10)%10);
    pos_str[pl++]=(char)('0'+cc%10);
    pos_str[pl++]=' '; pos_str[pl]=0;

    status[len]=0;
    for(int i=0;i<len;i++) print_char_at(status[i],i,STATUS_BAR_ROW,bar);
    int ps=80-pl;
    for(int i=0;i<pl;i++) print_char_at(pos_str[i],ps+i,STATUS_BAR_ROW,bar);

    for(int i=0;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,txt);
    const char* help=" ^H Help  ^ESC Save  S-ESC Quit  ^F Search  ^Z/^Y Undo/Redo  Lines: ";
    int hl=0;
    for(;help[hl];hl++) print_char_at(help[hl],hl,HELP_BAR_ROW,txt);

    char tl_str[8]; int tl=0; int t=total_lines;
    if(t>=1000) tl_str[tl++]=(char)('0'+t/1000);
    if(t>=100)  tl_str[tl++]=(char)('0'+(t/100)%10);
    if(t>=10)   tl_str[tl++]=(char)('0'+(t/10)%10);
    tl_str[tl++]=(char)('0'+t%10); tl_str[tl]=0;
    for(int i=0;i<tl;i++) print_char_at(tl_str[i],hl+i,HELP_BAR_ROW,txt);

    if(total_lines>EDITOR_VISIBLE_ROWS){
        const char* scr=" [SCROLL] "; int sl2=strlen(scr);
        for(int i=0;i<sl2;i++) print_char_at(scr[i],80-sl2+i,HELP_BAR_ROW,bar_attr());
    }
}

// ─── перерисовка ─────────────────────────────────────────────────────────────
static void editor_redraw() {
    clear_block_cursor(last_cursor_x,last_cursor_y);
    scroll_to_cursor();
    editor_clear_content();

    uint8_t attr=text_attr(), num_attr=bar_attr();
    uint8_t s_attr=sel_attr();

    for(int row=0;row<EDITOR_VISIBLE_ROWS;row++) {
        int abs_line=scroll_offset+row;
        if(abs_line>=total_lines) break;

        int n=abs_line+1;
        char nb[6];
        nb[0]=(n>=1000)?(char)('0'+n/1000):' ';
        nb[1]=(n>=100) ?(char)('0'+(n/100)%10):' ';
        nb[2]=(n>=10)  ?(char)('0'+(n/10)%10):' ';
        nb[3]=(char)('0'+n%10); nb[4]=' ';
        for(int i=0;i<LINE_NUM_WIDTH;i++) print_char_at(nb[i],i,row,num_attr);

        int line_len=strlen(editor_buffer[abs_line]);
        int max_disp=80-LINE_NUM_WIDTH;
        // line_len до 256 симв. — экрану видна только область
        // [h_scroll, h_scroll+max_disp), остальное прокручено за края.
        int line_end=line_len; if(line_end>h_scroll+max_disp) line_end=h_scroll+max_disp;

        classify_line(editor_buffer[abs_line], g_lang);

        int ci=h_scroll;
        while(ci<line_end) {
            int dc=ci-h_scroll; // видимая колонка на экране (0..max_disp-1)
            // Приоритет: выделение > поиск > синтаксис > обычный
            uint8_t a = (g_lang!=LANG_NONE && g_hlclass[ci]!=HL_NONE) ? hl_attr(g_hlclass[ci]) : attr;
            if(in_selection(abs_line,ci)) {
                a=s_attr;
            } else if(search_active && search_query_len>0 && ci+search_query_len<=line_len) {
                bool match=true;
                for(int qi=0;qi<search_query_len&&match;qi++)
                    if(editor_buffer[abs_line][ci+qi]!=search_query[qi]) match=false;
                if(match) a=SEARCH_HIGHLIGHT_ATTR;
            }

            // Если текущий символ часть найденного совпадения — рисуем весь блок
            if(a==SEARCH_HIGHLIGHT_ATTR) {
                uint8_t sa=search_attr();
                for(int qi=0;qi<search_query_len&&ci+qi<line_len&&(ci+qi-h_scroll)<max_disp;qi++)
                    print_char_at(editor_buffer[abs_line][ci+qi],
                                  (ci+qi-h_scroll)+LINE_NUM_WIDTH,row,sa);
                ci+=search_query_len;
            } else {
                print_char_at(editor_buffer[abs_line][ci],dc+LINE_NUM_WIDTH,row,a);
                ci++;
            }
        }
    }

    editor_draw_status_bar();
    int sy=current_line-scroll_offset;
    last_cursor_x=(current_col-h_scroll)+LINE_NUM_WIDTH; last_cursor_y=sy;
    draw_block_cursor((current_col-h_scroll)+LINE_NUM_WIDTH,sy);
    update_vga_cursor((current_col-h_scroll)+LINE_NUM_WIDTH,sy);
}

// ─── загрузка ────────────────────────────────────────────────────────────────
static void editor_load_file(const char* filename) {
    println("Loading...");
    FAT32_FindResult result=fat32_find_entry(filename,0x00);
    if(!result.found){println("New file");for(int i=0;i<1000000;i++);return;}
    uint32_t size=result.entry.file_size;
    uint32_t cluster=FAT32_GET_CLUSTER(&result.entry);
    if(size==0){println("Empty file");for(int i=0;i<1000000;i++);return;}

    static char file_content[EDITOR_MAX_LINES*(EDITOR_LINE_WIDTH+1)];
    uint32_t bytes_read=0, max_bytes=(uint32_t)(EDITOR_MAX_LINES*(EDITOR_LINE_WIDTH+1)-1);

    while((cluster&FAT32_MASK)<(FAT32_EOC&FAT32_MASK)&&bytes_read<size){
        uint32_t lba=fat32_cluster_to_lba(cluster);
        for(int sec=0;sec<FAT32_SECTORS_PER_CLUSTER&&bytes_read<size;sec++){
            ata_read_sector(lba+sec);
            for(int i=0;i<512&&bytes_read<size&&bytes_read<max_bytes;i++)
                file_content[bytes_read++]=sector_buffer[i];
        }
        cluster=fat32_get_next_cluster(cluster);
    }
    file_content[bytes_read]=0;

    int line=0,col=0;
    for(uint32_t i=0;i<bytes_read&&line<EDITOR_MAX_LINES;i++){
        char c=file_content[i];
        if(c=='\n'){editor_buffer[line][col]=0;line++;col=0;}
        else if(c=='\r'){continue;}
        else if(col<EDITOR_LINE_WIDTH){editor_buffer[line][col++]=c;}
    }
    if(col>0||line==0){editor_buffer[line][col]=0;line++;}
    total_lines=line;

    print("Loaded ");editor_print_hex(bytes_read);
    print(" bytes, ");editor_print_hex(total_lines);println(" lines");
    for(int i=0;i<2000000;i++);
}

// ─── сохранение ──────────────────────────────────────────────────────────────
static void editor_save_file() {
    if(strlen(current_filename)==0){
        for(int i=0;i<80;i++) print_char_at(' ',i,21,0x0C);
        print_at("ERROR: No filename!",0,21,0x0C); return;
    }
    static char content[EDITOR_MAX_LINES*EDITOR_LINE_WIDTH];
    int pos=0;
    for(int i=0;i<total_lines;i++){
        int len=strlen(editor_buffer[i]);
        for(int j=0;j<len;j++) content[pos++]=editor_buffer[i][j];
        if(i<total_lines-1) content[pos++]='\n';
    }
    content[pos]=0;

    editor_clear_content();
    for(int i=0;i<80;i++) print_char_at(' ',i,0,bar_attr());
    print_at(" Saving...",0,0,bar_attr());

    FAT32_FindResult existing=fat32_find_entry(current_filename,0x00);
    if(existing.found) fat32_delete_entry(current_filename,0x00);
    bool ok=fat32_create_file(current_filename,content,pos);
    if(ok){print_at(" Saved OK ",0,0,bar_attr());is_dirty=false;}
    else  {print_at(" SAVE FAILED! ",0,0,0x0C);}
    for(volatile int i=0;i<5000000;i++);
    editor_redraw();
}

// ─── диалог выхода (ESC) ─────────────────────────────────────────────────────
// Возвращает true если надо выходить, false — остаться
static bool editor_exit_dialog() {
    if(!is_dirty) return true;   // нечего спрашивать

    uint8_t bar=bar_attr();
    for(int i=0;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
    const char* msg=" Save before exit? [Y]es / [N]o / [C]ancel";
    for(int i=0;msg[i];i++) print_char_at(msg[i],i,HELP_BAR_ROW,bar);

    while(1){
        uint8_t k=0; while(k==0) k=get_key();
        if(k=='y'||k=='Y'){ editor_save_file(); return true; }
        if(k=='n'||k=='N') return true;   // выйти без сохранения
        if(k=='c'||k=='C'||k==27) { editor_draw_status_bar(); return false; }
    }
}

// ─── редактирование ──────────────────────────────────────────────────────────
static void editor_insert_char(char c) {
    if(current_col>=EDITOR_LINE_WIDTH) return;
    undo_save();
    sel_active=false;
    int len=strlen(editor_buffer[current_line]);
    for(int i=len;i>current_col;i--)
        editor_buffer[current_line][i]=editor_buffer[current_line][i-1];
    editor_buffer[current_line][current_col]=c;
    editor_buffer[current_line][len+1]=0;
    current_col++; is_dirty=true; editor_redraw();
}
static void editor_new_line() {
    if(total_lines>=EDITOR_MAX_LINES) return;
    undo_save(); sel_active=false;
    for(int i=total_lines;i>current_line+1;i--)
        strcpy(editor_buffer[i],editor_buffer[i-1]);
    editor_buffer[current_line+1][0]=0;
    if(current_col<(int)strlen(editor_buffer[current_line])){
        strcpy(editor_buffer[current_line+1],&editor_buffer[current_line][current_col]);
        editor_buffer[current_line][current_col]=0;
    }
    total_lines++; current_line++; current_col=0;
    is_dirty=true; editor_redraw();
}
static void editor_delete_char() {
    if(current_col>0){
        undo_save(); sel_active=false;
        int len=strlen(editor_buffer[current_line]);
        for(int i=current_col-1;i<len;i++)
            editor_buffer[current_line][i]=editor_buffer[current_line][i+1];
        current_col--; is_dirty=true; editor_redraw();
    } else if(current_line>0){
        int pl=strlen(editor_buffer[current_line-1]);
        int cl=strlen(editor_buffer[current_line]);
        if(pl+cl<EDITOR_LINE_WIDTH){
            undo_save(); sel_active=false;
            strcat(editor_buffer[current_line-1],editor_buffer[current_line]);
            for(int i=current_line;i<total_lines-1;i++)
                strcpy(editor_buffer[i],editor_buffer[i+1]);
            total_lines--; current_line--; current_col=pl;
            is_dirty=true; editor_redraw();
        }
    }
}
static void editor_delete_forward() {
    int len=strlen(editor_buffer[current_line]);
    if(current_col<len){
        undo_save(); sel_active=false;
        for(int i=current_col;i<len;i++)
            editor_buffer[current_line][i]=editor_buffer[current_line][i+1];
        is_dirty=true; editor_redraw();
    } else if(current_line<total_lines-1){
        int nl=strlen(editor_buffer[current_line+1]);
        if(len+nl<EDITOR_LINE_WIDTH){
            undo_save(); sel_active=false;
            strcat(editor_buffer[current_line],editor_buffer[current_line+1]);
            for(int i=current_line+1;i<total_lines-1;i++)
                strcpy(editor_buffer[i],editor_buffer[i+1]);
            editor_buffer[total_lines-1][0]=0;
            total_lines--; is_dirty=true; editor_redraw();
        }
    }
}

// ─── выделение ───────────────────────────────────────────────────────────────
// Начало выделения: запоминаем точку старта
static void sel_start() {
    if(!sel_active){
        sel_active=true;
        sel_line_start=current_line; sel_col_start=current_col;
    }
    // Обновляем конец (= текущая позиция курсора ПОСЛЕ перемещения)
}
static void sel_update_end() {
    sel_line_end=current_line; sel_col_end=current_col;
}

// Ctrl+C — копировать выделенный текст в clipboard
static void editor_copy() {
    if(!sel_active) return;  // нет выделения — clipboard не трогаем
    int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);

    clipboard_len=0;
    for(int ln=sl; ln<=el && clipboard_len<CLIPBOARD_SIZE-2; ln++){
        int from = (ln==sl) ? sc : 0;
        int to   = (ln==el) ? ec : (int)strlen(editor_buffer[ln]);
        for(int ci=from; ci<to && clipboard_len<CLIPBOARD_SIZE-2; ci++)
            clipboard[clipboard_len++]=editor_buffer[ln][ci];
        if(ln<el && clipboard_len<CLIPBOARD_SIZE-2)
            clipboard[clipboard_len++]='\n';
    }
    clipboard[clipboard_len]=0;
    clipboard_push(clipboard);   // + в общую историю (её видят шелл и gite)
    sel_active=false;
    editor_redraw();
}

// Вставка произвольного текста в текущую позицию (Ctrl+V и Win+V)
static void editor_paste_text(const char* txt, int n) {
    if(n<=0) return;
    undo_save();
    sel_active=false;
    for(int ci=0; ci<n; ci++){
        char c=txt[ci];
        if(c=='\n'){
            editor_new_line();
        } else {
            // Вставляем символ без вызова undo_save внутри editor_insert_char
            if(current_col<EDITOR_LINE_WIDTH){
                int len=strlen(editor_buffer[current_line]);
                for(int i=len;i>current_col;i--)
                    editor_buffer[current_line][i]=editor_buffer[current_line][i-1];
                editor_buffer[current_line][current_col]=c;
                editor_buffer[current_line][len+1]=0;
                current_col++;
            }
        }
    }
    is_dirty=true;
    editor_redraw();
}

// Ctrl+V — вставить clipboard в текущую позицию.
// Если в этом сеансе nano ещё ничего не копировали — берём самую свежую запись
// общей истории (то, что скопировали в шелле или gite).
static void editor_paste() {
    if(clipboard_len==0 && clipboard_count()>0){
        const char* s=clipboard_get(0);
        while(s[clipboard_len] && clipboard_len<CLIPBOARD_SIZE-2){
            clipboard[clipboard_len]=s[clipboard_len];
            clipboard_len++;
        }
        clipboard[clipboard_len]=0;
    }
    editor_paste_text(clipboard, clipboard_len);
}

// Ctrl+A — выделить весь текст
static void editor_select_all() {
    sel_active=true;
    sel_line_start=0; sel_col_start=0;
    sel_line_end=total_lines-1; sel_col_end=strlen(editor_buffer[total_lines-1]);
    editor_redraw();
}

// Ctrl+K — скопировать текущую строку целиком (без выделения)
static void editor_copy_line() {
    int len=strlen(editor_buffer[current_line]);
    clipboard_len=0;
    for(int i=0;i<len && clipboard_len<CLIPBOARD_SIZE-2;i++) clipboard[clipboard_len++]=editor_buffer[current_line][i];
    clipboard[clipboard_len++]='\n';
    clipboard[clipboard_len]=0;
    clipboard_push(clipboard);
}

// Ctrl+Shift+A — вырезать текущую строку целиком в clipboard
static void editor_cut_line() {
    undo_save(); sel_active=false;
    int len=strlen(editor_buffer[current_line]);
    clipboard_len=0;
    for(int i=0;i<len && clipboard_len<CLIPBOARD_SIZE-2;i++) clipboard[clipboard_len++]=editor_buffer[current_line][i];
    clipboard[clipboard_len++]='\n';
    clipboard[clipboard_len]=0;
    clipboard_push(clipboard);

    if(total_lines>1){
        for(int i=current_line;i<total_lines-1;i++) strcpy(editor_buffer[i],editor_buffer[i+1]);
        total_lines--;
        if(current_line>=total_lines) current_line=total_lines-1;
    } else {
        editor_buffer[0][0]=0; // единственная строка в файле — просто очищаем
    }
    current_col=0;
    is_dirty=true; editor_redraw();
}

// Удаляет текущее выделение (используется Ctrl+X). Аккуратно склеивает
// "голову" первой строки с "хвостом" последней, если выделение многострочное.
static void editor_delete_selection() {
    if(!sel_active) return;
    int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);
    undo_save(); sel_active=false;

    if(sl==el){
        int len=strlen(editor_buffer[sl]);
        int tail=len-ec; if(tail<0) tail=0;
        for(int i=0;i<=tail;i++) editor_buffer[sl][sc+i]=editor_buffer[sl][ec+i];
    } else {
        char tail_buf[EDITOR_LINE_WIDTH+1];
        strcpy(tail_buf,&editor_buffer[el][ec]);
        editor_buffer[sl][sc]=0;
        int sl_len=strlen(editor_buffer[sl]);
        int room=EDITOR_LINE_WIDTH-sl_len;
        int tl=strlen(tail_buf); if(tl>room) tl=room;
        for(int i=0;i<tl;i++) editor_buffer[sl][sl_len+i]=tail_buf[i];
        editor_buffer[sl][sl_len+tl]=0;

        int removed=el-sl;
        for(int i=el+1;i<total_lines;i++) strcpy(editor_buffer[i-removed],editor_buffer[i]);
        total_lines-=removed;
    }
    current_line=sl; current_col=sc;
    is_dirty=true; editor_redraw();
}

// Ctrl+X — вырезать выделение (копирует в clipboard, затем удаляет).
// Если ничего не выделено — ничего не делает (за "вырезать строку" отвечает Ctrl+Shift+A).
static void editor_cut() {
    if(!sel_active) return;
    int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);

    clipboard_len=0;
    for(int ln=sl; ln<=el && clipboard_len<CLIPBOARD_SIZE-2; ln++){
        int from = (ln==sl) ? sc : 0;
        int to   = (ln==el) ? ec : (int)strlen(editor_buffer[ln]);
        for(int ci=from; ci<to && clipboard_len<CLIPBOARD_SIZE-2; ci++)
            clipboard[clipboard_len++]=editor_buffer[ln][ci];
        if(ln<el && clipboard_len<CLIPBOARD_SIZE-2)
            clipboard[clipboard_len++]='\n';
    }
    clipboard[clipboard_len]=0;
    clipboard_push(clipboard);

    editor_delete_selection();
}

// ─── прыжки/удаление по словам (Ctrl+←/→, Ctrl+Backspace/Delete) ─────────────
static void editor_word_left() {
    char* ln=editor_buffer[current_line];
    int c=current_col;
    if(c==0) return;
    c--;
    while(c>0 && ln[c]==' ') c--;
    if(is_ident_char(ln[c])) { while(c>0 && is_ident_char(ln[c-1])) c--; }
    else { while(c>0 && ln[c-1]!=' ' && !is_ident_char(ln[c-1])) c--; }
    current_col=c;
}
static void editor_word_right() {
    char* ln=editor_buffer[current_line];
    int len=strlen(ln);
    int c=current_col;
    if(c>=len) return;
    while(c<len && ln[c]==' ') c++;
    if(c<len && is_ident_char(ln[c])) { while(c<len && is_ident_char(ln[c])) c++; }
    else { while(c<len && ln[c]!=' ' && !is_ident_char(ln[c])) c++; }
    current_col=c;
}
// Начало слова перед курсором (для автодополнения)
static int ac_word_start(int line, int col) {
    char* ln = editor_buffer[line];
    int c = col;
    while(c>0 && is_ident_char(ln[c-1])) c--;
    return c;
}
// Добавляет word[0..wlen) в список кандидатов, если он начинается с prefix,
// длиннее его и такого варианта в списке ещё нет.
static void ac_try_add(const char* word, int wlen, const char* prefix, int plen) {
    if(wlen<=plen || wlen>=AC_MAX_WORD) return;
    for(int i=0;i<plen;i++) if(word[i]!=prefix[i]) return;
    for(int i=0;i<ac_count;i++) {
        bool same=true;
        for(int k=0;k<wlen&&same;k++) if(ac_candidates[i][k]!=word[k]) same=false;
        if(same && ac_candidates[i][wlen]==0) return;
    }
    if(ac_count>=AC_MAX_CANDIDATES) return;
    for(int k=0;k<wlen;k++) ac_candidates[ac_count][k]=word[k];
    ac_candidates[ac_count][wlen]=0;
    ac_count++;
}
// Собирает кандидатов для prefix: ключевые слова/регистры текущего языка +
// все идентификаторы, уже встречающиеся в открытом файле.
static void ac_build(const char* prefix, int plen) {
    ac_count=0;
    if(g_lang==LANG_C) {
        for(int k=0; c_keywords[k]; k++) ac_try_add(c_keywords[k], strlen(c_keywords[k]), prefix, plen);
    } else if(g_lang==LANG_ASM) {
        for(int k=0; asm_keywords[k]; k++)  ac_try_add(asm_keywords[k],  strlen(asm_keywords[k]),  prefix, plen);
        for(int k=0; asm_registers[k]; k++) ac_try_add(asm_registers[k], strlen(asm_registers[k]), prefix, plen);
    }
    for(int ln=0; ln<total_lines; ln++){
        char* line=editor_buffer[ln];
        int len=strlen(line);
        int i=0;
        while(i<len){
            if(is_ident_start(line[i])){
                int j=i; while(j<len && is_ident_char(line[j])) j++;
                ac_try_add(&line[i], j-i, prefix, plen);
                i=j;
            } else i++;
        }
    }
}

static void editor_delete_word_back() {
    if(current_col==0){ editor_delete_char(); return; } // на границе строки — обычное слияние
    undo_save(); sel_active=false;
    int old_col=current_col;
    editor_word_left();
    int new_col=current_col;
    char* ln=editor_buffer[current_line];
    int len=strlen(ln);
    for(int i=0;i<=len-old_col;i++) ln[new_col+i]=ln[old_col+i];
    current_col=new_col;
    is_dirty=true; editor_redraw();
}
static void editor_delete_word_fwd() {
    int len=strlen(editor_buffer[current_line]);
    if(current_col>=len){ editor_delete_forward(); return; } // конец строки — обычное слияние
    undo_save(); sel_active=false;
    int old_col=current_col;
    editor_word_right();
    int new_col=current_col;
    current_col=old_col;
    char* ln=editor_buffer[current_line];
    int len2=strlen(ln);
    for(int i=0;i<=len2-new_col;i++) ln[old_col+i]=ln[new_col+i];
    is_dirty=true; editor_redraw();
}

// ─── Ctrl+Shift+X — переход к строке[:колонке] ───────────────────────────────
// Если указанной строки ещё нет — дозаполняем файл пустыми строками до неё
// (в пределах EDITOR_MAX_LINES). Если колонка больше длины строки — курсор
// просто встаёт в её конец.
static void editor_goto_dialog() {
    uint8_t bar=bar_attr();
    for(int i=0;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
    const char* prompt=" Go to line[:col]: ";
    int px=0;
    for(;prompt[px];px++) print_char_at(prompt[px],px,HELP_BAR_ROW,bar);

    char input[16]; int ilen=0; input[0]=0;
    while(1){
        for(int i=px;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
        for(int i=0;i<ilen;i++) print_char_at(input[i],px+i,HELP_BAR_ROW,bar);
        print_char_at('_',px+ilen,HELP_BAR_ROW,bar);
        uint8_t c=0; while(c==0) c=get_key();
        if(c==27){editor_draw_status_bar();return;}
        if(c=='\n') break;
        if(c=='\b'){if(ilen>0){ilen--;input[ilen]=0;}continue;}
        if(((c>='0'&&c<='9')||c==':')&&ilen<15){input[ilen++]=(char)c;input[ilen]=0;}
    }
    if(ilen==0){editor_draw_status_bar();return;}

    int want_line=0, want_col=0; bool has_col=false;
    int i=0;
    for(;input[i] && input[i]!=':'; i++) want_line=want_line*10+(input[i]-'0');
    if(input[i]==':'){
        has_col=true; i++;
        for(;input[i];i++) want_col=want_col*10+(input[i]-'0');
    }

    if(want_line<1) want_line=1;
    if(want_line>EDITOR_MAX_LINES) want_line=EDITOR_MAX_LINES;

    if(total_lines<want_line){
        undo_save();
        while(total_lines<want_line){ editor_buffer[total_lines][0]=0; total_lines++; }
        is_dirty=true;
    }
    current_line=want_line-1;

    int line_len=strlen(editor_buffer[current_line]);
    if(has_col){
        if(want_col<0) want_col=0;
        if(want_col>EDITOR_LINE_WIDTH) want_col=EDITOR_LINE_WIDTH;
        if(want_col>line_len) want_col=line_len; // строка короче запрошенной колонки — едем до её конца
        current_col=want_col;
    } else {
        current_col=0;
    }

    sel_active=false;
    editor_redraw();
}

// ─── Ctrl+H — полноэкранная справка по горячим клавишам ──────────────────────
// Нижняя строка подсказки слишком узкая, чтобы показать все комбинации —
// вместо неё отдельный экран, который открывается поверх и закрывается
// по любой клавише, не трогая содержимое файла.
// Текст на английском: экран текстового режима использует аппаратный шрифт
// VGA (CP437-подобный) без кириллических глифов — русский текст здесь
// просто не отрисуется (что и произошло в первой версии).
static void editor_show_help() {
    uint8_t a=text_attr(), title=bar_attr();
    for(int i=0;i<80*25;i++) video_memory[i]=(uint16_t)((a<<8)|' ');

    const char* lines[] = {
        " TerminusOS nano --- keyboard shortcuts",
        "",
        " File",
        "   ^ESC                    Save and exit",
        "   Shift+ESC               Exit without saving",
        "   ESC                     Exit (asks if unsaved changes)",
        "",
        " Editing",
        "   Backspace / Delete      Delete character",
        "   Ctrl+Backspace / Delete Delete word",
        "   Ctrl+Z / Ctrl+Y         Undo / Redo",
        "   Ctrl+C / Ctrl+X / Ctrl+V  Copy / Cut selection / Paste",
        "   Ctrl+K / Ctrl+Shift+A   Copy / Cut current line",
        "   Win+V                   Clipboard history (shared with shell)",
        " Selection & navigation",
        "   Shift+arrows            Select",
        "   Ctrl+Shift+Left/Right   Select by word",
        "   Ctrl+A                  Select all",
        "   Ctrl+Left/Right         Jump by word",
        "   Ctrl+F                  Search",
        "   Ctrl+Shift+X            Go to line[:column]",
        " Autocomplete",
        "   Tab                     Complete word / next match / indent",
        "   Ctrl+H                  This help screen",
        "        Press any key to close...",
    };
    int n=(int)(sizeof(lines)/sizeof(lines[0]));
    for(int row=0; row<n && row<25; row++){
        const char* s=lines[row];
        uint8_t rowattr=(row==0)?title:a;
        for(int x=0;x<80 && s[x];x++) print_char_at(s[x],x,row,rowattr);
    }

    uint8_t c=0; while(c==0) c=get_key();
    editor_redraw();
}

// ─── поиск ───────────────────────────────────────────────────────────────────
static void search_collect_results() {
    search_result_count=0;
    search_query_len=strlen(search_query);
    if(search_query_len==0) return;
    for(int ln=0;ln<total_lines;ln++)
        if(strstr(editor_buffer[ln],search_query))
            search_results[search_result_count++]=ln;
}
static void search_draw_bar() {
    uint8_t bar=bar_attr();
    for(int i=0;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
    int x=0;
    print_char_at('[',x++,HELP_BAR_ROW,bar);
    int cur=search_result_count>0?search_result_idx+1:0;
    if(cur>=10) print_char_at((char)('0'+cur/10),x++,HELP_BAR_ROW,bar);
    print_char_at((char)('0'+cur%10),x++,HELP_BAR_ROW,bar);
    print_char_at('/',x++,HELP_BAR_ROW,bar);
    int tot=search_result_count;
    if(tot>=10) print_char_at((char)('0'+tot/10),x++,HELP_BAR_ROW,bar);
    print_char_at((char)('0'+tot%10),x++,HELP_BAR_ROW,bar);
    print_char_at(']',x++,HELP_BAR_ROW,bar);
    const char* hint=" ESC Exit  Up/Dn Prev/Next : ";
    for(int i=0;hint[i]&&x<80;i++) print_char_at(hint[i],x++,HELP_BAR_ROW,bar);
    for(int i=0;search_query[i]&&x<80;i++)
        print_char_at(search_query[i],x++,HELP_BAR_ROW,(uint8_t)SEARCH_HIGHLIGHT_ATTR);
}
static void search_goto_current() {
    if(search_result_count==0) return;
    current_line=search_results[search_result_idx];
    char* pos=strstr(editor_buffer[current_line],search_query);
    current_col=pos?(int)(pos-editor_buffer[current_line]):0;
    scroll_to_cursor();
}
static void editor_search() {
    uint8_t bar=bar_attr();
    for(int i=0;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
    const char* prompt=" Search: ";
    int px=0;
    for(;prompt[px];px++) print_char_at(prompt[px],px,HELP_BAR_ROW,bar);

    char query[64]; int qlen=0; query[0]=0;
    while(1){
        for(int i=px;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
        for(int i=0;i<qlen;i++) print_char_at(query[i],px+i,HELP_BAR_ROW,bar);
        print_char_at('_',px+qlen,HELP_BAR_ROW,bar);
        uint8_t c=0; while(c==0) c=get_key();
        if(c==27){editor_draw_status_bar();return;}
        if(c=='\n') break;
        if(c=='\b'){if(qlen>0){qlen--;query[qlen]=0;}continue;}
        if(c>=32&&c<=126&&qlen<63){query[qlen++]=(char)c;query[qlen]=0;}
    }
    if(qlen==0){editor_draw_status_bar();return;}
    strcpy(search_query,query);

    search_collect_results();
    if(search_result_count==0){
        for(int i=0;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
        const char* msg=" Not found. Press any key...";
        for(int i=0;msg[i];i++) print_char_at(msg[i],i,HELP_BAR_ROW,bar);
        uint8_t k=0; while(k==0) k=get_key();
        editor_draw_status_bar(); return;
    }
    search_result_idx=0;
    for(int i=0;i<search_result_count;i++)
        if(search_results[i]>=current_line){search_result_idx=i;break;}
    search_active=true;
    search_goto_current();
    editor_redraw();
    search_draw_bar();

    while(1){
        uint8_t c=0; while(c==0) c=get_key();
        if(c==27){search_active=false;search_query_len=0;editor_redraw();return;}
        if(c==CHAR_ARROW_DOWN||c==CHAR_ARROW_RIGHT){
            if(search_result_count>0){
                search_result_idx=(search_result_idx+1)%search_result_count;
                search_goto_current();editor_redraw();search_draw_bar();
            }
            continue;
        }
        if(c==CHAR_ARROW_UP||c==CHAR_ARROW_LEFT){
            if(search_result_count>0){
                search_result_idx=(search_result_idx-1+search_result_count)%search_result_count;
                search_goto_current();editor_redraw();search_draw_bar();
            }
            continue;
        }
        search_active=false;search_query_len=0;editor_redraw();return;
    }
}

// ─── точка входа ─────────────────────────────────────────────────────────────
void cmd_nano(char* filename) {
    if(strlen(filename)==0){println("Usage: nano <filename>");return;}

    strcpy(current_filename,filename);
    g_lang=detect_lang(filename);
    current_line=0;current_col=0;total_lines=1;scroll_offset=0;h_scroll=0;
    is_dirty=false;last_cursor_x=0;last_cursor_y=0;
    sel_active=false;clipboard_len=0;
    search_active=false;search_query_len=0;search_query[0]=0;
    undo_top=-1;redo_top=-1;

    for(int i=0;i<EDITOR_MAX_LINES;i++) editor_buffer[i][0]=0;
    editor_load_file(filename);

    undo_top=0;redo_top=0;
    UndoEntry* e0=&undo_stack[0];
    for(int i=0;i<total_lines;i++) strcpy(e0->lines[i],editor_buffer[i]);
    e0->total_lines=total_lines;e0->cur_line=0;e0->cur_col=0;

    editor_redraw();

    while(1){
        uint8_t c=get_key();
        if(c==0) continue;

        // ── Выход ────────────────────────────────────────────────────────────
        if(c==CHAR_SHIFT_ESC) break;                    // Shift+ESC — без сохранения
        if(c==CHAR_CTRL_ESC) { editor_save_file(); break; } // Ctrl+ESC — сохранить
        if(c==27) { if(editor_exit_dialog()) break; continue; } // ESC — спросить

        // ── Правка ───────────────────────────────────────────────────────────
        if(c=='\n')            { editor_new_line();       continue; }
        if(c=='\b')            { editor_delete_char();    continue; }
        if(c==CHAR_DEL)        { editor_delete_forward(); continue; }
        if(c==CHAR_CTRL_F)     { editor_search();         continue; }
        if(c==CHAR_CTRL_Z)     { do_undo(); editor_redraw(); continue; }
        if(c==CHAR_CTRL_SHIFT_Z){ do_redo(); editor_redraw(); continue; }
        if(c==CHAR_CTRL_C)     { editor_copy();           continue; }
        if(c==CHAR_CTRL_V)     { editor_paste();          continue; }
        if(c==CHAR_CTRL_K)     { editor_copy_line();      continue; }
        if(c==CHAR_CTRL_X)     { editor_cut();            continue; }
        if(c==CHAR_WIN_V) {                                // Win+V — история буфера обмена
            int idx=shell_clipboard_pick();
            if(idx>=0){ const char* t=clipboard_get(idx); editor_paste_text(t,(int)strlen(t)); }
            else editor_redraw();
            continue;
        }
        if(c==CHAR_CTRL_SHIFT_A){ editor_cut_line();      continue; }
        if(c==CHAR_CTRL_SHIFT_X){ editor_goto_dialog();   continue; }
        if(c==CHAR_CTRL_A)     { editor_select_all();     continue; }
        if(c==CHAR_CTRL_BACKSPACE){ editor_delete_word_back(); continue; }
        if(c==CHAR_CTRL_DEL)   { editor_delete_word_fwd(); continue; }
        if(c==CHAR_CTRL_H)     { editor_show_help();      continue; }

        // ── Tab: автодополнение слова, иначе — обычный отступ ─────────────────
        if(c==CHAR_TAB) {
            sel_active=false;
            int wstart = ac_word_start(current_line, current_col);
            int plen = current_col-wstart;

            if(plen==0) {
                ac_index=-1; ac_count=0;
                for(int k=0;k<4;k++) editor_insert_char(' ');
                continue;
            }

            bool same_spot = (ac_index>=0 && ac_line==current_line &&
                               ac_start_col==wstart && ac_prefix_len==plen);
            if(!same_spot) {
                char prefix[AC_MAX_WORD];
                int pl=plen; if(pl>=AC_MAX_WORD) pl=AC_MAX_WORD-1;
                for(int k=0;k<pl;k++) prefix[k]=editor_buffer[current_line][wstart+k];
                prefix[pl]=0;
                ac_build(prefix, pl);
                ac_line=current_line; ac_start_col=wstart; ac_prefix_len=plen;
                ac_index = ac_count>0 ? 0 : -1;
            } else {
                // Повторный Tab на том же месте: стираем предыдущую подстановку
                // и переходим к следующему варианту по кругу.
                char* ln=editor_buffer[current_line];
                int len=strlen(ln);
                for(int i=0;i<=len-current_col;i++) ln[wstart+plen+i]=ln[current_col+i];
                current_col=wstart+plen;
                ac_index = (ac_count>0) ? (ac_index+1)%ac_count : -1;
            }

            if(ac_index>=0) {
                undo_save();
                const char* cand=ac_candidates[ac_index];
                int clen=strlen(cand);
                int suffix_len=clen-plen;
                char* ln=editor_buffer[current_line];
                int len=strlen(ln);
                if(len+suffix_len<=EDITOR_LINE_WIDTH){
                    for(int i=len;i>=current_col;i--) ln[i+suffix_len]=ln[i];
                    for(int k=0;k<suffix_len;k++) ln[current_col+k]=cand[plen+k];
                    current_col+=suffix_len;
                    is_dirty=true;
                }
                editor_redraw();

                // Подсказка "N/M: слово" в нижней строке — временно поверх обычной справки
                uint8_t bar=bar_attr();
                for(int i=0;i<80;i++) print_char_at(' ',i,HELP_BAR_ROW,bar);
                char msg[80]; int ml=0;
                const char* pfx=" Autocomplete ("; for(;pfx[ml];ml++) msg[ml]=pfx[ml];
                int a1=ac_index+1;
                if(a1>=10) msg[ml++]=(char)('0'+a1/10);
                msg[ml++]=(char)('0'+a1%10);
                msg[ml++]='/';
                if(ac_count>=10) msg[ml++]=(char)('0'+ac_count/10);
                msg[ml++]=(char)('0'+ac_count%10);
                msg[ml++]=')'; msg[ml++]=':'; msg[ml++]=' ';
                for(int k=0; cand[k] && ml<78; k++) msg[ml++]=cand[k];
                const char* hint=" [Tab -> next]"; for(int k=0;hint[k]&&ml<79;k++) msg[ml++]=hint[k];
                msg[ml]=0;
                for(int i=0;msg[i] && i<80;i++) print_char_at(msg[i], i, HELP_BAR_ROW, bar);
            } else {
                // Кандидатов не нашлось — Tab не должен "проглатываться" молча,
                // вставляем обычный отступ, как и на пустом месте.
                for(int k=0;k<4;k++) editor_insert_char(' ');
            }
            continue;
        }

        // ── Навигация ─────────────────────────────────────────────────────────
        if(c==CHAR_ARROW_UP||c==CHAR_SHIFT_UP) {
            bool sh=(c==CHAR_SHIFT_UP);
            if(sh) sel_start();
            else   sel_active=false;
            if(current_line>0) current_line--;
            int len=strlen(editor_buffer[current_line]);
            if(current_col>len) current_col=len;
            if(sh) sel_update_end();
            editor_redraw(); continue;
        }
        if(c==CHAR_ARROW_DOWN||c==CHAR_SHIFT_DOWN) {
            bool sh=(c==CHAR_SHIFT_DOWN);
            if(sh) sel_start();
            else   sel_active=false;
            if(current_line<total_lines-1) current_line++;
            int len=strlen(editor_buffer[current_line]);
            if(current_col>len) current_col=len;
            if(sh) sel_update_end();
            editor_redraw(); continue;
        }
        if(c==CHAR_ARROW_LEFT||c==CHAR_SHIFT_LEFT) {
            bool sh=(c==CHAR_SHIFT_LEFT);
            if(sh) sel_start();
            else   sel_active=false;
            if(current_col>0) current_col--;
            if(sh) sel_update_end();
            editor_redraw(); continue;
        }
        if(c==CHAR_CTRL_LEFT) {
            sel_active=false;
            editor_word_left();
            editor_redraw(); continue;
        }
        if(c==CHAR_CTRL_SHIFT_LEFT) {
            sel_start();
            editor_word_left();
            sel_update_end();
            editor_redraw(); continue;
        }
        if(c==CHAR_ARROW_RIGHT||c==CHAR_SHIFT_RIGHT) {
            bool sh=(c==CHAR_SHIFT_RIGHT);
            if(sh) sel_start();
            else   sel_active=false;
            int len=strlen(editor_buffer[current_line]);
            if(current_col<len) current_col++;
            if(sh) sel_update_end();
            editor_redraw(); continue;
        }
        if(c==CHAR_CTRL_RIGHT) {
            sel_active=false;
            editor_word_right();
            editor_redraw(); continue;
        }
        if(c==CHAR_CTRL_SHIFT_RIGHT) {
            sel_start();
            editor_word_right();
            sel_update_end();
            editor_redraw(); continue;
        }

        // ── Обычный символ ───────────────────────────────────────────────────
        if(c>=32&&c<=126){ sel_active=false; editor_insert_char((char)c); }
    }

    clear_block_cursor(last_cursor_x,last_cursor_y);
    uint8_t attr=text_attr();
    for(int i=0;i<80*25;i++) video_memory[i]=(uint16_t)((attr<<8)|' ');
    cursor_pos=80;
    shell_init_status_bar();
    println("Exited nano.");
}