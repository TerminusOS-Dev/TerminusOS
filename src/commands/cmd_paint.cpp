#include "cmd_paint.h"
#include "../drivers/vga_gfx.h"
#include "../drivers/gui_widgets.h"
#include "../drivers/fat32.h"
#include "../drivers/disk.h"
#include "../kernel/keyboard.h"
#include "../kernel/mouse.h"
#include "../lib/screen.h"
#include "../lib/string.h"
#include "../shell/shell.h"
#include "../drivers/serial.h"

// ─── Размеры ─────────────────────────────────────────────────────────────────
static const int TOOLBAR_H = 28;   // высота панели инструментов сверху
static const int STATUS_H  = 12;   // строка статуса снизу

// ─── Палитра paint (256 цветов) ──────────────────────────────────────────────
// pal6 — зеркало DAC (6 бит на канал, 0..63). Читать DAC из драйвера нельзя
// (геттера нет), поэтому paint сам хранит текущие значения и меняет палитру
// только через gfx_set_palette(), чтобы зеркало и железо не расходились.
//
// Слоты 0..15 — цвета интерфейса (тулбар, кнопки, рамки), их не трогаем:
// иначе при правке палитры сломается сам paint. Слоты 16..255 свободно
// редактируются (серые, куб 6x6x6, свои цвета) и сохраняются внутри BMP.
static uint8_t pal6[256][3];

static inline uint8_t p8(int v6) { return (uint8_t)((v6 << 2) | (v6 >> 4)); }  // 6 бит -> 8 бит

static void pal_set(int idx, int r, int g, int b) {
    if (r < 0) r = 0; if (r > 63) r = 63;
    if (g < 0) g = 0; if (g > 63) g = 63;
    if (b < 0) b = 0; if (b > 63) b = 63;
    pal6[idx][0] = (uint8_t)r; pal6[idx][1] = (uint8_t)g; pal6[idx][2] = (uint8_t)b;
    gfx_set_palette((uint8_t)idx, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// Сброс к стандартной палитре драйвера + синхронизация зеркала.
// Раскладка должна совпадать с gfx_init_palette() в vga_gfx.cpp:
// 0-15 VGA, 16-31 серые, 32-247 куб 6x6x6, 248-255 серые.
static void pal_reset_default() {
    gfx_init_palette();
    static const uint8_t std16[16][3] = {
        {  0,  0,  0 }, {  0,  0, 42 }, {  0, 42,  0 }, {  0, 42, 42 },
        { 42,  0,  0 }, { 42,  0, 42 }, { 42, 21,  0 }, { 42, 42, 42 },
        { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 },
        { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 },
    };
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) pal6[i][c] = std16[i][c];
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(i * 4);
        pal6[16 + i][0] = pal6[16 + i][1] = pal6[16 + i][2] = v;
    }
    static const uint8_t levels[6] = { 0, 12, 25, 38, 51, 63 };
    int idx = 32;
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                pal6[idx][0] = levels[r]; pal6[idx][1] = levels[g]; pal6[idx][2] = levels[b];
                idx++;
            }
    for (int i = 0; i < 8; i++) {
        int v = 7 + i * 8; if (v > 63) v = 63;
        pal6[248 + i][0] = pal6[248 + i][1] = pal6[248 + i][2] = (uint8_t)v;
    }
}

// Ближайший цвет среди ВСЕХ 256 слотов текущей палитры (вход — 8-бит RGB).
// Взвешенное расстояние: глаз чувствительнее к зелёному, чем к синему.
static uint8_t nearest_pal(uint8_t r, uint8_t g, uint8_t b) {
    int best = 0, bd = 0x7fffffff;
    for (int i = 0; i < 256; i++) {
        int dr = (int)r - (int)p8(pal6[i][0]);
        int dg = (int)g - (int)p8(pal6[i][1]);
        int db = (int)b - (int)p8(pal6[i][2]);
        int d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint8_t)best;
}

// int -> строка, возвращает длину
static int num_to_str(int v, char* out) {
    char tmp[12]; int n = 0;
    if (v <= 0) tmp[n++] = '0';
    else while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

// ─── Холст ───────────────────────────────────────────────────────────────────
static const int CANVAS_W = 640;
static uint8_t canvas[640 * 480];  // весь экран как холст (обрежем по факту)
static void canvas_clear(int canvas_h, uint8_t color) {
    for (int i = 0; i < CANVAS_W * canvas_h; i++) canvas[i] = color;
}
// Рисуем квадратным пером любого размера 1..10.
// Для чётных размеров перо смещено на пол-пикселя, ширина всегда ровно pen_size.
static void canvas_draw(int cx, int cy, int canvas_h,
                        int pen_size, uint8_t color) {
    int lo = -(pen_size - 1) / 2;
    int hi = lo + pen_size - 1;
    for (int dy = lo; dy <= hi; dy++)
        for (int dx = lo; dx <= hi; dx++) {
            int px = cx + dx, py = cy + dy;
            if (px >= 0 && px < CANVAS_W && py >= 0 && py < canvas_h)
                canvas[py * CANVAS_W + px] = color;
        }
}
// Bresenham между двумя точками (чтобы не было пробелов при быстром движении)
static void canvas_line(int x0, int y0, int x1, int y1,
                        int canvas_h, int pen_size, uint8_t color) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        canvas_draw(x0, y0, canvas_h, pen_size, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}
// ─── Блит холста в backbuffer ─────────────────────────────────────────────────
static void blit_canvas(int W, int canvas_y, int canvas_h) {
    uint8_t* bb = gfx_get_backbuffer();
    for (int row = 0; row < canvas_h; row++) {
        const uint8_t* src = canvas + row * CANVAS_W;
        uint8_t*       dst = bb + (canvas_y + row) * W;
        for (int col = 0; col < W; col++) dst[col] = src[col];
    }
}

// ─── Размер кисти: слайдер 0.0..1.0 -> 1..10 ─────────────────────────────────
static const int PEN_MIN = 1, PEN_MAX = 10;
static int pen_from_v(float v) {
    int s = PEN_MIN + (int)(v * (float)(PEN_MAX - PEN_MIN) + 0.5f);
    if (s < PEN_MIN) s = PEN_MIN;
    if (s > PEN_MAX) s = PEN_MAX;
    return s;
}

// ─── Toolbar ──────────────────────────────────────────────────────────────────
// Быстрая палитра: 16 стандартных цветов (индексы 0..15).
// Все 256 цветов и редактор — в панели "Colors".
static const uint8_t PALETTE[16] = {
    GFX_BLACK,      GFX_BLUE,       GFX_GREEN,      GFX_CYAN,
    GFX_RED,        GFX_MAGENTA,    GFX_BROWN,      GFX_LIGHT_GRAY,
    GFX_DARK_GRAY,  GFX_LIGHT_BLUE, GFX_LIGHT_GREEN,GFX_LIGHT_CYAN,
    GFX_LIGHT_RED,  GFX_LIGHT_MAGENTA, GFX_YELLOW,  GFX_WHITE,
};
static void draw_toolbar(int W, uint8_t cur_color, float* pen_v, int pen_size,
                         bool pal_open,
                         bool* clicked_clear, uint8_t* new_color,
                         bool* clicked_save, bool* toggle_pal) {
    gfx_fill_rect(0, 0, W, TOOLBAR_H, GFX_DARK_GRAY);
    gfx_draw_hline(0, TOOLBAR_H - 1, W, GFX_BLACK);
    gfx_draw_text(4, 10, "Paint", GFX_WHITE, GFX_DARK_GRAY);
    const int CUR_X = 52, CUR_Y = 3, CUR_SZ = 22;
    gfx_fill_rect(CUR_X, CUR_Y, CUR_SZ, CUR_SZ, cur_color);
    gfx_draw_rect(CUR_X, CUR_Y, CUR_SZ, CUR_SZ, GFX_WHITE);
    const int PAL_X = 82, PAL_Y = 4, SW = 16, SH = 9, GAP = 1;
    for (int i = 0; i < 16; i++) {
        int col = i % 8, row = i / 8;
        int bx = PAL_X + col * (SW + GAP);
        int by = PAL_Y + row * (SH + GAP);
        gfx_fill_rect(bx, by, SW, SH, PALETTE[i]);
        if (PALETTE[i] == cur_color)
            gfx_draw_rect(bx - 1, by - 1, SW + 2, SH + 2, GFX_WHITE);
        if (gui_click_edge() &&
            gui_point_in_rect(gfx_cursor_get_x(), gfx_cursor_get_y(),
                              bx, by, SW, SH))
            *new_color = PALETTE[i];
    }

    // Кнопка открытия панели со всеми 256 цветами
    if (gui_button(224, 4, 56, 20, pal_open ? "Close" : "Colors"))
        *toggle_pal = true;

    // Размер кисти: слайдер 1..10 + превью + число
    gfx_draw_text(288, 10, "Size", GFX_WHITE, GFX_DARK_GRAY);
    gui_slider(322, 6, 90, pen_v);
    // превью: квадрат реального размера пера на тёмной подложке
    const int PRV_X = 420, PRV_Y = 7, PRV_SZ = 14;
    gfx_fill_rect(PRV_X, PRV_Y, PRV_SZ, PRV_SZ, GFX_BLACK);
    gfx_draw_rect(PRV_X, PRV_Y, PRV_SZ, PRV_SZ, GFX_LIGHT_GRAY);
    int off = (PRV_SZ - pen_size) / 2;
    gfx_fill_rect(PRV_X + off, PRV_Y + off, pen_size, pen_size, cur_color);
    char nb[8]; num_to_str(pen_size, nb);
    gfx_draw_text(PRV_X + PRV_SZ + 6, 10, nb, GFX_WHITE, GFX_DARK_GRAY);

    if (gui_button(466, 4, 40, 20, "Clear"))
        *clicked_clear = true;
    if (gui_button(510, 4, 40, 20, "Save"))
        *clicked_save = true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Панель "Colors": все 256 цветов + редактор выбранного цвета (R/G/B)
// ═══════════════════════════════════════════════════════════════════════════
static const int PAL_PX = 8,  PAL_PY = TOOLBAR_H + 6;
static const int PAL_PW = 440, PAL_PH = 268;
static const int PAL_GX = PAL_PX + 8, PAL_GY = PAL_PY + 20;   // сетка 16x16
static const int PAL_CELL = 14, PAL_PITCH = 15;
static const int PAL_RX = PAL_PX + 8 + 16 * PAL_PITCH + 16;   // правая колонка

static bool pal_panel_hit(int x, int y) {
    return gui_point_in_rect(x, y, PAL_PX, PAL_PY, PAL_PW, PAL_PH);
}

static void draw_palette_panel(uint8_t* cur_color, bool* want_close) {
    const uint8_t BG = GFX_DARK_GRAY;
    gfx_fill_rect(PAL_PX, PAL_PY, PAL_PW, PAL_PH, BG);
    gfx_draw_rect(PAL_PX, PAL_PY, PAL_PW, PAL_PH, GFX_WHITE);
    gfx_draw_text(PAL_PX + 8, PAL_PY + 6, "Colors (256)", GFX_YELLOW, BG);

    // клик по сетке -> выбрать цвет
    if (gui_click_edge()) {
        int gx = gfx_cursor_get_x() - PAL_GX;
        int gy = gfx_cursor_get_y() - PAL_GY;
        if (gx >= 0 && gy >= 0 && gx < 16 * PAL_PITCH - 1 && gy < 16 * PAL_PITCH - 1)
            *cur_color = (uint8_t)((gy / PAL_PITCH) * 16 + (gx / PAL_PITCH));
    }

    // сетка 16x16
    for (int i = 0; i < 256; i++) {
        int bx = PAL_GX + (i % 16) * PAL_PITCH;
        int by = PAL_GY + (i / 16) * PAL_PITCH;
        gfx_fill_rect(bx, by, PAL_CELL, PAL_CELL, (uint8_t)i);
    }
    {   // рамка выбранного цвета поверх сетки
        int bx = PAL_GX + (*cur_color % 16) * PAL_PITCH;
        int by = PAL_GY + (*cur_color / 16) * PAL_PITCH;
        gfx_draw_rect(bx - 1, by - 1, PAL_CELL + 2, PAL_CELL + 2, GFX_WHITE);
        gfx_draw_rect(bx,     by,     PAL_CELL,     PAL_CELL,     GFX_BLACK);
    }

    const int RX = PAL_RX;
    uint8_t c = *cur_color;

    // заголовок "Color #N" + крупный образец
    char title[16] = "Color #";
    num_to_str(c, title + 7);
    gfx_draw_text(RX, PAL_PY + 20, title, GFX_WHITE, BG);
    gfx_fill_rect(RX, PAL_PY + 36, 44, 30, c);
    gfx_draw_rect(RX, PAL_PY + 36, 44, 30, GFX_WHITE);

    if (c >= 16) {
        // три слайдера R/G/B, шаг = 1 уровень DAC (0..63)
        static const char*   names[3] = { "R", "G", "B" };
        static const uint8_t ncol[3]  = { GFX_LIGHT_RED, GFX_LIGHT_GREEN, GFX_LIGHT_BLUE };
        for (int k = 0; k < 3; k++) {
            int sy = PAL_PY + 78 + k * 24;
            gfx_draw_text(RX, sy + 4, names[k], ncol[k], BG);
            float fv = (float)pal6[c][k] / 63.0f;
            if (gui_slider(RX + 16, sy, 120, &fv)) {
                int ch[3] = { pal6[c][0], pal6[c][1], pal6[c][2] };
                ch[k] = (int)(fv * 63.0f + 0.5f);
                pal_set(c, ch[0], ch[1], ch[2]);
            }
            char nb[8]; num_to_str(pal6[c][k], nb);
            gfx_draw_text(RX + 16 + 120 + 8, sy + 4, nb, GFX_WHITE, BG);
        }
    } else {
        gfx_draw_text(RX, PAL_PY + 82, "UI color (locked)", GFX_LIGHT_GRAY, BG);
        gfx_draw_text(RX, PAL_PY + 94, "Pick 16..255 to", GFX_LIGHT_GRAY, BG);
        gfx_draw_text(RX, PAL_PY + 106, "edit its RGB", GFX_LIGHT_GRAY, BG);
    }

    if (gui_button(RX, PAL_PY + 160, 88, 20, "Reset pal"))
        pal_reset_default();
    if (gui_button(RX + 96, PAL_PY + 160, 56, 20, "Close"))
        *want_close = true;

    gfx_draw_text(RX, PAL_PY + 192, "VGA DAC: 6 bit (0-63)", GFX_LIGHT_GRAY, BG);
    gfx_draw_text(RX, PAL_PY + 204, "Palette saved in .bmp", GFX_LIGHT_GRAY, BG);
    gfx_draw_text(RX, PAL_PY + 216, "Esc = close panel",     GFX_LIGHT_GRAY, BG);
}

// ═══════════════════════════════════════════════════════════════════════════
//  BMP (8-bit, BI_RGB, без сжатия): load + save
//  Холст paint — 8-битные индексы текущей 256-цветной палитры.
//  save пишет в файл ВСЮ палитру (256 записей) и индексы холста как есть,
//  поэтому рисунок со своими цветами сохраняется и открывается точно.
//  load: цвета файла, которые реально используются в пикселях (индексы
//  16..255), загружаются в слоты палитры как есть — картинка выглядит как
//  оригинал. Индексы 0..15 (заняты интерфейсом) заменяются ближайшим цветом
//  из 256. 24-bit пока не читается (парсер возвращает код 5).
// ═══════════════════════════════════════════════════════════════════════════
// Один буфер на весь файл (load и save). 8-bit 640x480 = 1078 + 307200 байт.
#define BMP_BUF (14 + 40 + 256 * 4 + 640 * 480)
static uint8_t bmp_buf[BMP_BUF];

static uint16_t rd16(const uint8_t* p){ return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                              ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr16(uint8_t* p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
                                          p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static bool ends_with_bmp(const char* s) {
    int n = 0; while (s[n]) n++;
    if (n < 4) return false;
    const char* e = s + n - 4;
    return (e[0] == '.' && (e[1] == 'b' || e[1] == 'B')
                     && (e[2] == 'm' || e[2] == 'M')
                     && (e[3] == 'p' || e[3] == 'P'));
}

// Сообщения load/save в статус-бар.
static char  paint_msg[48] = {0};
static int   paint_msg_timer = 0;
static void set_paint_msg(const char* s) {
    int i = 0; for (; s[i] && i < 47; i++) paint_msg[i] = s[i];
    paint_msg[i] = 0; paint_msg_timer = 180;
}

// Читает файл целиком в bmp_buf (как в nano — по кластерной цепочке).
static uint32_t read_file_to_buf(const char* name, uint32_t max_bytes) {
    FAT32_FindResult r = fat32_find_entry(name, 0x00);
    if (!r.found) return 0xFFFFFFFFu;            // «не найден»
    uint32_t size = r.entry.file_size;
    uint32_t cluster = FAT32_GET_CLUSTER(&r.entry);
    uint32_t to_read = size < max_bytes ? size : max_bytes;
    uint32_t got = 0;
    while ((cluster & FAT32_MASK) < (FAT32_EOC & FAT32_MASK) && got < to_read) {
        uint32_t lba = fat32_cluster_to_lba(cluster);
        for (int sec = 0; sec < FAT32_SECTORS_PER_CLUSTER && got < to_read; sec++) {
            ata_read_sector(lba + sec);
            uint32_t chunk = 512;
            if (got + chunk > to_read) chunk = to_read - got;
            for (uint32_t i = 0; i < chunk; i++) bmp_buf[got + i] = sector_buffer[i];
            got += chunk;
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return got;                                   // 0xFFFFFFFF = не найден
}

// load: 0=ok, 2=не найден, 3=битый/не BMP, 4=сжатие, 5=не 8-bit.
static int load_bmp(const char* name, int canvas_h) {
    canvas_clear(canvas_h, GFX_WHITE);
    uint32_t got = read_file_to_buf(name, BMP_BUF);
    if (got == 0xFFFFFFFFu) return 2;
    if (got < 54) return 3;
    if (bmp_buf[0] != 'B' || bmp_buf[1] != 'M') return 3;
    uint32_t data_off = rd32(bmp_buf + 10);
    uint32_t hdr      = rd32(bmp_buf + 14);
    int32_t  width    = (int32_t)rd32(bmp_buf + 18);
    int32_t  height   = (int32_t)rd32(bmp_buf + 22);
    uint16_t bpp      = rd16(bmp_buf + 28);
    uint32_t comp     = rd32(bmp_buf + 30);
    if (comp != 0) return 4;
    if (bpp != 8)  return 5;
    int IH = height > 0 ? height : -height;
    bool bottomup = height > 0;
    int stride = (width + 3) & ~3;
    // палитра файла (B,G,R,0) → R,G,B (8 бит)
    uint8_t fpal[256][3];
    uint32_t pal_off = 14 + hdr;
    for (int i = 0; i < 256; i++) {
        uint32_t o = pal_off + (uint32_t)i * 4;
        if (o + 3 < got) { fpal[i][0] = bmp_buf[o + 2]; fpal[i][1] = bmp_buf[o + 1]; fpal[i][2] = bmp_buf[o]; }
        else { fpal[i][0] = fpal[i][1] = fpal[i][2] = 0; }
    }
    int rows = IH < canvas_h ? IH : canvas_h;
    int cols = width < 640 ? width : 640;
    if (width <= 0) cols = 0;

    // Проход 1: какие индексы палитры реально встречаются в пикселях.
    bool used[256];
    for (int i = 0; i < 256; i++) used[i] = false;
    for (int dy = 0; dy < rows; dy++) {
        int file_row = bottomup ? (IH - 1 - dy) : dy;
        uint32_t off = data_off + (uint32_t)file_row * (uint32_t)stride;
        if (off + (uint32_t)cols > got) continue;
        for (int dx = 0; dx < cols; dx++) used[bmp_buf[off + dx]] = true;
    }
    // Используемые цвета 16..255 переносим в палитру как есть.
    // Неиспользуемые слоты не трогаем (остаются свои/стандартные).
    for (int i = 16; i < 256; i++)
        if (used[i]) pal_set(i, fpal[i][0] >> 2, fpal[i][1] >> 2, fpal[i][2] >> 2);
    // Таблица перекодировки: 16..255 -> тот же индекс, 0..15 -> ближайший из 256.
    uint8_t map[256];
    for (int i = 0; i < 256; i++)
        map[i] = (i >= 16) ? (uint8_t)i : nearest_pal(fpal[i][0], fpal[i][1], fpal[i][2]);

    // Проход 2: заполняем холст (top-left, с обрезкой).
    for (int dy = 0; dy < rows; dy++) {
        int file_row = bottomup ? (IH - 1 - dy) : dy;
        uint32_t off = data_off + (uint32_t)file_row * (uint32_t)stride;
        if (off + (uint32_t)cols > got) continue;         // строка не прочиталась
        for (int dx = 0; dx < cols; dx++)
            canvas[dy * CANVAS_W + dx] = map[bmp_buf[off + dx]];
    }
    return 0;
}

// save: пишет холст (640 x canvas_h) как 8-bit BMP с текущей палитрой. true=ok.
static bool save_bmp(const char* name, int canvas_h) {
    const int W = 640;
    const int data_off = 14 + 40 + 256 * 4;       // 1078
    int img_bytes = W * canvas_h;
    int file_size = data_off + img_bytes;
    if (file_size > BMP_BUF) return false;
    bmp_buf[0] = 'B'; bmp_buf[1] = 'M';
    wr32(bmp_buf + 2, (uint32_t)file_size);
    wr32(bmp_buf + 6, 0);
    wr32(bmp_buf + 10, (uint32_t)data_off);
    wr32(bmp_buf + 14, 40);                       // BITMAPINFOHEADER
    wr32(bmp_buf + 18, (uint32_t)W);
    wr32(bmp_buf + 22, (uint32_t)canvas_h);       // >0 = bottom-up
    wr16(bmp_buf + 26, 1);
    wr16(bmp_buf + 28, 8);
    wr32(bmp_buf + 30, 0);                        // BI_RGB
    wr32(bmp_buf + 34, (uint32_t)img_bytes);
    wr32(bmp_buf + 38, 2835); wr32(bmp_buf + 42, 2835);
    wr32(bmp_buf + 46, 256);  wr32(bmp_buf + 50, 0);
    for (int i = 0; i < 256; i++) {               // вся палитра, B,G,R,0
        int o = 54 + i * 4;
        bmp_buf[o]     = p8(pal6[i][2]);
        bmp_buf[o + 1] = p8(pal6[i][1]);
        bmp_buf[o + 2] = p8(pal6[i][0]);
        bmp_buf[o + 3] = 0;
    }
    for (int r = 0; r < canvas_h; r++) {          // r = строка в файле (снизу)
        int img_row = canvas_h - 1 - r;
        uint8_t* dst = bmp_buf + data_off + r * W;
        const uint8_t* src = canvas + img_row * W;
        for (int x = 0; x < W; x++) dst[x] = src[x];
    }
    FAT32_FindResult ex = fat32_find_entry(name, 0x00);
    if (ex.found) fat32_delete_entry(name, 0x00); // иначе создастся дубликат имени
    bool ok = fat32_create_file(name, (const char*)bmp_buf, (uint32_t)file_size);
    return ok;
}

// Всплывающий бокс "Saving <name>... please wait", который рисуется в backbuffer
// и сразу выводится на экран ДО долгой синхронной записи на диск. Без затемнения
// всего экрана (иначе затрётся тулбар в backbuffer, а он уже не перерисуется):
// бокс лежит строго по центру = в зоне холста, поэтому после записи следующий
// кадр (blit_canvas + статус) затирает его начисто и сцена восстанавливается сама.
static void paint_saving_overlay(const char* name) {
    int W = gfx_get_width(), H = gfx_get_height();
    const char* l1 = "Saving";
    const char* l3 = "please wait...";
    char l2[30]; int i = 0;
    for (; name[i] && i < 28; i++) l2[i] = name[i];   // обрежем длинное имя
    l2[i] = 0;
    int len1 = (int)strlen(l1), len2 = (int)strlen(l2), len3 = (int)strlen(l3);
    int maxl = len1; if (len2 > maxl) maxl = len2; if (len3 > maxl) maxl = len3;
    int bw = maxl * 8 + 28;
    int bh = 3 * 12 + 16;
    int bx = (W - bw) / 2, by = (H - bh) / 2;
    // тень для "всплывающего" вида (тоже в зоне холста — затрётся вместе с боксом)
    gfx_fill_rect(bx + 3, by + 3, bw, bh, GFX_BLACK);
    // бокс + двойная рамка
    gfx_fill_rect(bx, by, bw, bh, GFX_BLUE);
    gfx_draw_rect(bx, by, bw, bh, GFX_WHITE);
    gfx_draw_rect(bx + 1, by + 1, bw - 2, bh - 2, GFX_LIGHT_BLUE);
    // три строки по центру
    gfx_draw_text(bx + (bw - len1 * 8) / 2, by + 10,      l1, GFX_YELLOW,     GFX_BLUE);
    gfx_draw_text(bx + (bw - len2 * 8) / 2, by + 10 + 12, l2, GFX_WHITE,      GFX_BLUE);
    gfx_draw_text(bx + (bw - len3 * 8) / 2, by + 10 + 24, l3, GFX_LIGHT_GRAY, GFX_BLUE);
    gfx_flip();   // показать ДО записи — пользователь видит паузу объяснённой
}

static void do_save(const char* name, int canvas_h) {
    if (name[0] == 0) { set_paint_msg("No name"); return; }
    paint_saving_overlay(name);                 // вывести оверлей на экран
    if (save_bmp(name, canvas_h)) set_paint_msg("Saved");
    else                          set_paint_msg("Save failed");
}

// ─── paint ───────────────────────────────────────────────────────────────────
void cmd_paint(const char* args) {
    vga_enter_mode_640x480();
    pal_reset_default();               // стандартная палитра + зеркало pal6
    int W = gfx_get_width(), H = gfx_get_height();
    int canvas_y = TOOLBAR_H;
    int canvas_h = H - TOOLBAR_H - STATUS_H;

    canvas_clear(canvas_h, GFX_WHITE);

    // Если аргумент — .bmp, грузим его в холст и запоминаем имя файла.
    char current_file[64] = {0};
    if (args && args[0]) {
        if (ends_with_bmp(args)) {
            int i = 0; for (; args[i] && i < 63; i++) current_file[i] = args[i];
            current_file[i] = 0;
            int code = load_bmp(args, canvas_h);
            if (code == 0) set_paint_msg("Loaded");
            else if (code == 2) set_paint_msg("Not found");
            else if (code == 4) set_paint_msg("Compressed BMP");
            else if (code == 5) set_paint_msg("Need 8-bit BMP");
            else set_paint_msg("Bad BMP");
        } else {
            set_paint_msg("Not a .bmp");
        }
    }

    gfx_cursor_move(W / 2, H / 2);
    gfx_cursor_show();

    static int s_phys_x = 0, s_phys_y = 0;
    s_phys_x = mouse_state.x; s_phys_y = mouse_state.y;

    uint8_t cur_color = GFX_BLACK;
    float   pen_v     = 2.0f / (float)(PEN_MAX - PEN_MIN);   // старт: размер 3
    int     pen_size  = pen_from_v(pen_v);
    int     prev_draw_x = -1, prev_draw_y = -1;
    bool    was_drawing = false;
    bool    need_redraw = true;
    bool    show_pal    = false;   // открыта ли панель "Colors"
    bool    ui_press    = false;   // ЛКМ нажата на интерфейсе — холст не рисуем

    // Мини-режим ввода имени для Save as (строка в статус-баре).
    bool save_naming = false;
    char save_name[64] = {0};
    int  save_caret = 0;

    while (true) {
        uint8_t key = get_key();
        gui_begin_frame(key);

        // ── ввод имени для Save as (перехват клавиатуры) ──
        bool was_naming = save_naming;
        if (save_naming) {
            if (key == 27) { save_naming = false; }
            else if (key == '\n') {
                if (save_name[0]) {
                    int i = 0; for (; save_name[i] && i < 63; i++) current_file[i] = save_name[i];
                    current_file[i] = 0;
                    do_save(current_file, canvas_h);
                }
                save_naming = false;
            } else if (key == '\b') {
                if (save_caret > 0) { save_caret--; save_name[save_caret] = 0; }
            } else if (key >= 32 && key <= 126 && save_caret < 63) {
                save_name[save_caret++] = (char)key; save_name[save_caret] = 0;
            }
            need_redraw = true;
        }
        if (key == 27 && !was_naming) {            // Esc: закрыть панель, иначе выйти
            if (show_pal) { show_pal = false; need_redraw = true; }
            else break;
        }

        // Дельта курсора
        int cur_phys_x = mouse_state.x, cur_phys_y = mouse_state.y;
        int dx = cur_phys_x - s_phys_x, dy = cur_phys_y - s_phys_y;
        s_phys_x = cur_phys_x; s_phys_y = cur_phys_y;
        bool cursor_moved = false;
        if (dx != 0 || dy != 0) {
            int nx = gfx_cursor_get_x() + dx;
            int ny = gfx_cursor_get_y() + dy;
            if (nx < 0) nx = 0; if (nx >= W) nx = W - 1;
            if (ny < 0) ny = 0; if (ny >= H) ny = H - 1;
            gfx_cursor_move(nx, ny);
            cursor_moved = true;
        }
        int cx = gfx_cursor_get_x(), cy = gfx_cursor_get_y();

        // Рисование на холсте (LMB)
        bool lmb = (mouse_state.buttons & 0x01) != 0;
        bool rmb = (mouse_state.buttons & 0x02) != 0;
        bool in_canvas = (cx >= 0 && cx < W && cy >= canvas_y &&
                          cy < canvas_y + canvas_h);
        bool over_panel = show_pal && pal_panel_hit(cx, cy);
        // Если нажали ЛКМ на тулбаре/панели — тянем слайдер/кнопку, а не рисуем.
        if (gui_click_edge()) ui_press = (!in_canvas || over_panel);
        if (!lmb) ui_press = false;
        bool can_draw = in_canvas && !over_panel;
        bool drew = false;
        if (((lmb && !ui_press) || rmb) && can_draw) {
            uint8_t draw_color = rmb ? GFX_WHITE : cur_color;
            int ccx = cx, ccy = cy - canvas_y;
            if (was_drawing && prev_draw_x >= 0) {
                canvas_line(prev_draw_x, prev_draw_y,
                            ccx, ccy, canvas_h, pen_size, draw_color);
            } else {
                canvas_draw(ccx, ccy, canvas_h, pen_size, draw_color);
            }
            prev_draw_x = ccx; prev_draw_y = ccy;
            was_drawing = true;
            drew = true;
            need_redraw = true;
        } else {
            was_drawing = false;
            prev_draw_x = -1; prev_draw_y = -1;
        }

        // ВАЖНО (фикс клика): тулбар/палитра опрашиваются только внутри блока
        // перерисовки, а фронт клика живёт один кадр — поэтому входим в блок и
        // когда кнопка нажата/отпущена, иначе клик по неподвижной мыши терялся.
        if (need_redraw || cursor_moved || drew || lmb || rmb ||
            gui_click_edge() || gui_release_edge()) {
            need_redraw = false;
            bool clear_clicked = false;
            bool save_clicked  = false;
            bool toggle_pal    = false;
            uint8_t new_color = cur_color;
            draw_toolbar(W, cur_color, &pen_v, pen_size, show_pal,
                         &clear_clicked, &new_color, &save_clicked, &toggle_pal);
            pen_size = pen_from_v(pen_v);
            if (clear_clicked) canvas_clear(canvas_h, GFX_WHITE);
            if (save_clicked) {
                if (current_file[0]) do_save(current_file, canvas_h);
                else { save_naming = true; save_name[0] = 0; save_caret = 0; }
            }
            if (toggle_pal) show_pal = !show_pal;

            blit_canvas(W, canvas_y, canvas_h);

            // Панель "Colors" рисуется поверх холста
            if (show_pal) {
                bool want_close = false;
                draw_palette_panel(&new_color, &want_close);
                if (want_close) { show_pal = false; need_redraw = true; }
            }
            if (new_color != cur_color) { cur_color = new_color; need_redraw = true; }

            // Статус-бар
            gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, GFX_BLACK);
            if (save_naming) {
                gfx_draw_text(4, H - STATUS_H + 2, "Save as: ",
                              GFX_YELLOW, GFX_BLACK);
                gfx_draw_text(4 + 9 * 8, H - STATUS_H + 2, save_name,
                              GFX_WHITE, GFX_BLACK);
                gfx_draw_text(4 + (9 + save_caret) * 8, H - STATUS_H + 2, "_",
                              GFX_YELLOW, GFX_BLACK);
                gfx_draw_text(W - 12 * 8, H - STATUS_H + 2, "Enter/Esc",
                              GFX_LIGHT_GRAY, GFX_BLACK);
            } else {
                const char* left = (paint_msg_timer > 0) ? paint_msg
                                   : "LMB=Draw RMB=Erase Esc=Exit";
                gfx_draw_text(4, H - STATUS_H + 2, left,
                              (paint_msg_timer > 0) ? GFX_LIGHT_GREEN : GFX_LIGHT_GRAY,
                              GFX_BLACK);
                if (current_file[0])
                    gfx_draw_text(W - 16 * 8, H - STATUS_H + 2, current_file,
                                  GFX_LIGHT_CYAN, GFX_BLACK);
            }
            gfx_flip();
        }
        if (paint_msg_timer > 0) paint_msg_timer--;
    }
    gfx_cursor_hide();
    vga_exit_to_text();
    shell_clear_buffer();
    shell_redraw_full();
}

// ─── image: показать .bmp на весь экран (без редактирования), Esc — выход ───
void cmd_image(const char* name) {
    if (!name || !name[0]) { println("Usage: image <file.bmp>"); return; }
    vga_enter_mode_640x480();
    pal_reset_default();                   // зеркало палитры; load_bmp подставит цвета файла
    int W = gfx_get_width(), H = gfx_get_height();
    int code = load_bmp(name, H);          // весь экран как холст
    blit_canvas(W, 0, H);
    if (code != 0) {
        const char* m = (code == 2) ? "Not found"
                      : (code == 4) ? "Compressed BMP"
                      : (code == 5) ? "Need 8-bit BMP" : "Bad BMP";
        gfx_fill_rect(0, H - 12, W, 12, GFX_BLACK);
        gfx_draw_text(4, H - 10, m, GFX_LIGHT_RED, GFX_BLACK);
    }
    gfx_flip();
    while (true) { if (get_key() == 27) break; }   // ждём Esc
    vga_exit_to_text();
    shell_clear_buffer();
    shell_redraw_full();
}