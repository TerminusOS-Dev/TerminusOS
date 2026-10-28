// ═══════════════════════════════════════════════════════════════════════════
//  GITE — графический текстовый редактор для TerminusOS (режим 640×480)
//
//  Логика правки (буфер, undo/redo, выделение, clipboard, FAT32 load/save,
//  подсветка синтаксиса) взята из cmd_nano.cpp. Всё остальное — собственный
//  GUI поверх vga_gfx + gui_widgets.
//
//  ВАЖНО про цвета: режим 640×480 в этой ОС — 16-цветный (4-bit planar),
//  gfx_put_pixel берёт только младшие 4 бита индекса. Поэтому вся тема
//  построена ТОЛЬКО из слотов 0..15, переназначенных через gfx_set_palette.
//  Слоты 0 и 15 оставлены чёрным и белым — иначе сломается спрайт курсора
//  мыши (белая заливка + чёрный контур). Теней нет сознательно: на 16 цветах
//  полупрозрачных теней не бывает, а «псевдотени» дают визуальный мусор.
//
//  Все строки интерфейса на английском — gfx-шрифт 8×8 использует аппаратную
//  кодовую страницу VGA (CP437), в которой нет кириллицы.
// ═══════════════════════════════════════════════════════════════════════════

#include "cmd_gite.h"
#include "../drivers/vga_gfx.h"
#include "../drivers/gui_widgets.h"
#include "../drivers/fat32.h"
#include "../drivers/disk.h"
#include "../drivers/rtc.h"
#include "../kernel/keyboard.h"
#include "../kernel/mouse.h"
#include "../lib/string.h"
#include "../lib/screen.h"
#include "../lib/clipboard.h"
#include "cmd_registry.h"

// ─── размеры буферов ─────────────────────────────────────────────────────────
#define EDITOR_MAX_LINES    1024
#define EDITOR_LINE_WIDTH   256
// undo: раньше каждая правка хранила ПОЛНЫЙ снимок буфера (~257 КБ на слот —
// это и было "перебором по памяти"). Теперь запись undo хранит только
// небольшое окно строк вокруг места правки (см. UNDO_SPAN) — экономия в
// десятки раз при той же глубине истории.
#define UNDO_MAX 24
#define UNDO_SPAN 40   // строк до/после в одной записи — с запасом покрывает
                        // обычные правки и вставки/вырезания разумного размера.
                        // Правка, задевающая больше строк (огромный paste/cut,
                        // Replace All на весь файл), в историю undo осознанно
                        // не попадает — экономия памяти важнее редкого кейса.

// ─── геометрия интерфейса (пересчитывается под текущее разрешение) ──────────
#define TITLE_H   18    // тайтлбар
#define TOOL_H    26    // тулбар
#define TAB_H     20    // панель вкладок
#define STATUS_H  18    // статус-бар
#define SBW       10    // толщина скроллбаров
#define GUTTER_W  36    // колонка номеров строк (точка@1, маркер@3-4, номер@6)

static int W = 640, H = 480;
static int TX, TY, text_w, text_h, vis_cols, vis_rows;
static int GX;   // левый край гаттера (0, либо после боковой панели файлов)
#define SIDEBAR_W 150
static bool sidebar_open = true;
#define TERM_H 140
static bool term_open = false;

// ─── палитра "Dusk" на 16 слотах ─────────────────────────────────────────────
// Значения enum = физические слоты 0..15. Несколько имён делят слот по смыслу
// (дубликаты значений в enum разрешены). 0 = чёрный, 15 = белый (курсор мыши).
enum GN_C : uint8_t {
    C_BG = 0, C_LINE_CUR = 1, C_PANEL = 2, C_PANEL_HI = 3, C_BORDER = 4, C_BORDER_HI = 5,
    C_BTN = 2, C_BTN_HOVER = 3, C_BTN_ACTIVE = 4, C_BTN_TEXT = 6,
    C_TEXT = 6, C_TEXT_DIM = 7,
    C_GUTTER_BG = 2, C_GUTTER_NUM = 7, C_GUTTER_CUR = 8,
    C_ACCENT = 8, C_ACCENT_DIM = 9, C_CURSOR = 13,
    C_SEL = 10, C_SEL_TEXT = 6,
    C_KW = 11, C_STR = 12, C_CMT = 7, C_PRE = 9, C_NUM = 8, C_BRK = 11,
    C_MATCH_BG = 9, C_MATCH_CUR = 8, C_MATCH_FG = 6, C_MATCH_CUR_FG = 0,
    C_SCROLL_TRK = 0, C_SCROLL_TH = 3, C_SCROLL_HOV = 5,
    C_STATUS_BG = 2, C_STATUS_BLK = 3, C_STATUS_TXT = 7,
    C_TITLE_BG = 2, C_TITLE_TXT = 6,
    C_SEARCH_BG = 1, C_FIELD_BG = 0,
    C_SHADOW = 0, C_DIM = 2, C_DANGER = 14,
};

static void gn_setup_palette() {
    // idx  r   g   b   (0..63)   роль
    gfx_set_palette( 0,  0,  0,  0);  // #000000 фон / контур курсора мыши
    gfx_set_palette( 1, 10, 13, 19);  // #28344c текущая строка / панель поиска
    gfx_set_palette( 2, 12, 15, 21);  // #303c54 панели (тайтл/тулбар/статус/гаттер)
    gfx_set_palette( 3, 19, 24, 34);  // #4c6088 свет панели / hover / ползунок
    gfx_set_palette( 4, 28, 34, 48);  // #7088c0 рамки
    gfx_set_palette( 5, 38, 46, 62);  // #98b8f8 активные рамки / hover ползунка
    gfx_set_palette( 6, 49, 53, 60);  // #c4d4f0 основной светлый текст
    gfx_set_palette( 7, 31, 37, 49);  // #7c94c4 приглушённый / номера / комментарии
    gfx_set_palette( 8, 63, 45, 14);  // #ffb438 янтарный акцент / числа / текущее совпадение
    gfx_set_palette( 9, 44, 31, 11);  // #b07c2c тусклый янтарь / препроцессор / совпадения
    gfx_set_palette(10, 20, 32, 54);  // #5080d8 выделение
    gfx_set_palette(11, 40, 52, 63);  // #a0d0ff ключевые слова / скобки
    gfx_set_palette(12, 43, 55, 37);  // #acdc94 строки
    gfx_set_palette(13, 63, 53, 20);  // #ffd450 каретка (яркий янтарь)
    gfx_set_palette(14, 61, 29, 27);  // #f4746c опасный / красный
    gfx_set_palette(15, 63, 63, 63);  // #ffffff белый (заливка курсора мыши)
}

// ─── пиксельные иконки 8×8 ───────────────────────────────────────────────────
static const char* const ICON_LOGO[8] = {
    "........",
    "..####..",
    ".##.....",
    ".#......",
    ".#.###..",
    ".#...#..",
    ".#####..",
    "........"
};
static const char* const ICON_SAVE[8] = {
    ".#####..",".#...#..",".#.###..",".#.#.#..",
    ".#.###..",".#....#.",".#.##.#.",".######."};
static const char* const ICON_OPEN[8] = {   // папка (Open / загрузка файла)
    ".####...","#######.","########","########",
    "########","########",".######.","........"};
static const char* const ICON_SAVEAS[8] = { // дискета + стрелка вправо (Save as)
    ".###....","##.##...","#...#.#.","#.#.#..#",
    "#...####","#...#..#","#####.#.","........"};
static const char* const ICON_UNDO[8] = {
    "........","..#.....",".#......","########",
    ".#......","..#.....","........","........"};
static const char* const ICON_REDO[8] = {
    "........",".....#..","......#.","########",
    "......#.",".....#..","........","........"};
static const char* const ICON_CUT[8] = {
    "........",".#....#.","..#..#..","...##...",
    "..#..#..",".#....#.","#......#","........"};
static const char* const ICON_COPY[8] = {
    "..####..","..#..#..","..#..#..",".####.#.",
    ".#..#.#.",".#..#.#.",".####.#.","........"};
static const char* const ICON_PASTE[8] = {
    "..###...",".#####..",".#...#..",".#.#.#..",
    ".#...#..",".#.#.#..",".#####..","........"};
static const char* const ICON_FIND[8] = {
    "..###...",".#...#..",".#...#..","..###...",
    ".....#..","......#.",".......#","........"};
static const char* const ICON_GOTO[8] = {
    "...#....","...#....",".#####..","..###...",
    "...#....","........",".######.","........"};
static const char* const ICON_HELP[8] = {
    "..###...",".#...#..",".....#..","....#...",
    "...#....","........","...#....","........"};
static const char* const ICON_EXIT[8] = {
    "...#....","..#.#...",".#...#..",".#...#..",
    ".#...#..","..###...","........","........"};
static const char* const ICON_CLOSE[8] = {
    "........",".#...#..","..#.#...","...#....",
    "..#.#...",".#...#..","........","........"};
static const char* const ICON_UP[8] = {
    "........","...#....","..###...",".#####..",
    "...#....","...#....","........","........"};
static const char* const ICON_DOWN[8] = {
    "........","...#....","...#....",".#####..",
    "..###...","...#....","........","........"};

// ─── ID виджетов ─────────────────────────────────────────────────────────────
enum {
    ID_SAVE = 1, ID_OPEN, ID_SAVEAS, ID_UNDO, ID_REDO, ID_CUT, ID_COPY, ID_PASTE,
    ID_FIND, ID_GOTO, ID_HELP, ID_EXIT,
    ID_SRCH_PREV, ID_SRCH_NEXT, ID_SRCH_CLOSE, ID_SRCH_REPLACE, ID_SRCH_REPLACE_ALL, ID_SRCH_ALLTABS, ID_TAB_NEW,
    ID_SIDEBAR_UP, ID_SIDEBAR_DOWN, ID_SIDEBAR_TOGGLE, ID_TERM_TOGGLE, ID_TAB_SCROLL_L, ID_TAB_SCROLL_R,
    ID_SIDEBAR_NEWFILE, ID_SIDEBAR_NEWDIR,
    ID_GOTO_OK, ID_GOTO_CANCEL,
    ID_EXIT_SAVE, ID_EXIT_DISCARD, ID_EXIT_CANCEL, ID_HELP_CLOSE,
    ID_DLG_UP, ID_DLG_OK, ID_DLG_CANCEL,
};

// ─── модальные окна ──────────────────────────────────────────────────────────
enum { MODAL_NONE = 0, MODAL_EXIT, MODAL_GOTO, MODAL_HELP };
static int modal = MODAL_NONE;

// ─── буфер редактора (как в cmd_nano) ────────────────────────────────────────
// Запись undo хранит не весь буфер, а окно строк [start_line, start_line+count)
// в состоянии "до" и "после" правки, плюс позицию курсора до/после. undo/redo
// сдвигают "хвост" буфера (строки после окна) и подставляют нужную сторону.
struct UndoEntry {
    int  start_line;
    int  before_count, after_count;     // сколько строк в before[]/after[]
    int  total_before, total_after;     // total_lines до/после правки
    int  cur_line_before, cur_col_before, cur_line_after, cur_col_after;
    char before[UNDO_SPAN][EDITOR_LINE_WIDTH + 1];
    char after [UNDO_SPAN][EDITOR_LINE_WIDTH + 1];
};
static UndoEntry undo_stack[UNDO_MAX];
static int undo_top = -1, redo_top = -1;
static UndoEntry undo_staging;   // копится между undo_save() и undo_finalize()
static bool undo_pending = false;

static char editor_buffer[EDITOR_MAX_LINES][EDITOR_LINE_WIDTH + 1];
static int  current_line = 0, current_col = 0;
static int  total_lines = 1, scroll_offset = 0, h_scroll = 0;
static char current_filename[64] = {0};
static bool is_dirty = false;

// выделение + clipboard
static bool sel_active = false;
static int  sel_line_start = 0, sel_col_start = 0, sel_line_end = 0, sel_col_end = 0;
#define CLIPBOARD_SIZE (EDITOR_MAX_LINES * (EDITOR_LINE_WIDTH + 1))
static char clipboard[CLIPBOARD_SIZE] = {0};
static int  clipboard_len = 0;

// поиск
static char search_query[64] = {0};
static char search_replace[64] = {0};
static bool search_open = false, search_active = false, search_focused = false;
static bool replace_focused = false;
static int  search_caret = 0, replace_caret = 0;
#define SEARCH_MAX_RESULTS 4096
static int  search_result_line[SEARCH_MAX_RESULTS];
static int  search_result_col[SEARCH_MAX_RESULTS];
static int  search_result_count = 0, search_result_idx = 0, search_query_len = 0;
static int  search_cur_col = 0, search_cur_line = 0;
static uint8_t line_has_match[EDITOR_MAX_LINES];

// автодополнение
#define AC_MAX_CANDIDATES 64
#define AC_MAX_WORD 40
static char ac_candidates[AC_MAX_CANDIDATES][AC_MAX_WORD];
static int  ac_count = 0, ac_index = -1, ac_win = 0;
static bool ac_open = false;
static int  ac_line = -1, ac_start_col = -1, ac_prefix_len = 0;

// goto-диалог
static char goto_buf[16] = {0};
static int  goto_caret = 0;

// ─── состояние GUI ───────────────────────────────────────────────────────────
static bool g_running = true;
static int  frame = 0;
static int  blink_hold = 0;                 // после ввода каретка не мигает
static char clock_str[9] = "--:--:--";
static char msg_buf[64] = {0};              // сообщение в статус-баре
static int  msg_timer = 0;
static char tip_buf[80] = {0};              // hover-подсказка (живёт 1 кадр)
static int  g_pressed_id = -1;              // зажатая кнопка
static bool selecting = false;              // протяжка выделения мышью
static int  last_click_frame = -99, last_mx = 0, last_my = 0;
static int  v_drag = 0, v_grab = 0;         // перетаскивание ползунков
static int  h_drag = 0, h_grab = 0;
static bool popup_pressed = false;
static int  prev_cursor_line = -1, prev_cursor_col = -1;

// ─── контекстное меню по ПКМ ─────────────────────────────────────────────────
// Дублирует функции тулбара выпадающим списком с надписями (удобнее мышью,
// чем тянуться в тулбар). Рисуется в стиле GITE на цветах C_*, а не готовым
// gui_context_menu (тот использует стандартную VGA-палитру, которая в теме
// Dusk переназначена и дала бы чужеродные цвета).
enum {
    CTX_SAVE=0, CTX_UNDO, CTX_REDO, CTX_SEP1,
    CTX_CUT, CTX_COPY, CTX_PASTE, CTX_SEP2,
    CTX_FIND, CTX_GOTO, CTX_SELALL, CTX_SEP3,
    CTX_HELP, CTX_EXIT, CTX_OPEN, CTX_SAVEAS, CTX_COUNT
};
static const char* const CTX_LABEL[CTX_COUNT] = {
    "Save","Undo","Redo","---",
    "Cut","Copy","Paste","---",
    "Find","Go to line","Select all","---",
    "Help","Exit",
    "Open...","Save as..."
};
static const char* const CTX_HINT[CTX_COUNT] = {   // хоткей справа (подсказка)
    "^ESC","^Z","^Y","",
    "^X","^C","^V","",
    "^F","^S-X","^A","",
    "^H","ESC",
    "",""
};
static bool ctx_open    = false;
static int  ctx_x = 0, ctx_y = 0;       // левый-верхний угол
static int  ctx_w = 0, ctx_h = 0;       // размеры (считаются при открытии)
static int  ctx_hover   = -1;           // пункт под курсором (считается в отрисовке)
static bool ctx_lmb_was = false;        // свой фронт ЛКМ (gui_* даёт только общий)
static bool ctx_rmb_was = false;        // свой фронт ПКМ
#define CTX_ITEM_H   14
#define CTX_PAD_X    8
#define CTX_ROW_SEP(i) (CTX_LABEL[i][0]=='-' && CTX_LABEL[i][1]=='-')

extern uint8_t sector_buffer[512];

// ─── подсветка синтаксиса (из cmd_nano) ──────────────────────────────────────
enum EditorLang : uint8_t { LANG_NONE, LANG_C, LANG_ASM };
enum HLClass    : uint8_t { HL_NONE, HL_KEYWORD, HL_STRING, HL_COMMENT, HL_PREPROC, HL_BRACKET, HL_NUMBER };
static EditorLang g_lang = LANG_NONE;
static uint8_t g_hlclass[EDITOR_LINE_WIDTH];

// ─── вкладки (несколько открытых файлов) ─────────────────────────────────────
// Активная вкладка живёт прямо в глобальных editor_buffer/current_line/... —
// остальные хранятся здесь в свёрнутом виде. При переключении вкладки текущее
// состояние сохраняется в tabs[active_tab], затем состояние новой вкладки
// копируется обратно в глобальные переменные. Undo/redo и поиск между
// вкладками не переносятся (каждая вкладка начинает "с чистого листа" по
// undo), иначе стек истории пришлось бы дублировать на каждую вкладку —
// слишком дорого по памяти (по 16 полных снимков буфера на вкладку).
#define MAX_TABS 20
struct EditorTab {
    bool used;
    char filename[64];
    char lines[EDITOR_MAX_LINES][EDITOR_LINE_WIDTH+1];
    int  total_lines, current_line, current_col, scroll_offset, h_scroll;
    bool is_dirty;
    EditorLang lang;
};
static EditorTab tabs[MAX_TABS];
static int tab_count = 0;
static int active_tab = 0;

static void tab_save_active(){
    if(active_tab<0||active_tab>=tab_count) return;
    EditorTab* t=&tabs[active_tab];
    for(int i=0;i<total_lines;i++) strcpy(t->lines[i],editor_buffer[i]);
    t->total_lines=total_lines;
    t->current_line=current_line; t->current_col=current_col;
    t->scroll_offset=scroll_offset; t->h_scroll=h_scroll;
    t->is_dirty=is_dirty; t->lang=g_lang;
    strcpy(t->filename,current_filename);
}
// Загружает вкладку idx в глобальные переменные редактора. Undo/поиск/автодополнение
// сбрасываются — это отдельная сессия для вновь открытой вкладки.
static void tab_load(int idx){
    EditorTab* t=&tabs[idx];
    for(int i=0;i<t->total_lines;i++) strcpy(editor_buffer[i],t->lines[i]);
    total_lines=t->total_lines;
    current_line=t->current_line; current_col=t->current_col;
    scroll_offset=t->scroll_offset; h_scroll=t->h_scroll;
    is_dirty=t->is_dirty; g_lang=t->lang;
    strcpy(current_filename,t->filename);
    undo_top=-1; redo_top=-1; undo_pending=false;
    sel_active=false;
    search_open=false; search_active=false; search_focused=false; replace_focused=false;
    search_query[0]=0; search_query_len=0; search_result_count=0;
    ac_open=false; ac_count=0; ac_index=-1;
    ctx_open=false;
    active_tab=idx;
    prev_cursor_line=current_line; prev_cursor_col=current_col;
}
static void tab_switch(int idx){
    if(idx==active_tab || idx<0 || idx>=tab_count) return;
    tab_save_active();
    tab_load(idx);
}
// Создаёт новую пустую вкладку и делает её активной. false, если вкладок уже MAX_TABS.
static bool tab_create_new(){
    if(tab_count>=MAX_TABS){ return false; }
    tab_save_active();
    int idx=tab_count++;
    EditorTab* t=&tabs[idx];
    t->used=true; t->filename[0]=0; t->lines[0][0]=0; t->total_lines=1;
    t->current_line=0; t->current_col=0; t->scroll_offset=0; t->h_scroll=0;
    t->is_dirty=false; t->lang=LANG_NONE;
    tab_load(idx);
    return true;
}
// Закрытие вкладки всегда работает с активной вкладкой (см. tab_request_close) —
// это исключает необходимость сохранять/удалять "чужой" фоновый снимок.
static int pending_close_tab = -1; // >=0, пока модалка "сохранить перед закрытием" относится к вкладке, а не к выходу из gite целиком
static void tab_remove_active(){
    int idx=active_tab;
    for(int i=idx;i<tab_count-1;i++) tabs[i]=tabs[i+1];
    tab_count--;
    if(tab_count==0){ g_running=false; return; }
    int newidx = idx<tab_count ? idx : tab_count-1;
    tab_load(newidx);
}
// Клик по крестику вкладки: если это не активная вкладка — сперва переключаемся
// на неё (дальше работаем с живым буфером, как при обычном Exit), затем либо
// закрываем сразу, либо спрашиваем подтверждение, если есть несохранённые правки.
static void tab_request_close(int idx){
    if(idx<0||idx>=tab_count) return;
    if(idx!=active_tab) tab_switch(idx);
    if(is_dirty){ pending_close_tab=active_tab; modal=MODAL_EXIT; }
    else tab_remove_active();
}

static const char* c_keywords[] = {
    "int","char","short","long","float","double","void","unsigned","signed",
    "if","else","while","for","do","switch","case","default","break","continue",
    "return","goto","struct","union","enum","typedef","static","const","extern",
    "sizeof","volatile","register","inline","auto","class","public","private",
    "protected","new","delete","namespace","true","false","nullptr","bool",
    "uint8_t","uint16_t","uint32_t","int8_t","int16_t","int32_t","size_t",0};
static const char* asm_keywords[] = {
    "mov","add","sub","mul","div","imul","idiv","inc","dec","cmp","test",
    "jmp","je","jne","jz","jnz","jg","jl","jge","jle","ja","jb","jae","jbe",
    "call","ret","push","pop","lea","nop","int","cli","sti","hlt","xor","and",
    "or","not","shl","shr","sal","sar","loop","in","out","rep","movsb","stosb",
    "cld","std","pushf","popf","iret","db","dw","dd","dq","section","global",
    "extern","times","org","bits",0};
static const char* asm_registers[] = {
    "eax","ebx","ecx","edx","esi","edi","esp","ebp",
    "ax","bx","cx","dx","si","di","sp","bp",
    "al","bl","cl","dl","ah","bh","ch","dh",0};

static inline bool is_ident_start(char c){ return c=='_'||(c>='a'&&c<='z')||(c>='A'&&c<='Z'); }
static inline bool is_ident_char(char c){ return is_ident_start(c)||(c>='0'&&c<='9'); }

static bool word_eq(const char* line,int start,int len,const char* kw){
    int i=0;
    for(;i<len;i++){ if(kw[i]==0 || line[start+i]!=kw[i]) return false; }
    return kw[i]==0;
}
static bool ident_in_list(const char* line,int start,int len,const char** list){
    for(int k=0; list[k]; k++) if(word_eq(line,start,len,list[k])) return true;
    return false;
}
static bool ends_with(const char* s, const char* ext){
    int len=strlen(s), el=strlen(ext);
    if(len<el) return false;
    for(int i=0;i<el;i++) if(s[len-el+i]!=ext[i]) return false;
    return true;
}
static EditorLang detect_lang(const char* filename){
    if(ends_with(filename,".c")||ends_with(filename,".h")||ends_with(filename,".cpp")||
       ends_with(filename,".hpp")||ends_with(filename,".cc")) return LANG_C;
    if(ends_with(filename,".asm")||ends_with(filename,".s")) return LANG_ASM;
    return LANG_NONE;
}

// Разбор строки на классы подсветки.
static void classify_line(const char* line, EditorLang lang){
    int len=strlen(line);
    for(int i=0;i<len;i++) g_hlclass[i]=HL_NONE;
    if(lang==LANG_NONE) return;
    int i=0;
    while(i<len){
        char ch=line[i];
        if(lang==LANG_C && ch=='/' && i+1<len && line[i+1]=='/'){
            for(int j=i;j<len;j++) g_hlclass[j]=HL_COMMENT; break; }
        if(lang==LANG_ASM && ch==';'){
            for(int j=i;j<len;j++) g_hlclass[j]=HL_COMMENT; break; }
        if(lang==LANG_C && ch=='#' && i==0){
            for(int j=i;j<len;j++) g_hlclass[j]=HL_PREPROC; break; }
        if(ch=='"' || ch=='\''){
            char q=ch; int j=i; g_hlclass[j]=HL_STRING; j++;
            while(j<len && line[j]!=q){ g_hlclass[j]=HL_STRING; j++; }
            if(j<len){ g_hlclass[j]=HL_STRING; j++; }
            i=j; continue; }
        if(ch=='('||ch==')'||ch=='{'||ch=='}'||ch=='['||ch==']'){
            g_hlclass[i]=HL_BRACKET; i++; continue; }
        if(ch>='0'&&ch<='9'){
            int j=i;
            while(j<len && ((line[j]>='0'&&line[j]<='9')||line[j]=='x'||line[j]=='X'||
                   (line[j]>='a'&&line[j]<='f')||(line[j]>='A'&&line[j]<='F'))){ g_hlclass[j]=HL_NUMBER; j++; }
            i=j; continue; }
        if(is_ident_start(ch)){
            int j=i; while(j<len && is_ident_char(line[j])) j++;
            int wlen=j-i;
            bool kw = (lang==LANG_C)
                ? ident_in_list(line,i,wlen,c_keywords)
                : (ident_in_list(line,i,wlen,asm_keywords)||ident_in_list(line,i,wlen,asm_registers));
            if(kw) for(int k=i;k<j;k++) g_hlclass[k]=HL_KEYWORD;
            i=j; continue; }
        i++;
    }
}
static uint8_t hl_color(uint8_t cls){
    switch(cls){
        case HL_KEYWORD: return C_KW;
        case HL_STRING:  return C_STR;
        case HL_COMMENT: return C_CMT;
        case HL_PREPROC: return C_PRE;
        case HL_BRACKET: return C_BRK;
        case HL_NUMBER:  return C_NUM;
        default:         return C_TEXT;
    }
}

// ─── мелкие хелперы ──────────────────────────────────────────────────────────
static int iabs(int v){ return v<0?-v:v; }
static void itoa_simple(int v, char* b){
    if(v<0){ *b++='-'; v=-v; }
    char t[12]; int n=0;
    if(v==0) t[n++]='0';
    while(v){ t[n++]=(char)('0'+v%10); v/=10; }
    while(n) *b++=t[--n];
    *b=0;
}
static void set_msg(const char* s){
    int i=0; for(; s[i] && i<63; i++) msg_buf[i]=s[i];
    msg_buf[i]=0; msg_timer=150;
}
static void set_msg_num(const char* pre, int num, const char* post){
    char tmp[64]; int p=0;
    for(int i=0;pre[i];i++) tmp[p++]=pre[i];
    char nb[12]; itoa_simple(num,nb);
    for(int i=0;nb[i];i++) tmp[p++]=nb[i];
    for(int i=0;post[i];i++) tmp[p++]=post[i];
    tmp[p]=0; set_msg(tmp);
}
static bool caret_visible(){ return ((frame/20)&1)==0 || blink_hold>0; }

static void compute_geom(){
    W=gfx_get_width(); H=gfx_get_height();
    GX = sidebar_open ? SIDEBAR_W : 0;
    TX=GX+GUTTER_W; TY=TITLE_H+TOOL_H+TAB_H;
    text_w=W-GX-GUTTER_W-SBW; text_h=H-TY-STATUS_H-SBW-(term_open?TERM_H:0);
    vis_cols=text_w/8; vis_rows=text_h/8;
    if(vis_cols<1)vis_cols=1; if(vis_rows<1)vis_rows=1;
}

// ─── undo/redo (из cmd_nano) ─────────────────────────────────────────────────
// Вызывается ПЕРЕД правкой (как и раньше — те же call site'ы, без изменений).
// Захватывает "до"-окно строк вокруг курсора; "после" достраивает
// undo_finalize() один раз за кадр, когда правка уже применилась.
static void undo_save(){
    if(undo_pending){
        // предыдущая правка почему-то не была финализирована — не роняем стек,
        // просто отбрасываем незавершённую запись и начинаем новую.
        undo_pending=false;
    }
    undo_staging.start_line = current_line>0 ? current_line-1 : 0; // запас на join со строкой выше
    int n = total_lines - undo_staging.start_line;
    if(n<0) n=0;
    if(n>UNDO_SPAN) n=UNDO_SPAN;
    undo_staging.before_count=n;
    for(int i=0;i<n;i++) strcpy(undo_staging.before[i], editor_buffer[undo_staging.start_line+i]);
    undo_staging.total_before=total_lines;
    undo_staging.cur_line_before=current_line; undo_staging.cur_col_before=current_col;
    undo_pending=true;
}
// Вызывается один раз за кадр (главный цикл), уже после того как клавиша
// была обработана — то есть правка (если она была) уже в editor_buffer.
static void undo_finalize(){
    if(!undo_pending) return;
    undo_pending=false;
    int delta = total_lines - undo_staging.total_before;
    int after_count = undo_staging.before_count + delta;
    if(after_count<0 || after_count>UNDO_SPAN) return; // слишком крупная правка — не пишем в историю (см. UNDO_SPAN)
    for(int i=0;i<after_count;i++) strcpy(undo_staging.after[i], editor_buffer[undo_staging.start_line+i]);
    undo_staging.after_count=after_count;
    undo_staging.total_after=total_lines;
    undo_staging.cur_line_after=current_line; undo_staging.cur_col_after=current_col;
    if(undo_top>=UNDO_MAX-1){
        for(int i=0;i<UNDO_MAX-1;i++) undo_stack[i]=undo_stack[i+1];
        undo_top=UNDO_MAX-2;
    }
    undo_top++; redo_top=undo_top;
    undo_stack[undo_top]=undo_staging;
}
// forward=false — откат (after -> before), forward=true — повтор (before -> after)
static void undo_apply_entry(const UndoEntry* e, bool forward){
    int start=e->start_line;
    int old_count = forward? e->before_count : e->after_count;
    int new_count = forward? e->after_count  : e->before_count;
    int old_total = forward? e->total_before : e->total_after;
    int new_total = forward? e->total_after  : e->total_before;
    int shift = new_count - old_count;
    if(shift>0){
        for(int i=old_total-1; i>=start+old_count; i--) strcpy(editor_buffer[i+shift], editor_buffer[i]);
    } else if(shift<0){
        for(int i=start+old_count; i<old_total; i++) strcpy(editor_buffer[i+shift], editor_buffer[i]);
    }
    const char (*src)[EDITOR_LINE_WIDTH+1] = forward? e->after : e->before;
    for(int i=0;i<new_count;i++) strcpy(editor_buffer[start+i], src[i]);
    total_lines=new_total;
    current_line = forward? e->cur_line_after : e->cur_line_before;
    current_col  = forward? e->cur_col_after  : e->cur_col_before;
    is_dirty=true; sel_active=false;
}
static void do_undo(){
    undo_finalize(); // если правка была прямо перед undo — сначала зафиксируем её
    if(undo_top<0) return;
    undo_apply_entry(&undo_stack[undo_top], false);
    undo_top--;
}
static void do_redo(){
    if(undo_top+1>redo_top) return;
    undo_top++;
    undo_apply_entry(&undo_stack[undo_top], true);
}

// ─── прокрутка ───────────────────────────────────────────────────────────────
static void scroll_to_cursor(){
    if(current_line<scroll_offset) scroll_offset=current_line;
    else if(current_line>=scroll_offset+vis_rows) scroll_offset=current_line-vis_rows+1;
    if(current_col<h_scroll) h_scroll=current_col;
    else if(current_col>=h_scroll+vis_cols) h_scroll=current_col-vis_cols+1;
    if(scroll_offset<0) scroll_offset=0;
    if(h_scroll<0) h_scroll=0;
}
static void clamp_col(){
    int len=strlen(editor_buffer[current_line]);
    if(current_col>len) current_col=len;
}

// ─── выделение (из cmd_nano) ─────────────────────────────────────────────────
static void sel_normalize(int& sl,int& sc,int& el,int& ec){
    sl=sel_line_start; sc=sel_col_start; el=sel_line_end; ec=sel_col_end;
    if(sl>el || (sl==el && sc>ec)){ int tl=sl,tc=sc; sl=el; sc=ec; el=tl; ec=tc; }
}
static bool in_selection(int line,int col){
    if(!sel_active) return false;
    int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);
    if(line<sl || line>el) return false;
    if(sl==el) return col>=sc && col<ec;
    if(line==sl) return col>=sc;
    if(line==el) return col<ec;
    return true;
}

// ─── правка (из cmd_nano, без VGA-text перерисовок) ──────────────────────────
static void editor_insert_char(char c){
    if(current_col>=EDITOR_LINE_WIDTH) return;
    undo_save(); sel_active=false;
    int len=strlen(editor_buffer[current_line]);
    for(int i=len;i>current_col;i--)
        editor_buffer[current_line][i]=editor_buffer[current_line][i-1];
    editor_buffer[current_line][current_col]=c;
    editor_buffer[current_line][len+1]=0;
    current_col++; is_dirty=true;
}
// Вставляет пару open+close одним действием (одна запись в undo), курсор
// становится МЕЖДУ ними — так обычно ведут себя автозакрывающиеся скобки/кавычки.
static void editor_insert_pair(char open, char close){
    int len=strlen(editor_buffer[current_line]);
    if(len+2>=EDITOR_LINE_WIDTH){ editor_insert_char(open); return; }
    undo_save(); sel_active=false;
    for(int i=len;i>=current_col;i--)
        editor_buffer[current_line][i+2]=editor_buffer[current_line][i];
    editor_buffer[current_line][current_col]=open;
    editor_buffer[current_line][current_col+1]=close;
    current_col++;
    is_dirty=true;
}
static void editor_new_line(){
    if(total_lines>=EDITOR_MAX_LINES) return;
    undo_save(); sel_active=false;
    for(int i=total_lines;i>current_line+1;i--)
        strcpy(editor_buffer[i],editor_buffer[i-1]);
    editor_buffer[current_line+1][0]=0;
    if(current_col<(int)strlen(editor_buffer[current_line])){
        strcpy(editor_buffer[current_line+1],&editor_buffer[current_line][current_col]);
        editor_buffer[current_line][current_col]=0;
    }
    total_lines++; current_line++; current_col=0; is_dirty=true;
}
static void editor_delete_char(){
    if(current_col>0){
        undo_save(); sel_active=false;
        int len=strlen(editor_buffer[current_line]);
        for(int i=current_col-1;i<len;i++)
            editor_buffer[current_line][i]=editor_buffer[current_line][i+1];
        current_col--; is_dirty=true;
    } else if(current_line>0){
        int pl=strlen(editor_buffer[current_line-1]);
        int cl=strlen(editor_buffer[current_line]);
        if(pl+cl<EDITOR_LINE_WIDTH){
            undo_save(); sel_active=false;
            strcat(editor_buffer[current_line-1],editor_buffer[current_line]);
            for(int i=current_line;i<total_lines-1;i++)
                strcpy(editor_buffer[i],editor_buffer[i+1]);
            total_lines--; current_line--; current_col=pl; is_dirty=true;
        }
    }
}
// Backspace между только что автозакрытой пустой парой ("()", "[]", "\"\"" и т.п.)
// стирает обе стороны одним действием — иначе оставалась бы висящая закрывающая.
static bool editor_backspace_pair_match(char a, char b){
    return (a=='('&&b==')')||(a=='['&&b==']')||(a=='{'&&b=='}')||
           (a=='"'&&b=='"')||(a=='\''&&b=='\'');
}
static void editor_delete_pair(){
    undo_save(); sel_active=false;
    int len=strlen(editor_buffer[current_line]);
    for(int i=current_col-1;i+2<=len;i++)
        editor_buffer[current_line][i]=editor_buffer[current_line][i+2];
    current_col--; is_dirty=true;
}
static void editor_delete_forward(){
    int len=strlen(editor_buffer[current_line]);
    if(current_col<len){
        undo_save(); sel_active=false;
        for(int i=current_col;i<len;i++)
            editor_buffer[current_line][i]=editor_buffer[current_line][i+1];
        is_dirty=true;
    } else if(current_line<total_lines-1){
        int nl=strlen(editor_buffer[current_line+1]);
        if(len+nl<EDITOR_LINE_WIDTH){
            undo_save(); sel_active=false;
            strcat(editor_buffer[current_line],editor_buffer[current_line+1]);
            for(int i=current_line+1;i<total_lines-1;i++)
                strcpy(editor_buffer[i],editor_buffer[i+1]);
            editor_buffer[total_lines-1][0]=0;
            total_lines--; is_dirty=true;
        }
    }
}
static void editor_word_left(){
    char* ln=editor_buffer[current_line];
    int c=current_col;
    if(c==0) return;
    c--;
    while(c>0 && ln[c]==' ') c--;
    if(is_ident_char(ln[c])){ while(c>0 && is_ident_char(ln[c-1])) c--; }
    else { while(c>0 && ln[c-1]!=' ' && !is_ident_char(ln[c-1])) c--; }
    current_col=c;
}
static void editor_word_right(){
    char* ln=editor_buffer[current_line];
    int len=strlen(ln);
    int c=current_col;
    if(c>=len) return;
    while(c<len && ln[c]==' ') c++;
    if(c<len && is_ident_char(ln[c])){ while(c<len && is_ident_char(ln[c])) c++; }
    else { while(c<len && ln[c]!=' ' && !is_ident_char(ln[c])) c++; }
    current_col=c;
}
static void editor_delete_word_back(){
    if(current_col==0){ editor_delete_char(); return; }
    undo_save(); sel_active=false;
    int old_col=current_col;
    editor_word_left();
    int new_col=current_col;
    char* ln=editor_buffer[current_line];
    int len=strlen(ln);
    for(int i=0;i<=len-old_col;i++) ln[new_col+i]=ln[old_col+i];
    current_col=new_col; is_dirty=true;
}
static void editor_delete_word_fwd(){
    int len=strlen(editor_buffer[current_line]);
    if(current_col>=len){ editor_delete_forward(); return; }
    undo_save(); sel_active=false;
    int old_col=current_col;
    editor_word_right();
    int new_col=current_col;
    current_col=old_col;
    char* ln=editor_buffer[current_line];
    int len2=strlen(ln);
    for(int i=0;i<=len2-new_col;i++) ln[old_col+i]=ln[new_col+i];
    is_dirty=true;
}

// ─── clipboard (из cmd_nano) ─────────────────────────────────────────────────
static void editor_copy(){
    if(!sel_active) return;
    int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);
    clipboard_len=0;
    for(int ln=sl; ln<=el && clipboard_len<CLIPBOARD_SIZE-2; ln++){
        int from=(ln==sl)?sc:0;
        int to=(ln==el)?ec:(int)strlen(editor_buffer[ln]);
        for(int ci=from; ci<to && clipboard_len<CLIPBOARD_SIZE-2; ci++)
            clipboard[clipboard_len++]=editor_buffer[ln][ci];
        if(ln<el && clipboard_len<CLIPBOARD_SIZE-2) clipboard[clipboard_len++]='\n';
    }
    clipboard[clipboard_len]=0;
    sel_active=false;
    clipboard_push(clipboard);
    set_msg_num("Copied ",clipboard_len," chars");
}
static void editor_paste(){
    if(clipboard_len==0) return;
    undo_save();
    for(int ci=0; ci<clipboard_len; ci++){
        char c=clipboard[ci];
        if(c=='\n'){ editor_new_line(); }
        else if(current_col<EDITOR_LINE_WIDTH){
            int len=strlen(editor_buffer[current_line]);
            for(int i=len;i>current_col;i--)
                editor_buffer[current_line][i]=editor_buffer[current_line][i-1];
            editor_buffer[current_line][current_col]=c;
            editor_buffer[current_line][len+1]=0;
            current_col++;
        }
    }
    is_dirty=true; set_msg("Pasted");
}
static void editor_select_all(){
    sel_active=true;
    sel_line_start=0; sel_col_start=0;
    sel_line_end=total_lines-1; sel_col_end=strlen(editor_buffer[total_lines-1]);
    set_msg("Selected all");
}
static void editor_copy_line(){
    int len=strlen(editor_buffer[current_line]);
    clipboard_len=0;
    for(int i=0;i<len && clipboard_len<CLIPBOARD_SIZE-2;i++) clipboard[clipboard_len++]=editor_buffer[current_line][i];
    clipboard[clipboard_len++]='\n'; clipboard[clipboard_len]=0;
    clipboard_push(clipboard);
    set_msg("Line copied");
}
static void editor_cut_line(){
    undo_save(); sel_active=false;
    int len=strlen(editor_buffer[current_line]);
    clipboard_len=0;
    for(int i=0;i<len && clipboard_len<CLIPBOARD_SIZE-2;i++) clipboard[clipboard_len++]=editor_buffer[current_line][i];
    clipboard[clipboard_len++]='\n'; clipboard[clipboard_len]=0;
    clipboard_push(clipboard);
    if(total_lines>1){
        for(int i=current_line;i<total_lines-1;i++) strcpy(editor_buffer[i],editor_buffer[i+1]);
        total_lines--;
        if(current_line>=total_lines) current_line=total_lines-1;
    } else editor_buffer[0][0]=0;
    current_col=0; is_dirty=true; set_msg("Line cut");
}
static void editor_delete_selection(){
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
    current_line=sl; current_col=sc; is_dirty=true;
}
static void editor_cut(){
    if(!sel_active) return;
    int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);
    clipboard_len=0;
    for(int ln=sl; ln<=el && clipboard_len<CLIPBOARD_SIZE-2; ln++){
        int from=(ln==sl)?sc:0;
        int to=(ln==el)?ec:(int)strlen(editor_buffer[ln]);
        for(int ci=from; ci<to && clipboard_len<CLIPBOARD_SIZE-2; ci++)
            clipboard[clipboard_len++]=editor_buffer[ln][ci];
        if(ln<el && clipboard_len<CLIPBOARD_SIZE-2) clipboard[clipboard_len++]='\n';
    }
    clipboard[clipboard_len]=0;
    clipboard_push(clipboard);
    editor_delete_selection();
    set_msg_num("Cut ",clipboard_len," chars");
}

// ─── автодополнение: сбор кандидатов (из cmd_nano) ───────────────────────────
static int ac_word_start(int line,int col){
    char* ln=editor_buffer[line];
    int c=col;
    while(c>0 && is_ident_char(ln[c-1])) c--;
    return c;
}
static void ac_try_add(const char* word,int wlen,const char* prefix,int plen){
    if(wlen<=plen || wlen>=AC_MAX_WORD) return;
    for(int i=0;i<plen;i++) if(word[i]!=prefix[i]) return;
    for(int i=0;i<ac_count;i++){
        bool same=true;
        for(int k=0;k<wlen&&same;k++) if(ac_candidates[i][k]!=word[k]) same=false;
        if(same && ac_candidates[i][wlen]==0) return;
    }
    if(ac_count>=AC_MAX_CANDIDATES) return;
    for(int k=0;k<wlen;k++) ac_candidates[ac_count][k]=word[k];
    ac_candidates[ac_count][wlen]=0;
    ac_count++;
}
static void ac_build(const char* prefix,int plen){
    ac_count=0;
    if(g_lang==LANG_C){
        for(int k=0;c_keywords[k];k++) ac_try_add(c_keywords[k],strlen(c_keywords[k]),prefix,plen);
    } else if(g_lang==LANG_ASM){
        for(int k=0;asm_keywords[k];k++)  ac_try_add(asm_keywords[k],strlen(asm_keywords[k]),prefix,plen);
        for(int k=0;asm_registers[k];k++) ac_try_add(asm_registers[k],strlen(asm_registers[k]),prefix,plen);
    }
    for(int ln=0;ln<total_lines;ln++){
        char* line=editor_buffer[ln];
        int len=strlen(line); int i=0;
        while(i<len){
            if(is_ident_start(line[i])){
                int j=i; while(j<len && is_ident_char(line[j])) j++;
                ac_try_add(&line[i],j-i,prefix,plen); i=j;
            } else i++;
        }
    }
}
static void popup_rect(int* px,int* py,int* pw,int* ph);
static void ac_apply(){
    if(ac_index<0 || ac_index>=ac_count){ ac_open=false; return; }
    undo_save();
    char* ln=editor_buffer[current_line];
    int len=strlen(ln);
    int plen=current_col-ac_start_col; if(plen<0) plen=0;
    for(int i=0;i<=len-current_col;i++) ln[ac_start_col+i]=ln[current_col+i];
    len-=plen;
    const char* cand=ac_candidates[ac_index];
    int clen=strlen(cand);
    if(len+clen<=EDITOR_LINE_WIDTH){
        for(int i=len;i>=ac_start_col;i--) ln[i+clen]=ln[i];
        for(int k=0;k<clen;k++) ln[ac_start_col+k]=cand[k];
        current_col=ac_start_col+clen;
    }
    is_dirty=true; ac_open=false;
    char m[60]; int p=0;
    const char* pre="Completed: ";
    for(int i=0;pre[i];i++) m[p++]=pre[i];
    for(int i=0;cand[i]&&p<58;i++) m[p++]=cand[i];
    m[p]=0; set_msg(m);
}
static void ac_cycle(int dir){
    if(ac_count==0) return;
    ac_index=(ac_index+dir+ac_count)%ac_count;
    if(ac_index<ac_win) ac_win=ac_index;
    if(ac_index>=ac_win+8) ac_win=ac_index-7;
    if(ac_win<0) ac_win=0;
}
static void ac_refresh(){
    int wstart=ac_word_start(current_line,current_col);
    int plen=current_col-wstart;
    if(plen==0){ ac_open=false; return; }
    char prefix[AC_MAX_WORD];
    int pl=plen; if(pl>=AC_MAX_WORD) pl=AC_MAX_WORD-1;
    for(int k=0;k<pl;k++) prefix[k]=editor_buffer[current_line][wstart+k];
    prefix[pl]=0;
    ac_build(prefix,pl);
    if(ac_count==0){ ac_open=false; return; }
    ac_index=0; ac_win=0;
    ac_start_col=wstart; ac_prefix_len=plen; ac_line=current_line;
}
static void ac_after(){ if(ac_open) ac_refresh(); }
static void tab_autocomplete(){
    sel_active=false;
    int wstart=ac_word_start(current_line,current_col);
    int plen=current_col-wstart;
    if(plen==0){ for(int k=0;k<4;k++) editor_insert_char(' '); return; }
    char prefix[AC_MAX_WORD];
    int pl=plen; if(pl>=AC_MAX_WORD) pl=AC_MAX_WORD-1;
    for(int k=0;k<pl;k++) prefix[k]=editor_buffer[current_line][wstart+k];
    prefix[pl]=0;
    ac_build(prefix,pl);
    if(ac_count==0){
        for(int k=0;k<4;k++) editor_insert_char(' ');
        set_msg("No matches"); return;
    }
    ac_open=true; ac_index=0; ac_win=0;
    ac_line=current_line; ac_start_col=wstart; ac_prefix_len=plen;
}

// ─── поиск (из cmd_nano + лайв-режим) ───────────────────────────────────────
static bool search_all_tabs = false; // поиск по всем открытым вкладкам разом ("All" в панели)
static void search_collect_global(); // определена ниже, вместе с остальным состоянием кросс-вкладочного поиска
static void search_collect_results(){
    search_result_count=0;
    search_query_len=strlen(search_query);
    memset(line_has_match,0,sizeof(line_has_match));
    if(search_query_len==0) return;
    for(int ln=0;ln<total_lines;ln++){
        char* lb=editor_buffer[ln];
        int len=strlen(lb);
        for(int ci=0; ci+search_query_len<=len; ci++){
            bool match=true;
            for(int qi=0;qi<search_query_len&&match;qi++)
                if(lb[ci+qi]!=search_query[qi]) match=false;
            if(match){
                if(search_result_count<SEARCH_MAX_RESULTS){
                    search_result_line[search_result_count]=ln;
                    search_result_col[search_result_count]=ci;
                    search_result_count++;
                }
                line_has_match[ln]=1;
                ci+=search_query_len-1; // не ищем перекрывающиеся вхождения
            }
        }
    }
}
static void search_goto_current(){
    if(search_result_count==0) return;
    current_line=search_result_line[search_result_idx];
    search_cur_col=search_result_col[search_result_idx];
    search_cur_line=current_line;
    current_col=search_cur_col;
}
static void search_refresh(){
    search_collect_results();
    search_active=(search_query_len>0);
    if(search_all_tabs) search_collect_global();
    if(search_result_count>0){
        search_result_idx=0;
        for(int i=0;i<search_result_count;i++)
            if(search_result_line[i]>current_line ||
               (search_result_line[i]==current_line && search_result_col[i]>=current_col)){
                search_result_idx=i; break;
            }
        search_goto_current();
    }
}
static void search_next_global();
static void search_prev_global();
static void search_next(){
    if(search_all_tabs){ search_next_global(); return; }
    if(search_result_count==0) return;
    search_result_idx=(search_result_idx+1)%search_result_count;
    search_goto_current(); blink_hold=25;
}
static void search_prev(){
    if(search_all_tabs){ search_prev_global(); return; }
    if(search_result_count==0) return;
    search_result_idx=(search_result_idx-1+search_result_count)%search_result_count;
    search_goto_current(); blink_hold=25;
}
// ─── поиск по всем открытым вкладкам разом ("All" в панели поиска) ──────────
struct GlobalMatch{ uint8_t tab; int line, col; };
#define SEARCH_GLOBAL_MAX 2048
static GlobalMatch search_global[SEARCH_GLOBAL_MAX];
static int search_global_count = 0;
// Неактивные вкладки читаем из их собственного снимка (tabs[i]), активную — из живых editor_buffer.
static int tab_line_count(int t){ return (t==active_tab) ? total_lines : tabs[t].total_lines; }
static const char* tab_line_at(int t,int ln){ return (t==active_tab) ? editor_buffer[ln] : tabs[t].lines[ln]; }
static void search_collect_global(){
    search_global_count=0;
    if(search_query_len==0) return;
    for(int t=0;t<tab_count;t++){
        int n=tab_line_count(t);
        for(int ln=0; ln<n; ln++){
            const char* lb=tab_line_at(t,ln);
            int len=strlen(lb);
            for(int ci=0; ci+search_query_len<=len; ci++){
                bool match=true;
                for(int qi=0;qi<search_query_len&&match;qi++) if(lb[ci+qi]!=search_query[qi]) match=false;
                if(match){
                    if(search_global_count<SEARCH_GLOBAL_MAX){
                        search_global[search_global_count].tab=(uint8_t)t;
                        search_global[search_global_count].line=ln;
                        search_global[search_global_count].col=ci;
                        search_global_count++;
                    }
                    ci+=search_query_len-1;
                }
            }
        }
    }
}
// Переходит к глобальному совпадению idx, переключая вкладку при необходимости.
// tab_switch() сбрасывает состояние поиска у новой вкладки (это верно для
// обычного случая) — здесь состояние поиска сохраняем и восстанавливаем сами,
// чтобы прыжок между вкладками не закрывал панель поиска.
static void search_goto_global(int idx){
    if(idx<0||idx>=search_global_count) return;
    int t=search_global[idx].tab;
    if(t!=active_tab){
        bool sv_open=search_open, sv_active=search_active, sv_focus=search_focused, sv_all=search_all_tabs;
        char sv_q[64]; strcpy(sv_q, search_query);
        tab_switch(t);
        search_open=sv_open; search_active=sv_active; search_focused=sv_focus; search_all_tabs=sv_all;
        strcpy(search_query, sv_q); search_query_len=strlen(sv_q);
    }
    current_line=search_global[idx].line;
    current_col=search_global[idx].col;
    search_cur_line=current_line; search_cur_col=current_col;
    search_collect_results(); // локальный список для (теперь активной) вкладки — подсветка совпадений на строках
    for(int i=0;i<search_result_count;i++)
        if(search_result_line[i]==current_line && search_result_col[i]==current_col){ search_result_idx=i; break; }
}
static int search_global_find_current(){
    for(int i=0;i<search_global_count;i++)
        if(search_global[i].tab==active_tab && search_global[i].line==current_line && search_global[i].col==current_col)
            return i;
    return -1;
}
static void search_next_global(){
    if(search_global_count==0) return;
    int cur=search_global_find_current();
    int nxt=(cur+1+search_global_count)%search_global_count;
    search_goto_global(nxt); blink_hold=25;
}
static void search_prev_global(){
    if(search_global_count==0) return;
    int cur=search_global_find_current(); if(cur<0) cur=0;
    int prv=(cur-1+search_global_count)%search_global_count;
    search_goto_global(prv); blink_hold=25;
}
// Вставляет newtext на место [col,col+oldlen) в строке line. Если не влезает
// в лимит длины строки — молча не делает ничего (безопасный отказ).
static void editor_replace_at(int line,int col,int oldlen,const char* newtext){
    char* ln=editor_buffer[line];
    int len=strlen(ln);
    int newlen=strlen(newtext);
    int tail_len=len-(col+oldlen); if(tail_len<0) tail_len=0;
    if(col+newlen+tail_len>=EDITOR_LINE_WIDTH) return;
    char tail[EDITOR_LINE_WIDTH];
    for(int i=0;i<tail_len;i++) tail[i]=ln[col+oldlen+i];
    for(int i=0;i<newlen;i++) ln[col+i]=newtext[i];
    for(int i=0;i<tail_len;i++) ln[col+newlen+i]=tail[i];
    ln[col+newlen+tail_len]=0;
    is_dirty=true;
}
// Заменяет текущее найденное вхождение и переходит к следующему по порядку
// от курсора (следующая по счёту, а не следующая на этой же строке).
static void replace_current(){
    if(search_result_count==0) return;
    undo_save();
    int ln=search_result_line[search_result_idx];
    int col=search_result_col[search_result_idx];
    editor_replace_at(ln,col,search_query_len,search_replace);
    current_line=ln; current_col=col+(int)strlen(search_replace);
    search_refresh();
    blink_hold=25;
}
// Заменяет вообще все вхождения. Идём с конца документа к началу, чтобы
// замена на одной строке не сбивала колонки ещё не обработанных вхождений.
static void replace_all(){
    if(search_query_len==0) return;
    search_collect_results();
    if(search_result_count==0) return;
    undo_save();
    int count=0;
    for(int i=search_result_count-1;i>=0;i--){
        editor_replace_at(search_result_line[i], search_result_col[i], search_query_len, search_replace);
        count++;
    }
    search_refresh();
    set_msg_num("Replaced ",count," matches");
}
static void open_search(){
    search_open=true; search_focused=true; replace_focused=false;
    search_caret=strlen(search_query);
    if(search_query[0]) search_refresh();
}
static void close_search(){
    search_open=false; search_active=false; search_focused=false; replace_focused=false;
    search_query_len=0;
}

// ─── load / save (из cmd_nano, сообщения через статус-бар) ───────────────────
static void editor_load_file(const char* filename){
    FAT32_FindResult result=fat32_find_entry(filename,0x00);
    if(!result.found){ set_msg("New file"); return; }
    uint32_t size=result.entry.file_size;
    uint32_t cluster=FAT32_GET_CLUSTER(&result.entry);
    if(size==0){ set_msg("Empty file"); return; }
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
        if(c=='\n'){ editor_buffer[line][col]=0; line++; col=0; }
        else if(c=='\r'){ continue; }
        else if(col<EDITOR_LINE_WIDTH){ editor_buffer[line][col++]=c; }
    }
    if(col>0||line==0){ editor_buffer[line][col]=0; line++; }
    total_lines=line;
    char m[60]; int p=0;
    const char* pre="Loaded ";
    for(int i=0;pre[i];i++) m[p++]=pre[i];
    char nb[12]; itoa_simple((int)bytes_read,nb);
    for(int i=0;nb[i];i++) m[p++]=nb[i];
    const char* mid=" bytes, ";
    for(int i=0;mid[i];i++) m[p++]=mid[i];
    itoa_simple(total_lines,nb);
    for(int i=0;nb[i];i++) m[p++]=nb[i];
    const char* post=" lines";
    for(int i=0;post[i];i++) m[p++]=post[i];
    m[p]=0; set_msg(m);
}
static void editor_save_file(){
    if(strlen(current_filename)==0){ set_msg("No filename!"); return; }
    static char content[EDITOR_MAX_LINES*EDITOR_LINE_WIDTH];
    int pos=0;
    for(int i=0;i<total_lines;i++){
        int len=strlen(editor_buffer[i]);
        for(int j=0;j<len;j++) content[pos++]=editor_buffer[i][j];
        if(i<total_lines-1) content[pos++]='\n';
    }
    content[pos]=0;
    FAT32_FindResult existing=fat32_find_entry(current_filename,0x00);
    if(existing.found) fat32_delete_entry(current_filename,0x00);
    bool ok=fat32_create_file(current_filename,content,pos);
    if(ok){ is_dirty=false; set_msg_num("Saved ",pos," bytes"); }
    else set_msg("SAVE FAILED!");
}

// ─── goto (логика из cmd_nano, диалог — GUI) ─────────────────────────────────
static void open_goto(){
    modal=MODAL_GOTO;
    itoa_simple(current_line+1,goto_buf);
    goto_caret=strlen(goto_buf);
}
static void do_goto(){
    int want_line=0, want_col=0; bool has_col=false;
    int i=0;
    for(;goto_buf[i] && goto_buf[i]!=':';i++) want_line=want_line*10+(goto_buf[i]-'0');
    if(goto_buf[i]==':'){
        has_col=true; i++;
        for(;goto_buf[i];i++) want_col=want_col*10+(goto_buf[i]-'0');
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
        if(want_col>line_len) want_col=line_len;
        current_col=want_col;
    } else current_col=0;
    sel_active=false; modal=MODAL_NONE;
    set_msg_num("Jumped to line ",want_line,"");
}

// ─── GUI-примитивы ───────────────────────────────────────────────────────────
// ВАЖНО: draw_icon должна быть определена ДО gn_button / draw_title, которые
// её вызывают.
static void draw_icon(int x,int y,const char* const* ic,uint8_t col){
    for(int r=0;r<8;r++) for(int c=0;c<8;c++)
        if(ic[r][c]=='#') gfx_put_pixel(x+c,y+r,col);
}
// Тени выключены: на 16-цветном режиме полупрозрачности нет, а псевдотени
// дают визуальный мусор. text_shadow просто рисует текст, soft_shadow — no-op.
static void text_shadow(int x,int y,const char* s,uint8_t fg){
    gfx_draw_text(x,y,s,fg,GFX_TRANSPARENT);
}
static void soft_shadow(int x,int y,int w,int h){
    (void)x;(void)y;(void)w;(void)h;
}
static void dim_screen(){
    for(int y=0;y<H;y+=2)
        for(int x=(y>>1)&1;x<W;x+=2) gfx_put_pixel(x,y,C_DIM);
}
static void panel(int x,int y,int w,int h){
    soft_shadow(x,y,w,h);
    gfx_fill_rect(x,y,w,h,C_PANEL);
    gfx_draw_rect(x,y,w,h,C_BORDER_HI);
    gfx_draw_rect(x+1,y+1,w-2,h-2,C_BORDER);
}

// Кнопка: hover / нажатие / клик по отпусканию; tip уходит в статус-бар.
static bool gn_button(int id,int x,int y,int w,int h,
                      const char* label,const char* const* icon,
                      int accent,const char* tip){
    int mx=mouse_state.x,my=mouse_state.y;
    bool hover=gui_point_in_rect(mx,my,x,y,w,h);
    if(gui_click_edge()&&hover) g_pressed_id=id;
    bool clicked=false;
    if(gui_release_edge()){
        if(g_pressed_id==id){ if(hover) clicked=true; g_pressed_id=-1; }
    }
    bool pressed=(g_pressed_id==id)&&hover;
    uint8_t bg=pressed?C_BTN_ACTIVE:(hover?C_BTN_HOVER:C_BTN);
    gfx_fill_rect(x,y,w,h,bg);
    if(!pressed) gfx_fill_rect(x+1,y+1,w-2,1,C_PANEL_HI);
    gfx_draw_rect(x,y,w,h,hover?C_BORDER_HI:(accent?C_ACCENT_DIM:C_BORDER));
    int dy=pressed?1:0;
    uint8_t ic=accent?C_ACCENT:C_BTN_TEXT;
    if(id==ID_EXIT||id==ID_EXIT_DISCARD) ic=C_DANGER;
    if(icon) draw_icon(x+(w-8)/2,y+(h-8)/2+dy,icon,ic);
    else gfx_draw_text(x+(w-(int)strlen(label)*8)/2,y+(h-8)/2+dy,
                       label,accent?C_ACCENT:C_BTN_TEXT,bg);
    if(hover&&tip) strcpy(tip_buf,tip);
    return clicked;
}

// Поле ввода (только отрисовка; клавиши обрабатывает field_key в update-фазе)
static void draw_field(int x,int y,int w,int h,const char* buf,int caret,bool focused){
    gfx_fill_rect(x,y,w,h,C_FIELD_BG);
    gfx_draw_rect(x,y,w,h,focused?C_ACCENT:C_BORDER);
    int vis=(w-8)/8;
    int len=strlen(buf);
    int off=0;
    if(caret>vis-1) off=caret-vis+1;
    if(off>len-vis) off=len-vis;
    if(off<0) off=0;
    int n=len-off; if(n>vis) n=vis;
    for(int i=0;i<n;i++)
        gfx_draw_char(x+4+i*8,y+(h-8)/2,buf[off+i],C_TEXT,C_FIELD_BG);
    if(focused&&caret_visible())
        gfx_fill_rect(x+4+(caret-off)*8,y+2,1,h-4,C_CURSOR);
}
// Клавиатура поля ввода; true = нажат Enter
static bool field_key(uint8_t key,char* buf,int bufsize,int* caret){
    if(key==0) return false;
    if(key=='\n') return true;
    if(key=='\b'){
        if(*caret>0){
            int len=strlen(buf);
            for(int i=*caret-1;i<len;i++) buf[i]=buf[i+1];
            (*caret)--;
        }
        return false;
    }
    if(key==CHAR_ARROW_LEFT){ if(*caret>0)(*caret)--; return false; }
    if(key==CHAR_ARROW_RIGHT){ if(*caret<(int)strlen(buf))(*caret)++; return false; }
    if(key>=32&&key<=126){
        int len=strlen(buf);
        if(len<bufsize-1){
            for(int i=len;i>=*caret;i--) buf[i+1]=buf[i];
            buf[*caret]=(char)key; (*caret)++;
        }
        return false;
    }
    return false;
}

// ─── отрисовка: тайтлбар ─────────────────────────────────────────────────────
static void draw_title(){
    gfx_fill_rect(0,0,W,TITLE_H,C_TITLE_BG);
    gfx_fill_rect(0,0,W,2,C_ACCENT);                    // фирменная полоса
    draw_icon(6,5,ICON_LOGO,C_ACCENT);
    text_shadow(18,5,"GITE",C_TITLE_TXT);
    gfx_draw_text(18+44,5,"-",C_TEXT_DIM,GFX_TRANSPARENT);
    // имя файла (обрезаем до 26 символов)
    char fn[28]; int i=0;
    for(;current_filename[i]&&i<26;i++) fn[i]=current_filename[i];
    fn[i]=0;
    gfx_draw_text(18+56,5,fn,C_ACCENT,GFX_TRANSPARENT);
    if(is_dirty){                                       // пульсирующая звёздочка
        uint8_t dc=((frame/20)&1)?C_ACCENT:C_ACCENT_DIM;
        gfx_draw_text(18+56+(int)strlen(fn)*8+4,5,"*",dc,GFX_TRANSPARENT);
    }
    // справа: часы у правого края, "TerminusOS" левее с явным зазором
    // (раньше W-150 / W-70 слипались: 10 символов*8 = 80px как раз перекрывали)
    int clock_w = 8 * 8;                 // "HH:MM:SS"
    int term_w  = 10 * 8;                // "TerminusOS"
    int gap     = 16;
    int clock_x = W - 10 - clock_w;
    int term_x  = clock_x - gap - term_w;
    gfx_draw_text(term_x,5,"TerminusOS",C_TEXT_DIM,GFX_TRANSPARENT);
    gfx_draw_text(clock_x,5,clock_str,C_TEXT_DIM,GFX_TRANSPARENT);
    gfx_fill_rect(0,TITLE_H-1,W,1,C_BORDER);
}

// ─── отрисовка: тулбар ───────────────────────────────────────────────────────
static void dlg_open_at(int mode);   // определена ниже (секция файлового диалога); 1=Open, 2=Save-as
// "Open" теперь открывает файл в новой вкладке — кроме случая, когда текущая
// вкладка уже пустая и без изменений (тогда незачем плодить лишние вкладки).
static void tab_open_dialog(){
    bool blank = (current_filename[0]==0 && !is_dirty && total_lines<=1 && editor_buffer[0][0]==0);
    if(!blank){
        if(!tab_create_new()){ set_msg("Max tabs reached"); return; }
    }
    dlg_open_at(1); // 1 = DLG_OPEN
}
static void term_toggle(); // определена ниже (секция терминала)
static void tb_sep(int x){ gfx_fill_rect(x,TITLE_H+6,1,14,C_BORDER); }
static void draw_toolbar(){
    gfx_fill_rect(0,TITLE_H,W,TOOL_H,C_PANEL);
    gfx_fill_rect(0,TITLE_H+TOOL_H-1,W,1,C_BORDER);
    int y=TITLE_H+3, x=8;
    if(gn_button(ID_SIDEBAR_TOGGLE,x,y,22,20,sidebar_open?"<":">",nullptr,0,"Toggle file panel")){
        sidebar_open=!sidebar_open; compute_geom();
    }
    x+=26;
    if(gn_button(ID_TERM_TOGGLE,x,y,22,20,"$",nullptr,term_open?1:0,"Toggle terminal")){
        term_toggle();
    }
    x+=26; tb_sep(x); x+=5;
    if(gn_button(ID_SAVE,x,y,22,20,nullptr,ICON_SAVE,1,"Save  (^ESC)")) editor_save_file();
    x+=26; tb_sep(x); x+=5;
    if(gn_button(ID_OPEN,x,y,22,20,nullptr,ICON_OPEN,0,"Open file...")) tab_open_dialog();
    x+=26; tb_sep(x); x+=5;
    if(gn_button(ID_SAVEAS,x,y,22,20,nullptr,ICON_SAVEAS,0,"Save as...")) dlg_open_at(2); // 2 = DLG_SAVEAS
    x+=26; tb_sep(x); x+=5;
    if(gn_button(ID_UNDO,x,y,22,20,nullptr,ICON_UNDO,0,"Undo  (^Z)")){ do_undo(); set_msg("Undo"); }
    x+=24;
    if(gn_button(ID_REDO,x,y,22,20,nullptr,ICON_REDO,0,"Redo  (^Shift+Z)")){ do_redo(); set_msg("Redo"); }
    x+=26; tb_sep(x); x+=5;
    if(gn_button(ID_CUT,x,y,22,20,nullptr,ICON_CUT,0,"Cut  (^X)")){
        if(sel_active) editor_cut(); else editor_cut_line(); }
    x+=24;
    if(gn_button(ID_COPY,x,y,22,20,nullptr,ICON_COPY,0,"Copy  (^C)")){
        if(sel_active) editor_copy(); else editor_copy_line(); }
    x+=24;
    if(gn_button(ID_PASTE,x,y,22,20,nullptr,ICON_PASTE,0,"Paste  (^V)")) editor_paste();
    x+=26; tb_sep(x); x+=5;
    if(gn_button(ID_FIND,x,y,22,20,nullptr,ICON_FIND,0,"Find  (^F)")){
        if(search_open) close_search(); else open_search(); }
    x+=24;
    if(gn_button(ID_GOTO,x,y,22,20,nullptr,ICON_GOTO,0,"Go to line  (^Shift+X)")) open_goto();
    x+=26; tb_sep(x); x+=5;
    if(gn_button(ID_HELP,x,y,22,20,nullptr,ICON_HELP,0,"Help  (^H)")) modal=MODAL_HELP;
    x+=24;
    // справа: языковый бейдж и выход
    int bx=W-8-22;
    if(gn_button(ID_EXIT,bx,y,22,20,nullptr,ICON_EXIT,0,"Exit  (ESC)")){
        pending_close_tab=-1;
        if(is_dirty) modal=MODAL_EXIT; else g_running=false; }
    const char* lt=(g_lang==LANG_C)?"C":(g_lang==LANG_ASM)?"ASM":"TXT";
    uint8_t lc=(g_lang==LANG_C)?C_KW:(g_lang==LANG_ASM)?C_STR:C_TEXT_DIM;
    int lw=(int)strlen(lt)*8+12, lx=bx-10-lw;
    gfx_fill_rect(lx,y+2,lw,16,C_BTN);
    gfx_draw_rect(lx,y+2,lw,16,C_BORDER);
    gfx_draw_text(lx+6,y+6,lt,lc,C_BTN);
}

// ─── отрисовка: панель вкладок ───────────────────────────────────────────────
static int tab_bar_scroll = 0; // индекс первой видимой вкладки (при переполнении)
static int tab_bar_last_active = -1; // чтобы подтягивать окно прокрутки только при СМЕНЕ активной вкладки
static void draw_tab_bar(){
    int y=TITLE_H+TOOL_H;
    gfx_fill_rect(0,y,W,TAB_H,C_PANEL);
    gfx_fill_rect(0,y+TAB_H-1,W,1,C_BORDER);
    const int tabw=90;
    const int newbtn_w=20;
    int arrow_w=16;
    int avail=W-newbtn_w-4;
    int max_visible = avail/tabw; if(max_visible<1) max_visible=1;
    bool need_scroll = tab_count>max_visible;
    if(need_scroll){
        avail = W-newbtn_w-4-arrow_w*2;
        max_visible = avail/tabw; if(max_visible<1) max_visible=1;
    }
    if(active_tab!=tab_bar_last_active){
        // активную вкладку только что переключили — подтягиваем её в видимую зону.
        // Делаем это ТОЛЬКО при смене, а не каждый кадр — иначе ручной скролл
        // стрелками < > никогда не уводил бы окно дальше активной вкладки.
        if(active_tab<tab_bar_scroll) tab_bar_scroll=active_tab;
        if(active_tab>=tab_bar_scroll+max_visible) tab_bar_scroll=active_tab-max_visible+1;
        tab_bar_last_active=active_tab;
    }
    if(tab_bar_scroll>tab_count-max_visible) tab_bar_scroll=tab_count-max_visible;
    if(tab_bar_scroll<0) tab_bar_scroll=0;

    int mx=mouse_state.x,my=mouse_state.y;
    int x=0;
    if(need_scroll){
        if(gn_button(ID_TAB_SCROLL_L,x,y+2,arrow_w,TAB_H-4,"<",nullptr,0,nullptr)){
            if(tab_bar_scroll>0) tab_bar_scroll--;
        }
        x+=arrow_w;
    }
    int shown=0;
    for(int i=tab_bar_scroll;i<tab_count && shown<max_visible;i++,shown++){
        bool act=(i==active_tab);
        const char* fname = act?current_filename:tabs[i].filename;
        bool dirty = act?is_dirty:tabs[i].is_dirty;
        uint8_t bg = act?C_BG:C_PANEL;
        gfx_fill_rect(x,y,tabw,TAB_H,bg);
        if(act) gfx_fill_rect(x,y,tabw,2,C_ACCENT);
        if(x>0) gfx_fill_rect(x,y+3,1,TAB_H-6,C_BORDER);
        const char* disp = fname[0]?fname:"Untitled";
        int maxc=(tabw-26)/8; if(maxc<1) maxc=1; if(maxc>22) maxc=22;
        char label[24]; int k=0;
        for(;k<maxc && disp[k]; k++) label[k]=disp[k];
        label[k]=0;
        uint8_t fg=act?C_TEXT:C_TEXT_DIM;
        gfx_draw_text(x+8,y+6,label,fg,bg);
        if(dirty) gfx_fill_rect(x+tabw-30,y+8,4,4,C_ACCENT);
        if(gn_button(700+i,x+tabw-20,y+3,14,14,"x",nullptr,0,"Close tab")){
            tab_request_close(i); return; // состав вкладок мог измениться — доиграем на следующем кадре
        }
        if(gui_point_in_rect(mx,my,x,y,tabw-20,TAB_H) && gui_click_edge()) tab_switch(i);
        x+=tabw;
    }
    if(need_scroll){
        if(gn_button(ID_TAB_SCROLL_R,x,y+2,arrow_w,TAB_H-4,">",nullptr,0,nullptr)){
            if(tab_bar_scroll<tab_count-max_visible) tab_bar_scroll++;
        }
        x+=arrow_w;
    }
    if(tab_count<MAX_TABS){
        if(gn_button(ID_TAB_NEW,x+2,y+3,18,TAB_H-6,"+",nullptr,0,"New tab")){
            if(!tab_create_new()) set_msg("Max tabs reached");
        }
    }
}

// ─── отрисовка: текст + гаттер ───────────────────────────────────────────────
static void draw_editor_area(){
    gfx_fill_rect(GX,TY,GUTTER_W,text_h,C_GUTTER_BG);
    gfx_fill_rect(GX+GUTTER_W-1,TY,1,text_h,C_BORDER);     // разделитель
    gfx_fill_rect(TX,TY,text_w,text_h,C_BG);
    // лента текущей строки (в текстовой области)
    int crow=current_line-scroll_offset;
    if(crow>=0&&crow<vis_rows) gfx_fill_rect(TX,TY+crow*8,text_w,8,C_LINE_CUR);

    // ── ГАТТЕР рисуется БЕЗ клипа ─────────────────────────────────────────
    // (раньше клип стоял на текстовую область x>=TX и обрезал номера/маркеры,
    //  которые рисуются левее TX — поэтому номеров не было видно вообще)
        for(int row=0;row<vis_rows;row++){
        int ln=scroll_offset+row;
        if(ln>=total_lines) break;
        int n=ln+1;
        char nb[6];
        nb[0]=(n>=1000)?(char)('0'+n/1000):' ';
        nb[1]=(n>=100)?(char)('0'+(n/100)%10):' ';
        nb[2]=(n>=10)?(char)('0'+(n/10)%10):' ';
        nb[3]=(char)('0'+n%10); nb[4]=0;
        bool iscur=(ln==current_line);
        // номер ПО ЦЕНТРУ гаттера: 4 символа*8 = 32px, (40-32)/2 = 4 -> зона 4..35
        gfx_draw_text(GX+4,TY+row*8,nb,iscur?C_GUTTER_CUR:C_GUTTER_NUM,C_GUTTER_BG);
        // маркеры в левом поле [0..4) — НЕ наезжают на номер (он начинается с x=4)
        if(iscur){                                       // треугольник текущей строки @2-3
            int yy=TY+row*8;
            gfx_put_pixel(GX+2,yy+2,C_ACCENT); gfx_put_pixel(GX+2,yy+3,C_ACCENT);
            gfx_put_pixel(GX+2,yy+4,C_ACCENT); gfx_put_pixel(GX+3,yy+3,C_ACCENT);
        }
        if(search_active&&line_has_match[ln])            // точка совпадения поиска @0-1
            gfx_fill_rect(GX,TY+row*8+3,2,2,
                          (ln==search_cur_line)?C_ACCENT:C_ACCENT_DIM);
    }

    // ── ТЕКСТ рисуется с клипом по текстовой области ──────────────────────
    gfx_set_clip(TX,TY,text_w,text_h);
    for(int row=0;row<vis_rows;row++){
        int ln=scroll_offset+row;
        if(ln>=total_lines) break;
        classify_line(editor_buffer[ln],g_lang);
        int len=strlen(editor_buffer[ln]);
        int line_end=len; if(line_end>h_scroll+vis_cols) line_end=h_scroll+vis_cols;
        for(int ci=h_scroll;ci<line_end;ci++){
            char ch=editor_buffer[ln][ci];
            uint8_t fg=(g_lang!=LANG_NONE&&g_hlclass[ci]!=HL_NONE)?hl_color(g_hlclass[ci]):C_TEXT;
            uint8_t bg=(ln==current_line)?C_LINE_CUR:C_BG;
            if(in_selection(ln,ci)){ bg=C_SEL; fg=C_SEL_TEXT; }
            else if(search_active&&search_query_len>0&&ci+search_query_len<=len){
                bool match=true;
                for(int qi=0;qi<search_query_len&&match;qi++)
                    if(editor_buffer[ln][ci+qi]!=search_query[qi]) match=false;
                if(match){
                    if(ln==search_cur_line&&ci>=search_cur_col&&ci<search_cur_col+search_query_len){
                        bg=C_MATCH_CUR; fg=C_MATCH_CUR_FG;
                    } else { bg=C_MATCH_BG; fg=C_MATCH_FG; }
                }
            }
            gfx_draw_char(TX+(ci-h_scroll)*8,TY+row*8,ch,fg,bg);
        }
    }
    // рамка текущего совпадения (в текстовой области — под клипом ок)
    if(search_active&&search_result_count>0&&
       search_cur_line>=scroll_offset&&search_cur_line<scroll_offset+vis_rows&&
       search_cur_col>=h_scroll&&search_cur_col<h_scroll+vis_cols){
        gfx_draw_rect(TX+(search_cur_col-h_scroll)*8,
                      TY+(search_cur_line-scroll_offset)*8,
                      search_query_len*8,8,C_ACCENT);
    }
    gfx_clear_clip();
    // каретка (нижняя планка, мигает) — вне клипа
    if(crow>=0&&crow<vis_rows&&caret_visible()){
        int cx=TX+(current_col-h_scroll)*8;
        if(cx>=TX&&cx<TX+text_w)
            gfx_fill_rect(cx,TY+crow*8+6,8,2,C_CURSOR);
    }
}

// ─── отрисовка: скроллбары ───────────────────────────────────────────────────
static int doc_max_len(){
    int m=1;
    for(int i=0;i<total_lines;i++){ int L=strlen(editor_buffer[i]); if(L>m)m=L; }
    return m;
}
static void v_thumb(int* y,int* h){
    int total=total_lines>vis_rows?total_lines:vis_rows;
    *h=text_h*vis_rows/total; if(*h<16)*h=16; if(*h>text_h)*h=text_h;
    int maxoff=total_lines-vis_rows; if(maxoff<1)maxoff=1;
    *y=TY+((long)scroll_offset*(text_h-*h))/maxoff;
}
static void h_thumb(int* x,int* w){
    int ml=doc_max_len(); if(ml<vis_cols)ml=vis_cols;
    *w=text_w*vis_cols/ml; if(*w<16)*w=16; if(*w>text_w)*w=text_w;
    int maxoff=ml-vis_cols; if(maxoff<1)maxoff=1;
    *x=TX+((long)h_scroll*(text_w-*w))/maxoff;
}
static void draw_scrollbars(){
    int mx=mouse_state.x,my=mouse_state.y;
    // вертикальный
    gfx_fill_rect(W-SBW,TY,SBW,text_h,C_SCROLL_TRK);
    if(total_lines>vis_rows){
        int ty,th; v_thumb(&ty,&th);
        bool hov=gui_point_in_rect(mx,my,W-SBW,ty,SBW,th);
        gfx_fill_rect(W-SBW+1,ty,SBW-2,th,(v_drag||hov)?C_SCROLL_HOV:C_SCROLL_TH);
        for(int g=0;g<3;g++) gfx_fill_rect(W-SBW+3,ty+th/2-2+g*2,4,1,C_SCROLL_TRK);
    }
    // горизонтальный
    gfx_fill_rect(TX,TY+text_h,text_w,SBW,C_SCROLL_TRK);
    if(doc_max_len()>vis_cols){
        int tx,tw; h_thumb(&tx,&tw);
        bool hov=gui_point_in_rect(mx,my,tx,TY+text_h,tw,SBW);
        gfx_fill_rect(tx,TY+text_h+1,tw,SBW-2,(h_drag||hov)?C_SCROLL_HOV:C_SCROLL_TH);
        for(int g=0;g<3;g++) gfx_fill_rect(tx+tw/2-2+g*2,TY+text_h+3,1,4,C_SCROLL_TRK);
    }
    gfx_fill_rect(W-SBW,TY+text_h,SBW,SBW,C_PANEL);
    gfx_fill_rect(GX,TY+text_h,GUTTER_W,SBW,C_PANEL);
}

// ─── отрисовка: статус-бар ───────────────────────────────────────────────────
static int status_block(int x_right,const char* txt,uint8_t fg){
    int w=(int)strlen(txt)*8+12;
    int x=x_right-w, sy=H-STATUS_H;
    gfx_fill_rect(x,sy+3,w,12,C_STATUS_BLK);
    gfx_draw_rect(x,sy+3,w,12,C_BORDER);
    gfx_draw_text(x+6,sy+5,txt,fg,C_STATUS_BLK);
    return x-4;
}
static void draw_status(){
    int sy=H-STATUS_H;
    gfx_fill_rect(0,sy,W,STATUS_H,C_STATUS_BG);
    gfx_fill_rect(0,sy,W,1,C_BORDER);
    // слева: hover-подсказка или сообщение (янтарное -> тускнеет)
    const char* show=tip_buf[0]?tip_buf:(msg_timer>0?msg_buf:"");
    uint8_t mc=tip_buf[0]?C_STATUS_TXT:(msg_timer>70?C_ACCENT:C_ACCENT_DIM);
    if(show[0]) gfx_draw_text(8,sy+5,show,mc,C_STATUS_BG);
    // справа: блоки Ln/Col, Sel, размер, язык
    char b[40]; int p=0;
    const char* s1="Ln ";
    for(int i=0;s1[i];i++) b[p++]=s1[i];
    char nb[12]; itoa_simple(current_line+1,nb);
    for(int i=0;nb[i];i++) b[p++]=nb[i];
    const char* s2=", Col ";
    for(int i=0;s2[i];i++) b[p++]=s2[i];
    itoa_simple(current_col+1,nb);
    for(int i=0;nb[i];i++) b[p++]=nb[i];
    b[p]=0;
    int xr=status_block(W-6,b,C_STATUS_TXT);
    if(sel_active){
        int sl,sc,el,ec; sel_normalize(sl,sc,el,ec);
        int chars=0;
        for(int ln=sl;ln<=el;ln++){
            int from=(ln==sl)?sc:0;
            int to=(ln==el)?ec:(int)strlen(editor_buffer[ln]);
            chars+=to-from; if(ln<el)chars++;
        }
        p=0; const char* s3="Sel ";
        for(int i=0;s3[i];i++) b[p++]=s3[i];
        itoa_simple(chars,nb);
        for(int i=0;nb[i];i++) b[p++]=nb[i];
        b[p]=0;
        xr=status_block(xr,b,C_KW);
    }
    // размер документа
    int bytes=total_lines-1;
    for(int i=0;i<total_lines;i++) bytes+=strlen(editor_buffer[i]);
    p=0;
    if(bytes<1024){ itoa_simple(bytes,nb); for(int i=0;nb[i];i++)b[p++]=nb[i];
        const char* u=" B"; for(int i=0;u[i];i++)b[p++]=u[i]; }
    else{
        itoa_simple(bytes/1024,nb); for(int i=0;nb[i];i++)b[p++]=nb[i];
        b[p++]='.'; b[p++]=(char)('0'+((bytes%1024)*10)/1024);
        const char* u=" KB"; for(int i=0;u[i];i++)b[p++]=u[i];
    }
    b[p]=0;
    xr=status_block(xr,b,C_STATUS_TXT);
    const char* lt=(g_lang==LANG_C)?"C":(g_lang==LANG_ASM)?"ASM":"TXT";
    uint8_t lc=(g_lang==LANG_C)?C_KW:(g_lang==LANG_ASM)?C_STR:C_TEXT_DIM;
    status_block(xr,lt,lc);
}

// ─── отрисовка: панель поиска ────────────────────────────────────────────────
static void search_panel_rect(int* x,int* y,int* w,int* h){
    *w=372; *h=52; *x=W-*w-14; *y=TY+8;
}
static void draw_search_panel(){
    int px,py,pw,ph; search_panel_rect(&px,&py,&pw,&ph);
    soft_shadow(px,py,pw,ph);
    gfx_fill_rect(px,py,pw,ph,C_SEARCH_BG);
    gfx_draw_rect(px,py,pw,ph,C_BORDER_HI);
    gfx_draw_text(px+8,py+9,"Find",C_TEXT_DIM,C_SEARCH_BG);
    draw_field(px+40,py+4,150,18,search_query,search_caret,search_focused);
    // счётчик n/m
    char cnt[16]; int p=0;
    char nb[12];
    if(search_all_tabs){
        int cur=search_global_find_current();
        itoa_simple(search_global_count?cur+1:0,nb);
    } else {
        itoa_simple(search_result_count?search_result_idx+1:0,nb);
    }
    for(int i=0;nb[i];i++)cnt[p++]=nb[i];
    cnt[p++]='/';
    itoa_simple(search_all_tabs?search_global_count:search_result_count,nb);
    for(int i=0;nb[i];i++)cnt[p++]=nb[i];
    cnt[p]=0;
    int cur_count = search_all_tabs?search_global_count:search_result_count;
    uint8_t cc=(search_query_len>0&&cur_count==0)?C_DANGER:C_ACCENT;
    gfx_draw_text(px+200,py+9,cnt,cc,C_SEARCH_BG);
    if(gn_button(ID_SRCH_PREV,px+244,py+3,20,20,nullptr,ICON_UP,0,"Previous match")) search_prev();
    if(gn_button(ID_SRCH_NEXT,px+266,py+3,20,20,nullptr,ICON_DOWN,0,"Next match")) search_next();
    if(gn_button(ID_SRCH_ALLTABS,px+288,py+3,36,20,"Tabs",nullptr,search_all_tabs?1:0,"Search across all open tabs")){
        search_all_tabs=!search_all_tabs;
        if(search_all_tabs) search_collect_global();
        search_refresh();
    }
    if(gn_button(ID_SRCH_CLOSE,px+328,py+3,20,20,nullptr,ICON_CLOSE,0,"Close  (ESC)")) close_search();
    // вторая строка — замена
    gfx_draw_text(px+8,py+35,"With",C_TEXT_DIM,C_SEARCH_BG);
    draw_field(px+40,py+30,150,18,search_replace,replace_caret,replace_focused);
    if(gn_button(ID_SRCH_REPLACE,px+196,py+29,58,20,"Repl",nullptr,0,"Replace current match  (Enter)")) replace_current();
    if(gn_button(ID_SRCH_REPLACE_ALL,px+258,py+29,50,20,"All",nullptr,1,"Replace all matches — current tab only  (Ctrl+Enter)")) replace_all();
}

// ─── отрисовка: попап автодополнения ─────────────────────────────────────────
static void popup_rect(int* px,int* py,int* pw,int* ph){
    int shown=ac_count<8?ac_count:8;
    int wl=0;
    for(int i=0;i<shown;i++){
        int L=strlen(ac_candidates[ac_win+i]); if(L>wl)wl=L;
    }
    *pw=wl*8+20; if(*pw<90)*pw=90; if(*pw>260)*pw=260;
    *ph=shown*11+8;
    *px=TX+(current_col-h_scroll)*8;
    if(*px+*pw>W-4)*px=W-4-*pw;
    if(*px<2)*px=2;
    *py=TY+(current_line-scroll_offset)*8+10;
    if(*py+*ph>TY+text_h-2)*py-=(*ph+12);
}
static void draw_popup(){
    int px,py,pw,ph; popup_rect(&px,&py,&pw,&ph);
    int mx=mouse_state.x,my=mouse_state.y;
    soft_shadow(px,py,pw,ph);
    gfx_fill_rect(px,py,pw,ph,C_SEARCH_BG);
    gfx_draw_rect(px,py,pw,ph,C_BORDER_HI);
    gfx_fill_rect(px+1,py+1,pw-2,1,C_PANEL_HI);
    int shown=ac_count<8?ac_count:8;
    for(int i=0;i<shown;i++){
        int idx=ac_win+i;
        int iy=py+4+i*11;
        if(gui_point_in_rect(mx,my,px+2,iy,pw-4,11)) ac_index=idx;
        if(idx==ac_index){
            gfx_fill_rect(px+2,iy,pw-4,11,C_BTN_HOVER);
            gfx_fill_rect(px+2,iy,2,11,C_ACCENT);
        }
        int maxc=(pw-16)/8;
        const char* s=ac_candidates[idx];
        for(int k=0;k<maxc&&s[k];k++)
            gfx_draw_char(px+8+k*8,iy+1,s[k],
                          idx==ac_index?C_ACCENT:C_TEXT,GFX_TRANSPARENT);
    }
    if(gui_click_edge()&&gui_point_in_rect(mx,my,px,py,pw,ph)) popup_pressed=true;
    if(gui_release_edge()){
        if(popup_pressed&&gui_point_in_rect(mx,my,px,py,pw,ph)) ac_apply();
        popup_pressed=false;
    }
}

// ─── отрисовка: модальные окна ───────────────────────────────────────────────
// ═══════════════════════════════════════════════════════════════════════════
//  МОДАЛЬНЫЙ ФАЙЛОВЫЙ ДИАЛОГ Open / Save-as (проводник по FAT32 с LFN)
//  Рисует длинные имена (копит LFN как scan_dir_cluster/fat32_tab_complete).
//  Open/Save передают длинное имя напрямую: find ищет по LFN, create сам
//  пишет LFN-цепочку. Навигация Up — через запись ".." (её кластер = родитель).
//  Перед confirm выставляем current_dir_cluster = папка диалога, т.к.
//  editor_load/save ищут файл через fat32_find_entry именно в cwd.
// ═══════════════════════════════════════════════════════════════════════════
extern uint32_t current_dir_cluster;   // cwd шелла (extern можно повторять)

enum DlgMode { DLG_NONE = 0, DLG_OPEN, DLG_SAVEAS };
static int      dlg_mode = DLG_NONE;
static uint32_t dlg_dir_cluster = 2;
static bool     dlg_has_parent = false;
static uint32_t dlg_parent_cluster = 2;
#define DLG_MAX_ENTRIES 256
struct DlgEntry { char name[64]; bool is_dir; uint32_t cluster; };
static DlgEntry dlg_entries[DLG_MAX_ENTRIES];
static int  dlg_count = 0, dlg_sel = 0, dlg_scroll = 0;
static char dlg_name[64] = {0};
static int  dlg_name_caret = 0;
static bool dlg_name_focus = false;
static int  dlg_last_click = -99, dlg_last_row = -1;
static char dlg_path_stack[16][64];
static int  dlg_path_depth = 0;

static bool dlg_sb_drag = false;   // тянем ползунок скроллбара списка файлов
static int  dlg_sb_grab = 0;       // смещение захвата внутри ползунка

static const char* const ICON_FOLDER[8] = {   // папка для списка
    ".###....","#####...","#######.","########",
    "########","########",".######.","........"};

static void dlg_decode_83(const FAT32_DirEntry* e, char* out){
    int di=0;
    for(int n=0;n<8&&e->name[n]!=' ';n++) out[di++]=(char)(e->name[n]|32);
    bool has=false; for(int n=0;n<3;n++) if(e->ext[n]!=' '){has=true;break;}
    if(has){ out[di++]='.'; for(int n=0;n<3&&e->ext[n]!=' ';n++) out[di++]=(char)(e->ext[n]|32); }
    out[di]=0;
}
static bool dlg_ieq(const char* a,const char* b){
    int i=0;
    while(a[i]&&b[i]){
        char ca=a[i],cb=b[i];
        if(ca>='a'&&ca<='z')ca-=32; if(cb>='a'&&cb<='z')cb-=32;
        if(ca!=cb) return false; i++;
    }
    return a[i]==0&&b[i]==0;
}
static int dlg_cmp(const DlgEntry* a,const DlgEntry* b){
    if(a->is_dir!=b->is_dir) return a->is_dir?-1:1;
    int i=0;
    while(a->name[i]&&b->name[i]){
        char ca=a->name[i],cb=b->name[i];
        if(ca>='a'&&ca<='z')ca-=32; if(cb>='a'&&cb<='z')cb-=32;
        if(ca!=cb) return ca-cb; i++;
    }
    return (a->name[i]?1:0)-(b->name[i]?1:0);
}
// Обход dir_cluster: копит LFN, ловит ".." (родитель), пропускает "."/volume.
// Общая для модального диалога (dlg_*) и боковой панели (sidebar_*) — чтобы
// не дублировать разбор FAT32 директорий в двух местах.
static void fs_read_dir(uint32_t dir_cluster, DlgEntry* out, int* out_count, int max_count,
                        bool* has_parent, uint32_t* parent_cluster){
    *out_count=0; *has_parent=false; *parent_cluster=FAT32_ROOT_CLUSTER;
    uint32_t c=dir_cluster;
    char lfn[256]; int lfn_len=0;
    while((c&FAT32_MASK)>=2 && (c&FAT32_MASK)<0x0FFFFFF8 && *out_count<max_count){
        uint32_t base=fat32_cluster_to_lba(c);
        for(uint8_t s=0;s<FAT32_SECTORS_PER_CLUSTER && *out_count<max_count;s++){
            ata_read_sector(base+s);
            for(int i=0;i<FAT32_ENTRIES_PER_SECTOR && *out_count<max_count;i++){
                FAT32_DirEntry* e=(FAT32_DirEntry*)(sector_buffer+i*FAT32_ENTRY_SIZE);
                uint8_t first=(uint8_t)e->name[0];
                if(first==0x00) goto fs_read_dir_done;
                if(first==0xE5){ lfn_len=0; continue; }
                if(e->attributes==0x0F){
                    uint8_t* raw=(uint8_t*)e;
                    fat32_lfn_accumulate(raw, lfn, &lfn_len, sizeof(lfn));
                    continue;
                }
                if(e->name[0]=='.'&&e->name[1]=='.'&&e->name[2]==' '){
                    uint32_t pc=FAT32_GET_CLUSTER(e);
                    if(pc!=dir_cluster){ *has_parent=true; *parent_cluster=(pc<2)?FAT32_ROOT_CLUSTER:pc; }
                    lfn_len=0; continue;
                }
                if(e->name[0]=='.'){ lfn_len=0; continue; }
                if(e->attributes&0x08){ lfn_len=0; continue; }   // volume label
                DlgEntry* d=&out[*out_count];
                d->is_dir=(e->attributes&0x10)!=0;
                d->cluster=FAT32_GET_CLUSTER(e);
                char dec[13]; const char* src=(lfn_len>0)?lfn:nullptr;
                if(!src){ dlg_decode_83(e,dec); src=dec; }
                int k=0; while(src[k]&&k<63){ d->name[k]=src[k]; k++; } d->name[k]=0;
                lfn_len=0;
                (*out_count)++;
            }
        }
        uint32_t nx=fat32_get_next_cluster(c);
        if((nx&FAT32_MASK)>=(FAT32_EOC&FAT32_MASK)) break;
        c=nx;
    }
    fs_read_dir_done:
    for(int i=1;i<*out_count;i++){                       // папки вверх, затем по имени
        DlgEntry t=out[i]; int j=i-1;
        while(j>=0 && dlg_cmp(&out[j],&t)>0){ out[j+1]=out[j]; j--; }
        out[j+1]=t;
    }
}
static void dlg_read_dir(){
    fs_read_dir(dlg_dir_cluster, dlg_entries, &dlg_count, DLG_MAX_ENTRIES, &dlg_has_parent, &dlg_parent_cluster);
    dlg_sel=0; dlg_scroll=0;
}
static void dlg_build_path(char* out,int max){
    int p=0; out[p++]='/';
    for(int d=0;d<dlg_path_depth;d++){
        if(d>0&&p<max-1) out[p++]='/';
        for(int k=0;dlg_path_stack[d][k]&&p<max-2;k++) out[p++]=dlg_path_stack[d][k];
    }
    out[p]=0;
}
static void dlg_layout(int* lx,int* ly,int* lw,int* lh,int* rows,
                       int* fx,int* fy,int* fw,int* fh,int* bx,int* by){
    int pw=460, ph=300, px=(W-pw)/2, py=(H-ph)/2-10;
    *lx=px+8; *lw=pw-16; *ly=py+36;
    *by=py+ph-30; *fy=*by-24; *fx=px+8; *fw=pw-16; *fh=18;
    *lh=*fy-8-*ly; *rows=*lh/14; if(*rows<1)*rows=1;
}
static void dlg_sync_field_from_sel(){
    if(dlg_sel>=0&&dlg_sel<dlg_count){
        int k=0; while(dlg_entries[dlg_sel].name[k]&&k<63){ dlg_name[k]=dlg_entries[dlg_sel].name[k]; k++; }
        dlg_name[k]=0; dlg_name_caret=k;
    }
}
static void dlg_move_sel(int d){
    if(dlg_count==0) return;
    dlg_sel+=d; if(dlg_sel<0)dlg_sel=0; if(dlg_sel>=dlg_count)dlg_sel=dlg_count-1;
    int rows; int lx,ly,lw,lh,fx,fy,fw,fh,bx,by; dlg_layout(&lx,&ly,&lw,&lh,&rows,&fx,&fy,&fw,&fh,&bx,&by);
    if(dlg_sel<dlg_scroll) dlg_scroll=dlg_sel;
    if(dlg_sel>=dlg_scroll+rows) dlg_scroll=dlg_sel-rows+1;
    dlg_sync_field_from_sel();
}
static void dlg_reset_editor(){
    current_line=0; current_col=0; total_lines=1; scroll_offset=0; h_scroll=0;
    is_dirty=false; sel_active=false; clipboard_len=0;
    search_open=false; search_active=false; search_focused=false; replace_focused=false;
    search_query[0]=0; search_query_len=0; search_replace[0]=0; search_result_count=0;
    ac_open=false; ac_count=0; ac_index=-1;
    undo_top=-1; redo_top=-1; undo_pending=false;
    for(int i=0;i<EDITOR_MAX_LINES;i++) editor_buffer[i][0]=0;
}

// ─── боковая панель файлов (как в VS Code) ───────────────────────────────────
#define SIDEBAR_MAX_ENTRIES 128
static DlgEntry sidebar_entries[SIDEBAR_MAX_ENTRIES];
static int      sidebar_count = 0, sidebar_sel = -1, sidebar_scroll = 0;
static uint32_t sidebar_dir_cluster = 2;
static bool     sidebar_has_parent = false;
static uint32_t sidebar_parent_cluster = 2;
static char     sidebar_path_stack[16][64];
static int      sidebar_path_depth = 0;
static void sidebar_read_dir(){
    fs_read_dir(sidebar_dir_cluster, sidebar_entries, &sidebar_count, SIDEBAR_MAX_ENTRIES,
                &sidebar_has_parent, &sidebar_parent_cluster);
    sidebar_sel=-1; sidebar_scroll=0;
}
static void sidebar_init(){
    sidebar_dir_cluster = current_dir_cluster; // папка, из которой запустили gite
    sidebar_path_depth = 0;
    sidebar_read_dir();
}
static void sidebar_enter_dir(int idx){
    if(sidebar_path_depth<16){
        int k=0; while(sidebar_entries[idx].name[k]&&k<63){ sidebar_path_stack[sidebar_path_depth][k]=sidebar_entries[idx].name[k]; k++; }
        sidebar_path_stack[sidebar_path_depth][k]=0; sidebar_path_depth++;
    }
    sidebar_dir_cluster=sidebar_entries[idx].cluster;
    sidebar_read_dir();
}
static void sidebar_up(){
    if(!sidebar_has_parent) return;
    if(sidebar_path_depth>0) sidebar_path_depth--;
    sidebar_dir_cluster=sidebar_parent_cluster;
    sidebar_read_dir();
}
// Открывает выбранный файл в новой вкладке (как toolbar Open), затем сразу
// грузит его — без модального диалога, раз путь уже выбран в панели.
static void sidebar_open_file(int idx){
    if(sidebar_entries[idx].is_dir){ sidebar_enter_dir(idx); return; }
    bool blank = (current_filename[0]==0 && !is_dirty && total_lines<=1 && editor_buffer[0][0]==0);
    if(!blank && !tab_create_new()) { set_msg("Max tabs reached"); return; }
    uint32_t saved_dir=current_dir_cluster;
    current_dir_cluster=sidebar_dir_cluster;
    strcpy(current_filename, sidebar_entries[idx].name);
    g_lang=detect_lang(current_filename);
    dlg_reset_editor();
    editor_load_file(current_filename);
    current_dir_cluster=saved_dir;
    prev_cursor_line=current_line; prev_cursor_col=current_col;
    set_msg("Opened");
}
// ─── управление файлами прямо из панели: создать / переименовать / удалить ──
static int  sidebar_mode = 0;         // 0=обычно, 1=новый файл, 2=новая папка, 3=переименование, 4=подтверждение удаления
static char sidebar_name_buf[64] = {0};
static int  sidebar_name_caret = 0;
static int  sidebar_action_idx = -1;  // запись, к которой относится rename/delete
static void sidebar_start_new_file(){ sidebar_mode=1; sidebar_name_buf[0]=0; sidebar_name_caret=0; }
static void sidebar_start_new_folder(){ sidebar_mode=2; sidebar_name_buf[0]=0; sidebar_name_caret=0; }
static void sidebar_start_rename(int idx){
    sidebar_mode=3; sidebar_action_idx=idx;
    int k=0; while(sidebar_entries[idx].name[k]&&k<63){ sidebar_name_buf[k]=sidebar_entries[idx].name[k]; k++; }
    sidebar_name_buf[k]=0; sidebar_name_caret=k;
}
static void sidebar_start_delete(int idx){ sidebar_mode=4; sidebar_action_idx=idx; }
static void sidebar_cancel_action(){ sidebar_mode=0; sidebar_action_idx=-1; }
// Переименование ФАЙЛА — не через fat32_rename_entry (тот делит с mv общий путь
// освобождения/переиспользования LFN-слотов каталога, который на практике
// иногда задевает соседние записи), а как копирование под капотом: читаем
// содержимое, создаём файл с новым именем через fat32_create_file (это уже
// много раз использовалось в gite без проблем — буфер обмена, темы и т.д.),
// затем удаляем старую запись. Для папок это неприменимо (внутри могут быть
// другие файлы) — их по-прежнему переименовывает fat32_rename_entry.
#define SIDEBAR_RENAME_BUF_MAX 65536
static bool sidebar_rename_file_via_copy(const char* oldname, const char* newname){
    FAT32_FindResult res = fat32_find_entry(oldname, 0x00);
    if(!res.found) { println("Error: source not found"); return false; }
    uint32_t size = res.entry.file_size;
    static uint8_t buf[SIDEBAR_RENAME_BUF_MAX];
    if(size > SIDEBAR_RENAME_BUF_MAX) { println("Error: file too large to rename"); return false; }
    uint32_t cluster = FAT32_GET_CLUSTER(&res.entry);
    uint32_t got = 0;
    while((cluster&FAT32_MASK)>=2 && (cluster&FAT32_MASK)<(FAT32_EOC&FAT32_MASK) && got<size){
        uint32_t lba=fat32_cluster_to_lba(cluster);
        for(int s=0;s<FAT32_SECTORS_PER_CLUSTER && got<size; s++){
            ata_read_sector(lba+s);
            for(int b=0;b<FAT32_BYTES_PER_SECTOR && got<size; b++) buf[got++]=sector_buffer[b];
        }
        cluster=fat32_get_next_cluster(cluster);
    }
    if(!fat32_create_file(newname,(const char*)buf,got)) { println("Error: create failed"); return false; }
    fat32_delete_entry((char*)oldname, 0x00);
    return true;
}
// Выполняет запланированное действие через уже имеющиеся FAT32-примитивы
// (те же, что использует шелл: fat32_create_file/create_dir/rename_entry/delete_entry).
// Оборачиваем redirect_start()/stop(), чтобы поймать текстовые FAT32 ERROR:...
// сообщения (они печатаются в текстовый VGA-режим, невидимый внутри gite) и
// показать их через обычный set_msg() панели.
static void sidebar_confirm_action(){
    int mode=sidebar_mode, idx=sidebar_action_idx;
    char oldname[64]={0}; if(idx>=0) strcpy(oldname, sidebar_entries[idx].name);
    bool old_is_dir = (idx>=0) && sidebar_entries[idx].is_dir;
    uint32_t saved=current_dir_cluster;
    current_dir_cluster=sidebar_dir_cluster;
    redirect_start();
    bool ok=false; const char* okmsg="Done";
    if(mode==1 && sidebar_name_buf[0]){
        if(fat32_find_entry(sidebar_name_buf,0x00).found) println("Error: already exists");
        else { ok=fat32_create_file(sidebar_name_buf,"",0); okmsg="File created"; }
    } else if(mode==2 && sidebar_name_buf[0]){
        if(fat32_find_entry(sidebar_name_buf,0x00).found) println("Error: already exists");
        else { fat32_create_dir(sidebar_name_buf); ok=true; okmsg="Folder created"; }
    } else if(mode==3 && sidebar_name_buf[0] && idx>=0){
        if(!dlg_ieq(oldname,sidebar_name_buf) && fat32_find_entry(sidebar_name_buf,0x00).found)
            println("Error: already exists");
        else if(old_is_dir){ fat32_rename_entry(oldname, sidebar_name_buf); ok=true; okmsg="Renamed"; }
        else { ok=sidebar_rename_file_via_copy(oldname, sidebar_name_buf); okmsg="Renamed"; }
    } else if(mode==4 && idx>=0){
        uint8_t attr = old_is_dir?0x10:0x00;
        fat32_delete_entry(oldname, attr); ok=true; okmsg="Deleted";
    }
    redirect_stop();
    current_dir_cluster=saved;
    const char* out=redirect_get_buf();
    set_msg(out&&out[0] ? out : okmsg);
    (void)ok;
    sidebar_mode=0; sidebar_action_idx=-1;
    sidebar_read_dir();
}
static void draw_sidebar(){
    if(!sidebar_open) return;
    int x=0, y=TY, w=SIDEBAR_W, h=text_h+SBW;   // +SBW — закрываем и строку под гориз. скроллбаром
    gfx_fill_rect(x,y,w,h,C_PANEL);
    gfx_fill_rect(x+w-1,y,1,h,C_BORDER);
    int rowh=14;
    int mx=mouse_state.x,my=mouse_state.y;
    bool click=gui_click_edge();
    int hdr=16;
    if(sidebar_mode==0){
        if(sidebar_has_parent && gui_point_in_rect(mx,my,x,y,w-38,hdr) && click) sidebar_up();
        gfx_draw_text(x+4,y+4,sidebar_has_parent?"< ..":"/",C_TEXT_DIM,C_PANEL);
        if(gn_button(ID_SIDEBAR_NEWFILE,x+w-34,y+2,15,13,"N",nullptr,0,"New file")) sidebar_start_new_file();
        if(gn_button(ID_SIDEBAR_NEWDIR, x+w-17,y+2,15,13,"M",nullptr,0,"New folder")) sidebar_start_new_folder();
    } else if(sidebar_mode==4){
        char msg[24]; int p=0; const char* pre="Del '";
        for(int i=0;pre[i]&&p<20;i++) msg[p++]=pre[i];
        for(int i=0;sidebar_entries[sidebar_action_idx].name[i]&&p<20;i++) msg[p++]=sidebar_entries[sidebar_action_idx].name[i];
        msg[p++]='\''; msg[p]=0;
        gfx_draw_text(x+4,y+4,msg,C_DANGER,C_PANEL);
        gfx_draw_text(x+4,y+16,"Enter=yes  Esc=no",C_TEXT_DIM,C_PANEL);
        hdr=28;
    } else {
        gfx_fill_rect(x+2,y+2,w-4,14,C_FIELD_BG);
        gfx_draw_rect(x+2,y+2,w-4,14,C_BORDER);
        gfx_draw_text(x+4,y+5,sidebar_name_buf,C_TEXT,C_FIELD_BG);
        if((frame/20)%2==0) gfx_draw_text(x+4+sidebar_name_caret*8,y+5,"_",C_ACCENT,C_FIELD_BG);
        gfx_draw_text(x+4,y+18,"Enter=ok  Esc=cancel",C_TEXT_DIM,C_PANEL);
        hdr=30;
    }
    int listy=y+hdr, listh=h-hdr-(sidebar_count>0?14:0);
    int visible_rows = listh/rowh; if(visible_rows<1) visible_rows=1;
    if(sidebar_scroll>sidebar_count-visible_rows) sidebar_scroll=sidebar_count-visible_rows;
    if(sidebar_scroll<0) sidebar_scroll=0;
    int row=0;
    for(int i=sidebar_scroll;i<sidebar_count && row<visible_rows;i++,row++){
        int ry=listy+row*rowh;
        bool sel=(i==sidebar_sel);
        uint8_t rowbg = sel?C_BTN_HOVER:C_PANEL;
        if(sel) gfx_fill_rect(x,ry,w-1,rowh,rowbg);
        const char* nm=sidebar_entries[i].name;
        uint8_t fg = sidebar_entries[i].is_dir? C_ACCENT : C_TEXT;
        int maxc=(w-22-28)/8; if(maxc<1) maxc=1; if(maxc>18) maxc=18;
        char label[20]; int k=0;
        for(;k<maxc && nm[k]; k++) label[k]=nm[k];
        label[k]=0;
        gfx_draw_text(x+4,ry+3,sidebar_entries[i].is_dir?">":" ",C_TEXT_DIM,rowbg);
        gfx_draw_text(x+16,ry+3,label,fg,rowbg);
        if(gn_button(900+i*2,x+w-30,ry+1,13,12,"e",nullptr,0,"Rename")) sidebar_start_rename(i);
        if(gn_button(900+i*2+1,x+w-16,ry+1,13,12,"x",nullptr,0,"Delete")) sidebar_start_delete(i);
        if(gui_point_in_rect(mx,my,x,ry,w-30,rowh) && click){ sidebar_sel=i; sidebar_open_file(i); return; }
    }
    if(sidebar_count>visible_rows){
        int by=listy+listh;
        if(gn_button(ID_SIDEBAR_UP,x+2,by,(w-6)/2,14,nullptr,ICON_UP,0,nullptr)){
            if(sidebar_scroll>0) sidebar_scroll--;
        }
        if(gn_button(ID_SIDEBAR_DOWN,x+2+(w-6)/2+2,by,(w-6)/2,14,nullptr,ICON_DOWN,0,nullptr)){
            if(sidebar_scroll<sidebar_count-visible_rows) sidebar_scroll++;
        }
    }
}
static void update_sidebar_key(uint8_t key){
    if(key==0) return;
    if(key==27){ sidebar_cancel_action(); return; }
    if(sidebar_mode==4){
        if(key=='\n'||key=='y'||key=='Y') sidebar_confirm_action();
        else if(key=='n'||key=='N') sidebar_cancel_action();
        return;
    }
    if(key=='\n'){ sidebar_confirm_action(); return; }
    field_key(key, sidebar_name_buf, 64, &sidebar_name_caret);
}

// ─── встроенный терминал (низ окна) ──────────────────────────────────────────
// Выполняет обычные команды шелла (cmd_dispatch) прямо внутри gite: вывод
// перехватывается через уже существующий в ОС redirect_start()/redirect_stop()
// (тот же механизм, что у "echo текст > файл") и рисуется в собственном
// графическом буфере строк — панель не трогает текстовый VGA-режим 80×25,
// поэтому ничего не конфликтует с графикой gite.
//
// Чёрный список — команды, которые сами занимают экран/цикл ввода (gite,
// paint, widgets, nano, edit, image) — их повторный вызов ИЗНУТРИ gite привёл
// бы к тому, что они начали бы рисовать в невидимый текстовый режим и держать
// весь ввод клавиатуры себе, а окно gite просто "зависло" бы на вид. Плюс
// необратимые/опасные операции (reboot, shutdown, exit, fsd — форматирование
// диска, panic) — чтобы их нельзя было случайно набрать посреди правки файла.
#define TERM_HIST  200
#define TERM_COLS  78
static char term_hist[TERM_HIST][TERM_COLS+1];
static int  term_hist_count = 0;
static int  term_scroll = 0;          // 0 = внизу (последние строки)
static char term_input[64] = {0};
static int  term_input_len = 0, term_input_caret = 0;
static uint32_t term_dir_cluster = 2; // своя cwd терминала — не трогает cwd шелла/gite

static void term_push_line(const char* s){
    if(term_hist_count>=TERM_HIST){
        for(int i=0;i<TERM_HIST-1;i++) strcpy(term_hist[i],term_hist[i+1]);
        term_hist_count=TERM_HIST-1;
    }
    int k=0; while(s[k] && k<TERM_COLS){ term_hist[term_hist_count][k]=s[k]; k++; }
    term_hist[term_hist_count][k]=0;
    term_hist_count++;
    term_scroll=0;
}
static void term_feed(const char* text){
    char line[TERM_COLS+1]; int k=0;
    for(int i=0; text[i]; i++){
        char c=text[i];
        if(c=='\n'){ line[k]=0; term_push_line(line); k=0; }
        else if(c=='\r'){ /* игнор */ }
        else if(k<TERM_COLS){ line[k++]=c; }
    }
    if(k>0){ line[k]=0; term_push_line(line); }
}
static bool term_blocked(const char* cmd){
    static const char* blocked[] = {
        "gite","paint","widgets","nano","edit","image",
        "reboot","shutdown","exit","fsd","panic", nullptr
    };
    for(int i=0; blocked[i]; i++) if(dlg_ieq(cmd,blocked[i])) return true;
    return false;
}
static void term_toggle(){
    term_open=!term_open;
    if(term_open && term_hist_count==0) term_dir_cluster=current_dir_cluster;
    compute_geom();
}
static void term_run(){
    term_input[term_input_len]=0;
    char echo[TERM_COLS+1]; int p=0;
    const char* pre="$ "; for(int i=0;pre[i]&&p<TERM_COLS;i++) echo[p++]=pre[i];
    for(int i=0;term_input[i]&&p<TERM_COLS;i++) echo[p++]=term_input[i];
    echo[p]=0;
    term_push_line(echo);
    char cmdname[32]; int ci=0;
    while(term_input[ci] && term_input[ci]!=' ' && ci<31){ cmdname[ci]=term_input[ci]; ci++; }
    cmdname[ci]=0;
    if(cmdname[0]){
        if(term_blocked(cmdname)){
            term_push_line("This command is disabled inside the editor terminal.");
        } else {
            uint32_t saved=current_dir_cluster;
            current_dir_cluster=term_dir_cluster;
            redirect_start();
            cmd_dispatch(term_input);
            redirect_stop();
            term_dir_cluster=current_dir_cluster; // если была cd — фиксируем новую cwd терминала
            current_dir_cluster=saved;
            term_feed(redirect_get_buf());
        }
    }
    term_input[0]=0; term_input_len=0; term_input_caret=0;
}
// Автодополнение по Tab — как в обычном шелле: имя команды (пока нет пробела)
// через таблицу команд, иначе путь к файлу через fat32_tab_complete. Одно
// совпадение — дополняем сразу; несколько — печатаем варианты списком в вывод.
static void term_print_matches(char (*matches)[128], int mc){
    char line[TERM_COLS+1]; int p=0;
    for(int i=0;i<mc;i++){
        for(int k=0;matches[i][k]&&p<TERM_COLS-1;k++) line[p++]=matches[i][k];
        if(p<TERM_COLS-1) line[p++]=' ';
    }
    line[p]=0; term_push_line(line);
}
static void term_handle_tab(){
    int sp=-1; for(int i=0;i<term_input_len;i++) if(term_input[i]==' '){ sp=i; break; }
    if(sp<0){
        static char matches[16][128]; int mc=0;
        for(int i=0;i<CMD_TABLE_SIZE && mc<16;i++)
            if(strncmp(CMD_TABLE[i].name, term_input, term_input_len)==0)
                strcpy(matches[mc++], CMD_TABLE[i].name);
        if(mc==1){
            strcpy(term_input, matches[0]);
            term_input_len=strlen(term_input); term_input_caret=term_input_len;
        } else if(mc>1) term_print_matches(matches, mc);
    } else {
        char prefix[128]={0}; int k=0;
        for(int i=sp+1;i<term_input_len && k<127;i++) prefix[k++]=term_input[i];
        prefix[k]=0;
        static char matches[16][128];
        uint32_t saved=current_dir_cluster;
        current_dir_cluster=term_dir_cluster;
        int mc=fat32_tab_complete(prefix, matches, 16);
        current_dir_cluster=saved;
        if(mc==1){
            int base=sp+1, mlen=strlen(matches[0]);
            for(int i=0;i<mlen && base+i<62;i++) term_input[base+i]=matches[0][i];
            term_input_len=base+mlen; if(term_input_len>62) term_input_len=62;
            term_input[term_input_len]=0; term_input_caret=term_input_len;
        } else if(mc>1) term_print_matches(matches, mc);
    }
}
static void update_term_key(uint8_t key){
    if(key==0) return;
    if(key==27){ term_open=false; compute_geom(); return; }
    if(key=='\n'){ term_run(); return; }
    if(key==CHAR_TAB){ term_handle_tab(); return; }
    if(key=='\b'){
        if(term_input_caret>0){
            for(int i=term_input_caret-1;i<term_input_len;i++) term_input[i]=term_input[i+1];
            term_input_len--; term_input_caret--;
        }
        return;
    }
    if(key==CHAR_ARROW_LEFT){ if(term_input_caret>0) term_input_caret--; return; }
    if(key==CHAR_ARROW_RIGHT){ if(term_input_caret<term_input_len) term_input_caret++; return; }
    if(key==CHAR_ARROW_UP){ if(term_scroll<term_hist_count-1) term_scroll++; return; }
    if(key==CHAR_ARROW_DOWN){ if(term_scroll>0) term_scroll--; return; }
    if(key>=32 && key<127 && term_input_len<62){
        for(int i=term_input_len;i>term_input_caret;i--) term_input[i]=term_input[i-1];
        term_input[term_input_caret]=(char)key; term_input_len++; term_input_caret++;
    }
}
static void draw_terminal(){
    if(!term_open) return;
    int x=0, y=TY+text_h+SBW, w=W, h=TERM_H;
    gfx_fill_rect(x,y,w,h,C_PANEL);
    gfx_fill_rect(x,y,w,1,C_BORDER_HI);
    int lineh=8;
    int inputy=y+h-14;
    int outy=y+4, outh=inputy-outy;
    int visible = outh/lineh; if(visible<1) visible=1;
    int total=term_hist_count;
    if(term_scroll>total-visible) term_scroll=total-visible;
    if(term_scroll<0) term_scroll=0;
    int last = total-term_scroll;                 // индекс (не вкл.) последней видимой строки
    if(last>total) last=total;
    int first = last-visible; if(first<0) first=0;
    int row=0;
    for(int i=first;i<last;i++,row++)
        gfx_draw_text(x+6,outy+row*lineh,term_hist[i],C_TEXT,C_PANEL);
    gfx_fill_rect(x,inputy-2,w,1,C_BORDER);
    gfx_draw_text(x+6,inputy+3,"$",C_ACCENT,C_PANEL);
    // строка ввода — та же логика прокрутки/каретки, что и у обычных полей (draw_field),
    // с горизонтальным скроллом при длинном вводе, а не бесконечным выездом за край панели
    int fx=x+18, fw=w-24;
    int vis_chars=fw/8;
    int len=term_input_len;
    int off=0;
    if(term_input_caret>vis_chars-1) off=term_input_caret-vis_chars+1;
    if(off>len-vis_chars) off=len-vis_chars;
    if(off<0) off=0;
    int n=len-off; if(n>vis_chars) n=vis_chars; if(n<0) n=0;
    for(int i=0;i<n;i++) gfx_draw_char(fx+i*8,inputy+3,term_input[off+i],C_TEXT,C_PANEL);
    if(caret_visible()) gfx_fill_rect(fx+(term_input_caret-off)*8,inputy+1,1,10,C_CURSOR);
}

static void dlg_enter_dir(int idx){
    if(dlg_path_depth<16){
        int k=0; while(dlg_entries[idx].name[k]&&k<63){ dlg_path_stack[dlg_path_depth][k]=dlg_entries[idx].name[k]; k++; }
        dlg_path_stack[dlg_path_depth][k]=0; dlg_path_depth++;
    }
    dlg_dir_cluster=dlg_entries[idx].cluster;
    dlg_read_dir();
    dlg_name[0]=0; dlg_name_caret=0;
    dlg_name_focus=(dlg_mode==DLG_SAVEAS);
}
static void dlg_up(){
    if(!dlg_has_parent) return;
    if(dlg_path_depth>0) dlg_path_depth--;
    dlg_dir_cluster=dlg_parent_cluster;
    dlg_read_dir();
    dlg_name[0]=0; dlg_name_caret=0;
    dlg_name_focus=(dlg_mode==DLG_SAVEAS);
}
static void dlg_apply_open(int idx){
    current_dir_cluster=dlg_dir_cluster;
    strcpy(current_filename,dlg_entries[idx].name);
    g_lang=detect_lang(current_filename);
    dlg_reset_editor();
    editor_load_file(current_filename);
    prev_cursor_line=current_line; prev_cursor_col=current_col;
    dlg_mode=DLG_NONE;
    set_msg("Opened");
}
static void dlg_apply_save(){
    if(dlg_name[0]==0){ set_msg("Empty name"); return; }
    current_dir_cluster=dlg_dir_cluster;
    strcpy(current_filename,dlg_name);
    g_lang=detect_lang(current_filename);
    editor_save_file();
    dlg_mode=DLG_NONE;
}
static void dlg_confirm(){
    if(dlg_name[0]==0) return;
    int idx=-1;
    for(int i=0;i<dlg_count;i++) if(dlg_ieq(dlg_entries[i].name,dlg_name)){ idx=i; break; }
    if(idx>=0 && dlg_entries[idx].is_dir){ dlg_enter_dir(idx); return; }
    if(dlg_mode==DLG_OPEN){
        if(idx<0){ set_msg("Not found"); return; }
        dlg_apply_open(idx);
    } else {
        dlg_apply_save();
    }
}
static void dlg_cancel(){ dlg_mode=DLG_NONE; }
static void dlg_open_at(int mode){
    dlg_mode=mode;
    dlg_dir_cluster=current_dir_cluster;
    dlg_path_depth=0;
    ctx_open=false; ac_open=false; search_open=false;
    dlg_read_dir();
    strcpy(dlg_name,current_filename);
    dlg_name_caret=strlen(dlg_name);
    dlg_name_focus=(mode==DLG_SAVEAS);
    dlg_last_click=-99; dlg_last_row=-1; dlg_sb_drag=false;
}

static void dlg_sb_geom(int ly, int lh, int rows, int* ty, int* th){
    int total = dlg_count > rows ? dlg_count : rows;
    *th = lh * rows / total; if(*th < 16) *th = 16; if(*th > lh) *th = lh;
    int maxoff = dlg_count - rows; if(maxoff < 1) maxoff = 1;
    *ty = ly + (int)(((long)dlg_scroll * (lh - *th)) / maxoff);
}

// ─── Панель истории буфера обмена (Win+V) ────────────────────────────────────
static bool clip_popup_open = false;
static int  clip_sel = 0;
static void clip_popup_confirm(){
    if(clipboard_count()==0) return;
    const char* txt=clipboard_get(clip_sel);
    clipboard_len=0;
    for(int i=0; txt[i] && clipboard_len<CLIPBOARD_SIZE-1; i++) clipboard[clipboard_len++]=txt[i];
    clipboard[clipboard_len]=0;
    editor_paste();
    ac_after();
}
static void update_clip_popup_key(uint8_t c){
    if(c==0) return;
    int n=clipboard_count();
    if(c==CHAR_ARROW_UP){ if(clip_sel>0) clip_sel--; }
    else if(c==CHAR_ARROW_DOWN){ if(clip_sel<n-1) clip_sel++; }
    else if(c==27){ clip_popup_open=false; }
    else if(c=='\n'){ clip_popup_confirm(); clip_popup_open=false; }
}
static void draw_clip_popup(){
    dim_screen();
    int pw=420, ph=200, px=(W-pw)/2, py=(H-ph)/2;
    panel(px,py,pw,ph);
    gfx_fill_rect(px+2,py+2,pw-4,18,C_PANEL_HI);
    gfx_draw_text(px+10,py+7,"Clipboard history",C_TITLE_TXT,C_PANEL_HI);
    int n=clipboard_count();
    int lx=px+10, ly=py+26, lw=pw-20, lh=114;
    gfx_fill_rect(lx,ly,lw,lh,C_FIELD_BG);
    gfx_draw_rect(lx,ly,lw,lh,C_BORDER);
    if(n==0){
        gfx_draw_text(lx+8,ly+8,"(empty)",C_TEXT_DIM,C_FIELD_BG);
    } else {
        int maxc=(lw-16-16)/8; if(maxc>60) maxc=60;
        int mx=mouse_state.x,my=mouse_state.y;
        for(int i=0;i<n;i++){
            int iy=ly+2+i*14;
            if(i==clip_sel) gfx_fill_rect(lx+2,iy,lw-4,14,C_BTN_HOVER);
            // маленькая кнопка удаления слева
            if(gn_button(500+i,lx+2,iy,14,14,"x",nullptr,0,"Delete this entry")){
                clipboard_delete(i);
                if(clip_sel>=clipboard_count()) clip_sel=clipboard_count()-1;
                if(clip_sel<0) clip_sel=0;
                return; // список изменился — доиграем на следующем кадре
            }
            const char* s=clipboard_get(i);
            uint8_t fg=(i==clip_sel)?C_ACCENT:C_TEXT;
            char line[64]; int k=0;
            for(;k<maxc-1 && s[k] && s[k]!='\n'; k++) line[k]=s[k];
            line[k]=0;
            int tx=lx+20, ty=iy, tw=lw-24, th=14;
            gfx_draw_text(tx,ty+3,line,fg,GFX_TRANSPARENT);
            // клик по тексту строки — выбрать и сразу вставить (как Enter)
            if(gui_point_in_rect(mx,my,tx,ty,tw,th) && gui_click_edge()){
                clip_sel=i; clip_popup_confirm(); clip_popup_open=false; return;
            }
        }
    }
    int by=ly+lh+6;
    if(n>0 && gn_button(599,lx,by,90,18,"Clear all",nullptr,1,"Delete entire history")){
        clipboard_clear(); clip_sel=0;
    }
    gfx_draw_text(px+10,py+ph-14,"Up/Down/Enter or click  Esc=close",7,2);
}


static void draw_dlg(){
    dim_screen();
    int pw=460, ph=300, px=(W-pw)/2, py=(H-ph)/2-10;
    panel(px,py,pw,ph);
    gfx_fill_rect(px+2,py+2,pw-4,18,C_PANEL_HI);
    gfx_draw_text(px+10,py+7,(dlg_mode==DLG_OPEN)?"Open file":"Save as",C_TITLE_TXT,C_PANEL_HI);
    // строка пути
    char pth[96]; dlg_build_path(pth,96);
    gfx_draw_text(px+10,py+24,pth,C_ACCENT,C_PANEL);
    // список
    int lx,ly,lw,lh,rows,fx,fy,fw,fh,bx,by;
    dlg_layout(&lx,&ly,&lw,&lh,&rows,&fx,&fy,&fw,&fh,&bx,&by);
    int sbw    = (dlg_count > rows) ? 10 : 0;   // ползунок только когда не всё видно
    int list_w = lw - sbw;                       // ширина видимой текстовой области
    gfx_fill_rect(lx,ly,lw,lh,C_FIELD_BG);
    gfx_draw_rect(lx,ly,lw,lh,C_BORDER);
    gfx_set_clip(lx+1,ly+1,list_w-2,lh-2);
    for(int r=0;r<rows;r++){
        int i=dlg_scroll+r; if(i>=dlg_count) break;
        int iy=ly+2+r*14;
        if(i==dlg_sel) gfx_fill_rect(lx+2,iy,list_w-4,14,C_BTN_HOVER);
        int nx=lx+6;
        if(dlg_entries[i].is_dir){ draw_icon(nx,iy+2,ICON_FOLDER,C_ACCENT); nx+=10; }
        int maxc=(list_w-16-(dlg_entries[i].is_dir?10:0))/8;
        const char* s=dlg_entries[i].name;
        uint8_t fg=dlg_entries[i].is_dir?C_ACCENT:(i==dlg_sel?C_ACCENT:C_TEXT);
        for(int k=0;k<maxc&&s[k];k++)
            gfx_draw_char(nx+k*8,iy+3,s[k],fg,GFX_TRANSPARENT);
    }
    gfx_clear_clip();
    // вертикальный скроллбар списка (в стиле редактора: трек + ползунок + риски)
    if(sbw){
        int mx=mouse_state.x, my=mouse_state.y;
        int sx=lx+list_w;
        gfx_fill_rect(sx,ly,sbw,lh,C_SCROLL_TRK);
        int ty,th; dlg_sb_geom(ly,lh,rows,&ty,&th);
        bool hov=gui_point_in_rect(mx,my,sx,ty,sbw,th);
        gfx_fill_rect(sx+1,ty,sbw-2,th,(dlg_sb_drag||hov)?C_SCROLL_HOV:C_SCROLL_TH);
        for(int g=0;g<3;g++) gfx_fill_rect(sx+3,ty+th/2-2+g*2,4,1,C_SCROLL_TRK);
    }
    // поле имени
    gfx_draw_text(fx,fy-12,"File name:",C_TEXT_DIM,C_PANEL);
    draw_field(fx,fy,fw,fh,dlg_name,dlg_name_caret,dlg_name_focus);
    // кнопки
    if(gn_button(ID_DLG_CANCEL,bx+pw-16-64,by,64,22,"Cancel",nullptr,0,nullptr)) dlg_cancel();
    const char* ok=(dlg_mode==DLG_OPEN)?"Open":"Save";
    if(gn_button(ID_DLG_OK,bx+pw-16-64-8-64,by,64,22,ok,nullptr,1,nullptr)) dlg_confirm();
    bool upc=gn_button(ID_DLG_UP,bx+pw-16-64-8-64-8-52,by,52,22,"Up",nullptr,0,nullptr);
    if(upc && dlg_has_parent) dlg_up();
}

static void update_dlg_mouse(){
    int mx=mouse_state.x,my=mouse_state.y;
    int pw=460, ph=300, px=(W-pw)/2, py=(H-ph)/2-10;
    int lx,ly,lw,lh,rows,fx,fy,fw,fh,bx,by;
    dlg_layout(&lx,&ly,&lw,&lh,&rows,&fx,&fy,&fw,&fh,&bx,&by);
    int sbw    = (dlg_count > rows) ? 10 : 0;
    int list_w = lw - sbw;
    int sx     = lx + list_w;
    // ── активный drag ползунка: кнопка ещё зажата → пересчёт scroll по Y ──
    if(dlg_sb_drag){
        if(gui_mouse_down()){
            int ty,th; dlg_sb_geom(ly,lh,rows,&ty,&th);
            int maxoff=dlg_count-rows; if(maxoff<1)maxoff=1;
            if(lh-th>0) dlg_scroll=(my-dlg_sb_grab-ly)*maxoff/(lh-th);
            if(dlg_scroll<0)dlg_scroll=0; if(dlg_scroll>maxoff)dlg_scroll=maxoff;
        } else dlg_sb_drag=false;     // отпустили — конец перетаскивания
        return;
    }
    if(!gui_click_edge()) return;
    // ── клик по скроллбару: выше/ниже ползунка = страница, по ползунку = захват ──
    if(sbw && gui_point_in_rect(mx,my,sx,ly,sbw,lh)){
        int ty,th; dlg_sb_geom(ly,lh,rows,&ty,&th);
        int maxoff=dlg_count-rows; if(maxoff<1)maxoff=1;
        if(my<ty)        dlg_scroll-=rows;
        else if(my>ty+th) dlg_scroll+=rows;
        else             { dlg_sb_drag=true; dlg_sb_grab=my-ty; }
        if(dlg_scroll<0)dlg_scroll=0; if(dlg_scroll>maxoff)dlg_scroll=maxoff;
        return;
    }
    // клик по полю имени
    if(gui_point_in_rect(mx,my,fx,fy,fw,fh)){
        dlg_name_focus=true; dlg_name_caret=strlen(dlg_name); return;
    }
    // клик по списку (только текстовая область, полоса ползунка исключена)
    if(gui_point_in_rect(mx,my,lx,ly,list_w,lh)){
        int r=(my-ly-2)/14 + dlg_scroll;
        if(r>=0&&r<dlg_count){
            dlg_sel=r; dlg_name_focus=false; dlg_sync_field_from_sel();
            if(frame-dlg_last_click<14 && r==dlg_last_row){     // двойной клик
                if(dlg_entries[r].is_dir) dlg_enter_dir(r);
                else { strcpy(dlg_name,dlg_entries[r].name); dlg_confirm(); }
            }
            dlg_last_click=frame; dlg_last_row=r;
        }
        return;
    }
    // клик вне панели (но не по кнопкам — их ловит draw_dlg) — игнор
}

static void update_dlg_key(uint8_t key){
    if(key==0) return;
    if(key==27){ dlg_cancel(); return; }
    if(key=='\n'){ dlg_confirm(); return; }
    if(key==CHAR_TAB){ dlg_name_focus=!dlg_name_focus; return; }
    if(key==CHAR_ARROW_UP){ dlg_move_sel(-1); return; }
    if(key==CHAR_ARROW_DOWN){ dlg_move_sel(1); return; }
    if(key==CHAR_ARROW_LEFT||key==CHAR_ARROW_RIGHT){
        if(dlg_name_focus) field_key(key,dlg_name,64,&dlg_name_caret);
        return;
    }
    if(dlg_name_focus){ field_key(key,dlg_name,64,&dlg_name_caret); return; }
}

static const char* HELP_KEYS[] = {
    "^ESC","Shift+ESC","ESC","Ctrl+F","Ctrl+Z / Ctrl+Y",
    "Ctrl+C / Ctrl+X / Ctrl+V","Ctrl+K","Ctrl+Shift+A","Ctrl+A",
    "Shift+Arrows","Ctrl+Arrows","Ctrl+Bksp / Ctrl+Del",
    "Tab / Shift+Tab","Ctrl+Shift+X","Home / End","PgUp / PgDn",
    "Double-click","Drag mouse"};
static const char* HELP_DESC[] = {
    "Save and exit","Exit without saving","Exit (ask if unsaved)",
    "Find in file","Undo / Redo",
    "Copy / Cut / Paste selection",
    "Copy current line","Cut current line","Select all",
    "Select text","Jump by word","Delete word left / right",
    "Autocomplete: next / previous match",
    "Go to line[:column]","Start / end of line","Scroll by page",
    "Select word under cursor","Extend selection"};

// ─── контекстное меню: логика + отрисовка ────────────────────────────────────
static void ctx_compute_size(){
    int maxl=0;
    for(int i=0;i<CTX_COUNT;i++){
        int l=(int)strlen(CTX_LABEL[i])+(int)strlen(CTX_HINT[i]);
        if(CTX_HINT[i][0]) l+=2;            // визуальный зазор label..hint
        if(l>maxl) maxl=l;
    }
    ctx_w=maxl*8+CTX_PAD_X*2+16; if(ctx_w<120) ctx_w=120;
    ctx_h=CTX_COUNT*CTX_ITEM_H+4;
}
static void ctx_open_at(int mx,int my){
    ctx_compute_size();
    ctx_x=mx; ctx_y=my;
    if(ctx_x+ctx_w>W-2) ctx_x=W-2-ctx_w;     // не вылезать за правый/нижний край
    if(ctx_y+ctx_h>H-2) ctx_y=H-2-ctx_h;
    if(ctx_x<2) ctx_x=2; if(ctx_y<2) ctx_y=2;
    ctx_open=true; ctx_hover=-1;
}
static void ctx_close(){ ctx_open=false; ctx_hover=-1; }
static void ctx_do(int id){
    switch(id){
        case CTX_SAVE:  editor_save_file(); break;
        case CTX_UNDO:  do_undo();  set_msg("Undo");  break;
        case CTX_REDO:  do_redo();  set_msg("Redo");  break;
        case CTX_CUT:   if(sel_active) editor_cut();  else editor_cut_line();  break;
        case CTX_COPY:  if(sel_active) editor_copy(); else editor_copy_line(); break;
        case CTX_PASTE: editor_paste(); break;
        case CTX_FIND:  if(search_open) close_search(); else open_search(); break;
        case CTX_GOTO:  open_goto(); break;
        case CTX_SELALL:editor_select_all(); break;
        case CTX_HELP:  modal=MODAL_HELP; break;
        case CTX_EXIT:  pending_close_tab=-1; if(is_dirty) modal=MODAL_EXIT; else g_running=false; break;
        case CTX_OPEN:   tab_open_dialog();   break;
        case CTX_SAVEAS: dlg_open_at(DLG_SAVEAS); break;
    }
}
static void draw_ctx_menu(){
    int mx=mouse_state.x, my=mouse_state.y;
    gfx_fill_rect(ctx_x,ctx_y,ctx_w,ctx_h,C_PANEL);
    gfx_draw_rect(ctx_x,ctx_y,ctx_w,ctx_h,C_BORDER_HI);
    gfx_draw_rect(ctx_x+1,ctx_y+1,ctx_w-2,ctx_h-2,C_BORDER);
    gfx_fill_rect(ctx_x+1,ctx_y+1,ctx_w-2,1,C_PANEL_HI);   // верхний блик
    ctx_hover=-1;
    for(int i=0;i<CTX_COUNT;i++){
        int iy=ctx_y+2+i*CTX_ITEM_H;
        if(CTX_ROW_SEP(i)){                                   // разделитель
            gfx_fill_rect(ctx_x+CTX_PAD_X,iy+CTX_ITEM_H/2,ctx_w-CTX_PAD_X*2,1,C_BORDER);
            continue;
        }
        bool hov=gui_point_in_rect(mx,my,ctx_x+2,iy,ctx_w-4,CTX_ITEM_H);
        if(hov) ctx_hover=i;
        if(hov){
            gfx_fill_rect(ctx_x+2,iy,ctx_w-4,CTX_ITEM_H,C_BTN_HOVER);
            gfx_fill_rect(ctx_x+2,iy,2,CTX_ITEM_H,C_ACCENT);     // левая акцент-полоса
        }
        uint8_t fg=(i==CTX_EXIT)?C_DANGER:(hov?C_ACCENT:C_TEXT);
        gfx_draw_text(ctx_x+CTX_PAD_X,iy+3,CTX_LABEL[i],fg,GFX_TRANSPARENT);
        if(CTX_HINT[i][0]){                                   // хоткей справа
            int hl=(int)strlen(CTX_HINT[i])*8;
            gfx_draw_text(ctx_x+ctx_w-CTX_PAD_X-hl,iy+3,CTX_HINT[i],
                          hov?C_ACCENT_DIM:C_TEXT_DIM,GFX_TRANSPARENT);
        }
    }
}

#define HELP_ROWS 18

static void draw_modal(){
    dim_screen();
    int x,y,w,h;
    if(modal==MODAL_EXIT){
        w=340; h=118; x=(W-w)/2; y=(H-h)/2-30;
        panel(x,y,w,h);
        gfx_fill_rect(x+2,y+2,w-4,18,C_PANEL_HI);
        gfx_draw_text(x+10,y+7,"Unsaved changes",C_TITLE_TXT,C_PANEL_HI);
        char m[44]; int p=0;
        const char* pre="Save changes to '";
        for(int i=0;pre[i];i++)m[p++]=pre[i];
        for(int i=0;current_filename[i]&&p<38;i++)m[p++]=current_filename[i];
        m[p++]='\''; m[p++]='?'; m[p]=0;
        gfx_draw_text(x+16,y+38,m,C_TEXT,C_PANEL);
        gfx_draw_text(x+16,y+52,"Changes will be lost otherwise.",C_TEXT_DIM,C_PANEL);
        int by=y+h-32;
        if(gn_button(ID_EXIT_CANCEL,x+w-16-64,by,64,22,"Cancel",nullptr,0,nullptr)){
            modal=MODAL_NONE; pending_close_tab=-1;
        }
        if(gn_button(ID_EXIT_DISCARD,x+w-16-64-8-78,by,78,22,"Discard",nullptr,0,nullptr)){
            if(pending_close_tab>=0){ tab_remove_active(); pending_close_tab=-1; modal=MODAL_NONE; }
            else g_running=false;
        }
        if(gn_button(ID_EXIT_SAVE,x+w-16-64-8-78-8-104,by,104,22,"Save & Exit",nullptr,1,nullptr)){
            editor_save_file();
            if(pending_close_tab>=0){ tab_remove_active(); pending_close_tab=-1; modal=MODAL_NONE; }
            else g_running=false;
        }
    }
    else if(modal==MODAL_GOTO){
        w=280; h=112; x=(W-w)/2; y=(H-h)/2-30;
        panel(x,y,w,h);
        gfx_fill_rect(x+2,y+2,w-4,18,C_PANEL_HI);
        gfx_draw_text(x+10,y+7,"Go to line[:column]",C_TITLE_TXT,C_PANEL_HI);
        draw_field(x+16,y+34,w-32,18,goto_buf,goto_caret,true);
        int by=y+h-32;
        if(gn_button(ID_GOTO_CANCEL,x+w-16-64,by,64,22,"Cancel",nullptr,0,nullptr)) modal=MODAL_NONE;
        if(gn_button(ID_GOTO_OK,x+w-16-64-8-52,by,52,22,"Go",nullptr,1,nullptr)) do_goto();
    }
    else if(modal==MODAL_HELP){
        w=430; h=282; x=(W-w)/2; y=(H-h)/2-20;
        panel(x,y,w,h);
        gfx_fill_rect(x+2,y+2,w-4,18,C_PANEL_HI);
        gfx_draw_text(x+10,y+7,"GITE - keyboard & mouse",C_TITLE_TXT,C_PANEL_HI);
        for(int r=0;r<HELP_ROWS;r++){
            int ry=y+30+r*12;
            gfx_draw_text(x+16,ry,HELP_KEYS[r],C_ACCENT,C_PANEL);
            gfx_draw_text(x+160,ry,HELP_DESC[r],C_TEXT,C_PANEL);
        }
        gfx_draw_text(x+16,y+h-24,"Press any key to close",C_TEXT_DIM,C_PANEL);
        if(gn_button(ID_HELP_CLOSE,x+w-16-56,y+h-30,56,20,"Close",nullptr,0,nullptr)) modal=MODAL_NONE;
    }
}

// ─── update: модальные окна ──────────────────────────────────────────────────
static void update_modal(uint8_t key){
    if(modal==MODAL_EXIT){
        if(key==27) modal=MODAL_NONE;
    } else if(modal==MODAL_GOTO){
        if(key==27){ modal=MODAL_NONE; return; }
        if(field_key(key,goto_buf,16,&goto_caret)) do_goto();
    } else if(modal==MODAL_HELP){
        if(key!=0) modal=MODAL_NONE;
    }
}

// ─── update: поиск (возвращает true, если клавиша съедена) ──────────────────
static bool update_search_key(uint8_t key){
    if(key==CHAR_CTRL_F){
        if(search_open) close_search(); else open_search();
        return true;
    }
    if(!search_open) return false;
    if(key==27){ close_search(); return true; }
    if(key==0) return false;
    if(key=='\t'){ replace_focused=!replace_focused; search_focused=!replace_focused; return true; }
    if(search_focused){
        if(key=='\n'){ search_next(); return true; }
        if(key==CHAR_ARROW_DOWN||key==CHAR_ARROW_RIGHT){ search_next(); return true; }
        if(key==CHAR_ARROW_UP||key==CHAR_ARROW_LEFT){ search_prev(); return true; }
        int old_len=strlen(search_query);
        field_key(key,search_query,64,&search_caret);
        if((int)strlen(search_query)!=old_len) search_refresh();
        return true;
    }
    if(replace_focused){
        if(key=='\n'){ replace_current(); return true; }
        if(key==CHAR_CTRL_ENTER){ replace_all(); return true; }
        field_key(key,search_replace,64,&replace_caret);
        return true;
    }
    return false;
}

// ─── update: клавиатура редактора ────────────────────────────────────────────
static void update_editor_key(uint8_t c){
    if(c==0) return;
    // попап автодополнения перехватывает свои клавиши
    if(ac_open){
        if(c==CHAR_TAB){ ac_cycle(1); blink_hold=25; return; }
        if(c==CHAR_SHIFT_TAB){ ac_cycle(-1); blink_hold=25; return; }
        if(c=='\n'){ ac_apply(); return; }
        if(c==27){ ac_open=false; return; }
    }
    blink_hold=25;
    // выход
    if(c==CHAR_SHIFT_ESC){ g_running=false; return; }
    if(c==CHAR_CTRL_ESC){ editor_save_file(); g_running=false; return; }
    if(c==27){
        if(search_open){ close_search(); return; }
        pending_close_tab=-1;
        if(is_dirty) modal=MODAL_EXIT; else g_running=false;
        return;
    }
    // правка
    if(c=='\n'){ editor_new_line(); ac_after(); return; }
    if(c=='\b'){
        if(sel_active) editor_delete_selection();
        else if(current_col>0 && current_col<(int)strlen(editor_buffer[current_line]) &&
                editor_backspace_pair_match(editor_buffer[current_line][current_col-1],
                                             editor_buffer[current_line][current_col]))
            editor_delete_pair();
        else editor_delete_char();
        ac_after(); return;
    }
    if(c==CHAR_DEL){ if(sel_active) editor_delete_selection(); else editor_delete_forward(); ac_after(); return; }
    if(c==CHAR_CTRL_Z){ do_undo(); set_msg("Undo"); return; }
    if(c==CHAR_CTRL_SHIFT_Z){ do_redo(); set_msg("Redo"); return; }
    if(c==CHAR_CTRL_C){ editor_copy(); return; }
    if(c==CHAR_CTRL_V){ editor_paste(); ac_after(); return; }
    if(c==CHAR_WIN_V){ clip_popup_open=true; clip_sel=0; ac_open=false; return; }
    if(c==CHAR_CTRL_K){ editor_copy_line(); return; }
    if(c==CHAR_CTRL_X){ editor_cut(); return; }
    if(c==CHAR_CTRL_SHIFT_A){ editor_cut_line(); return; }
    if(c==CHAR_CTRL_SHIFT_X){ open_goto(); return; }
    if(c==CHAR_CTRL_A){ editor_select_all(); return; }
    if(c==CHAR_CTRL_BACKSPACE){ if(sel_active) editor_delete_selection(); else editor_delete_word_back(); ac_after(); return; }
    if(c==CHAR_CTRL_DEL){ if(sel_active) editor_delete_selection(); else editor_delete_word_fwd(); ac_after(); return; }
    if(c==CHAR_CTRL_H){ modal=MODAL_HELP; return; }
    if(c==CHAR_HOME){ current_col=0; return; }
    if(c==CHAR_END){ current_col=strlen(editor_buffer[current_line]); return; }
    if(c==CHAR_PGUP){ current_line-=vis_rows; if(current_line<0)current_line=0; clamp_col(); return; }
    if(c==CHAR_PGDN){ current_line+=vis_rows; if(current_line>=total_lines)current_line=total_lines-1; clamp_col(); return; }
    if(c==CHAR_TAB){ tab_autocomplete(); return; }
    // навигация (+ выделение с Shift) — как в cmd_nano
    if(c==CHAR_ARROW_UP||c==CHAR_SHIFT_UP){
        bool sh=(c==CHAR_SHIFT_UP);
        if(sh){ if(!sel_active){sel_active=true;sel_line_start=current_line;sel_col_start=current_col;} }
        else sel_active=false;
        if(current_line>0) current_line--;
        clamp_col();
        if(sh){ sel_line_end=current_line; sel_col_end=current_col; }
        return;
    }
    if(c==CHAR_ARROW_DOWN||c==CHAR_SHIFT_DOWN){
        bool sh=(c==CHAR_SHIFT_DOWN);
        if(sh){ if(!sel_active){sel_active=true;sel_line_start=current_line;sel_col_start=current_col;} }
        else sel_active=false;
        if(current_line<total_lines-1) current_line++;
        clamp_col();
        if(sh){ sel_line_end=current_line; sel_col_end=current_col; }
        return;
    }
    if(c==CHAR_ARROW_LEFT||c==CHAR_SHIFT_LEFT){
        bool sh=(c==CHAR_SHIFT_LEFT);
        if(sh){ if(!sel_active){sel_active=true;sel_line_start=current_line;sel_col_start=current_col;} }
        else sel_active=false;
        if(current_col>0) current_col--;
        if(sh){ sel_line_end=current_line; sel_col_end=current_col; }
        return;
    }
    if(c==CHAR_ARROW_RIGHT||c==CHAR_SHIFT_RIGHT){
        bool sh=(c==CHAR_SHIFT_RIGHT);
        if(sh){ if(!sel_active){sel_active=true;sel_line_start=current_line;sel_col_start=current_col;} }
        else sel_active=false;
        if(current_col<(int)strlen(editor_buffer[current_line])) current_col++;
        if(sh){ sel_line_end=current_line; sel_col_end=current_col; }
        return;
    }
    if(c==CHAR_CTRL_LEFT){ sel_active=false; editor_word_left(); return; }
    if(c==CHAR_CTRL_RIGHT){ sel_active=false; editor_word_right(); return; }
    if(c==CHAR_CTRL_SHIFT_LEFT){
        if(!sel_active){sel_active=true;sel_line_start=current_line;sel_col_start=current_col;}
        editor_word_left(); sel_line_end=current_line; sel_col_end=current_col; return;
    }
    if(c==CHAR_CTRL_SHIFT_RIGHT){
        if(!sel_active){sel_active=true;sel_line_start=current_line;sel_col_start=current_col;}
        editor_word_right(); sel_line_end=current_line; sel_col_end=current_col; return;
    }
    // обычный символ
    if(c>=32&&c<=126){
        sel_active=false;
        char* line=editor_buffer[current_line];
        char nextc = (current_col<(int)strlen(line)) ? line[current_col] : 0;
        bool is_closer = (c==')'||c==']'||c=='}'||c=='"'||c=='\'');
        if(is_closer && nextc==c) current_col++;                    // печать поверх уже стоящей закрывающей — проходим сквозь
        else if(c=='(') editor_insert_pair('(',')');
        else if(c=='[') editor_insert_pair('[',']');
        else if(c=='{') editor_insert_pair('{','}');
        else if(c=='"') editor_insert_pair('"','"');
        else if(c=='\'') editor_insert_pair('\'','\'');
        else editor_insert_char((char)c);
        ac_after();
    }
}

// ─── update: мышь ────────────────────────────────────────────────────────────
static void mouse_to_caret(int mx,int my,int* out_line,int* out_col){
    int line=scroll_offset+(my-TY)/8;
    if(line<0)line=0; if(line>=total_lines)line=total_lines-1;
    int col=h_scroll+(mx-TX+4)/8;
    int len=strlen(editor_buffer[line]);
    if(col<0)col=0; if(col>len)col=len;
    *out_line=line; *out_col=col;
}
static void select_word_at(){
    char* ln=editor_buffer[current_line];
    int len=strlen(ln);
    int a=current_col; if(a>len)a=len;
    int b;
    if(a<len&&is_ident_char(ln[a])){
        while(a>0&&is_ident_char(ln[a-1]))a--;
        b=a; while(b<len&&is_ident_char(ln[b]))b++;
    } else { b=a+1; if(b>len)b=len; }
    sel_active=true;
    sel_line_start=current_line; sel_col_start=a;
    sel_line_end=current_line; sel_col_end=b;
    current_col=b;
}
static void update_mouse(){
    int mx=mouse_state.x,my=mouse_state.y;
    if(dlg_mode!=DLG_NONE){ update_dlg_mouse(); return; }   // диалог ловит мышь целиком
    if(clip_popup_open) return;   // попап буфера обмена — только клавиатура
    if(term_open) return;         // терминал — только клавиатура (Esc, стрелки, ввод)
    // ── ПКМ: toggle контекстного меню (работает всегда внутри update_mouse) ──
    bool rmb_now  = (mouse_state.buttons & 0x02) != 0;
    bool rmb_edge = rmb_now && !ctx_rmb_was;
    ctx_rmb_was   = rmb_now;
    if(rmb_edge){
        if(ctx_open) ctx_close();
        else { ac_open=false; ctx_open_at(mx,my); }   // заодно гасим попап автодополнения
        return;
    }
    // ── пока меню открыто — оно перехватывает мышь целиком (редактор/скролл/
    //    поиск в этом кадре не получают клик, иначе ЛКМ по пункту «протёк» бы
    //    в текст и поставил туда каретку) ──
    if(ctx_open){
        bool lmb_now  = gui_mouse_down();
        bool lmb_edge = lmb_now && !ctx_lmb_was;
        ctx_lmb_was   = lmb_now;
        if(lmb_edge){
            if(gui_point_in_rect(mx,my,ctx_x,ctx_y,ctx_w,ctx_h)){
                if(ctx_hover>=0){ int id=ctx_hover; ctx_close(); ctx_do(id); }
                // клик по разделителю (hover<0) — меню остаётся открытым
            } else {
                ctx_close();                            // клик вне меню — закрыть
            }
        }
        return;
    }

    if(ac_open){
        int px,py,pw,ph; popup_rect(&px,&py,&pw,&ph);
        if(gui_point_in_rect(mx,my,px,py,pw,ph)) return;
    }
    // перетаскивание ползунков
    if(v_drag){
        if(gui_mouse_down()){
            int ty,th; v_thumb(&ty,&th);
            int maxoff=total_lines-vis_rows; if(maxoff<1)maxoff=1;
            if(text_h-th>0) scroll_offset=(my-v_grab-TY)*maxoff/(text_h-th);
            if(scroll_offset<0)scroll_offset=0; if(scroll_offset>maxoff)scroll_offset=maxoff;
        } else v_drag=0;
        return;
    }
    if(h_drag){
        if(gui_mouse_down()){
            int tx,tw; h_thumb(&tx,&tw);
            int ml=doc_max_len(); if(ml<vis_cols)ml=vis_cols;
            int maxoff=ml-vis_cols; if(maxoff<1)maxoff=1;
            if(text_w-tw>0) h_scroll=(mx-h_grab-TX)*maxoff/(text_w-tw);
            if(h_scroll<0)h_scroll=0; if(h_scroll>maxoff)h_scroll=maxoff;
        } else h_drag=0;
        return;
    }
    bool click=gui_click_edge();
    bool rel=gui_release_edge();
    bool down=gui_mouse_down();
    if(click){
        if(ac_open) ac_open=false;
        // фокус поля поиска
        if(search_open){
            int px,py,pw,ph; search_panel_rect(&px,&py,&pw,&ph);
            if(gui_point_in_rect(mx,my,px+40,py+4,150,18)){
                search_focused=true; replace_focused=false; search_caret=strlen(search_query); return;
            }
            if(gui_point_in_rect(mx,my,px+40,py+30,150,18)){
                replace_focused=true; search_focused=false; replace_caret=strlen(search_replace); return;
            }
            if(!gui_point_in_rect(mx,my,px,py,pw,ph)){ search_focused=false; replace_focused=false; }
        }
        // вертикальный скроллбар
        if(gui_point_in_rect(mx,my,W-SBW,TY,SBW,text_h)){
            if(total_lines>vis_rows){
                int ty,th; v_thumb(&ty,&th);
                if(my<ty) scroll_offset-=vis_rows;
                else if(my>ty+th) scroll_offset+=vis_rows;
                else { v_drag=1; v_grab=my-ty; }
            }
            return;
        }
        // горизонтальный скроллбар
        if(gui_point_in_rect(mx,my,TX,TY+text_h,text_w,SBW)){
            if(doc_max_len()>vis_cols){
                int tx,tw; h_thumb(&tx,&tw);
                if(mx<tx) h_scroll-=vis_cols;
                else if(mx>tx+tw) h_scroll+=vis_cols;
                else { h_drag=1; h_grab=mx-tx; }
            }
            return;
        }
        // текстовая область: клик / двойной клик
        if(gui_point_in_rect(mx,my,TX,TY,text_w,text_h)){
            int line,col; mouse_to_caret(mx,my,&line,&col);
            if(frame-last_click_frame<14&&iabs(mx-last_mx)<4&&iabs(my-last_my)<4){
                current_line=line; current_col=col;
                select_word_at(); selecting=false;
            } else {
                current_line=line; current_col=col;
                sel_active=true;
                sel_line_start=line; sel_col_start=col;
                sel_line_end=line; sel_col_end=col;
                selecting=true;
            }
            last_click_frame=frame; last_mx=mx; last_my=my;
            blink_hold=25;
            return;
        }
    }
    // протяжка выделения + автопрокрутка у краёв
    if(selecting){
        if(down){
            int line,col; mouse_to_caret(mx,my,&line,&col);
            current_line=line; current_col=col;
            sel_line_end=line; sel_col_end=col;
            if(my<TY+10&&scroll_offset>0) scroll_offset--;
            if(my>TY+text_h-10&&scroll_offset<total_lines-vis_rows) scroll_offset++;
            if(mx<TX+10&&h_scroll>0) h_scroll--;
            if(mx>TX+text_w-10) h_scroll++;
        }
        if(rel) selecting=false;
    }
}

// ─── общий кадр ──────────────────────────────────────────────────────────────
static void draw_all(){
    bool cursor_moved = (current_line != prev_cursor_line) ||
                        (current_col  != prev_cursor_col);
    if(cursor_moved) scroll_to_cursor();
    prev_cursor_line = current_line;
    prev_cursor_col  = current_col;

    draw_title();
    draw_toolbar();
    draw_tab_bar();
    draw_sidebar();
    draw_editor_area();
    draw_scrollbars();
    draw_status();
    draw_terminal();
    if(search_open) draw_search_panel();
    if(ac_open) draw_popup();
    if(ctx_open && modal==MODAL_NONE) draw_ctx_menu();
    if(modal!=MODAL_NONE) draw_modal();
    if(clip_popup_open) draw_clip_popup();
    if(dlg_mode!=DLG_NONE) draw_dlg();
}

// ─── точка входа ─────────────────────────────────────────────────────────────
void cmd_gite(char* filename){
    bool has_file = strlen(filename)>0;
    strcpy(current_filename, has_file?filename:"");
    g_lang = has_file?detect_lang(filename):LANG_NONE;
    // сброс состояния (ВАЖНО: g_running=true — иначе повторный запуск не крутит цикл)
    g_running=true;
    current_line=0; current_col=0; total_lines=1; scroll_offset=0; h_scroll=0;
    is_dirty=false; sel_active=false; clipboard_len=0;
    search_open=false; search_active=false; search_focused=false; replace_focused=false;
    search_query[0]=0; search_query_len=0; search_replace[0]=0; search_result_count=0;
    ac_open=false; ac_count=0; ac_index=-1; popup_pressed=false;
    modal=MODAL_NONE; msg_timer=0; frame=0; blink_hold=0;
    v_drag=0; h_drag=0; selecting=false; g_pressed_id=-1;
    undo_top=-1; redo_top=-1; undo_pending=false;
    for(int i=0;i<EDITOR_MAX_LINES;i++) editor_buffer[i][0]=0;
    // графика: 640x480, встроенная палитра редактора
    vga_enter_mode_640x480();
    gn_setup_palette();
    compute_geom();
    gfx_clear(C_BG);
    sidebar_init();
    if(has_file) editor_load_file(filename); else set_msg("New file");
    // вкладки: файл, с которым запустили gite, становится вкладкой 0
    tab_count=1; active_tab=0;
    EditorTab* t0=&tabs[0];
    t0->used=true;
    strcpy(t0->filename,current_filename);
    for(int i=0;i<total_lines;i++) strcpy(t0->lines[i],editor_buffer[i]);
    t0->total_lines=total_lines; t0->current_line=current_line; t0->current_col=current_col;
    t0->scroll_offset=scroll_offset; t0->h_scroll=h_scroll; t0->is_dirty=is_dirty; t0->lang=g_lang;
    pending_close_tab=-1;
    prev_cursor_line = current_line;
    prev_cursor_col  = current_col;
    rtc_get_time_string(clock_str);
    gfx_cursor_show();
    // главный цикл: key -> update -> draw -> flip
    while(g_running){
        uint8_t key=get_key();
        gui_begin_frame(key);
        frame++;
        tip_buf[0]=0;
        if(msg_timer>0) msg_timer--;
        if(blink_hold>0) blink_hold--;
        if(frame%24==0) rtc_get_time_string(clock_str);
        if(dlg_mode!=DLG_NONE){
            update_dlg_key(key);
            update_mouse();
        } else if(clip_popup_open){
            update_clip_popup_key(key);
            update_mouse();
        } else if(term_open){
            update_term_key(key);
            update_mouse();
        } else if(sidebar_mode!=0){
            update_sidebar_key(key);
            update_mouse();
        } else if(modal!=MODAL_NONE){
            update_modal(key);
            ctx_open=false;
        } else {
            if(ctx_open){
                if(key!=0) ctx_close();
            } else {
                if(!update_search_key(key)) update_editor_key(key);
            }
            update_mouse();
        }
        undo_finalize();
        draw_all();
        gfx_cursor_move(mouse_state.x, mouse_state.y);                 // ДО flip: s_cur = текущая мышь
        gfx_cursor_blit_backbuffer(gfx_cursor_get_x(), gfx_cursor_get_y()); // курсор в backbuffer
        gfx_flip();                                                    // унесёт курсор в VRAM вместе со сценой
        for(volatile int d=0;d<20000;d++);   // лёгкий throttle цикла
    }
    gfx_cursor_hide();
    gfx_init_palette();        // вернуть шеллу стандартные 16 цветов
    vga_exit_to_text();
    shell_init_status_bar();
    println("Exited gite.");
}