#include "mouse.h"
#include "../lib/screen.h"
#include "../drivers/serial.h"
#include "../drivers/vga_gfx.h"

#define MOUSE_DATA_PORT 0x60
#define MOUSE_STATUS_PORT 0x64
#define MOUSE_COMMAND_PORT 0x64

// PS/2 контроллер команды
#define PS2_WRITE_CCB 0x60
#define PS2_READ_CCB 0x20
#define PS2_DISABLE_MOUSE 0xA7
#define PS2_ENABLE_MOUSE 0xA8
#define PS2_WRITE_TO_MOUSE 0xD4

// Команды мыши
#define MOUSE_RESET 0xFF
#define MOUSE_ENABLE_DATA_REPORTING 0xF4
#define MOUSE_DISABLE_DATA_REPORTING 0xF5
#define MOUSE_SET_SAMPLE_RATE 0xF3

MouseState mouse_state = {320, 240, 0, 0, 0};

static uint8_t mouse_packet[3];
static uint8_t packet_index = 0;
static int packet_ready = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void wait_output_buffer() {
    for (int i = 0; i < 100000; i++) {
        if ((inb(MOUSE_STATUS_PORT) & 0x02) == 0) return;
    }
}

static void wait_input_buffer() {
    for (int i = 0; i < 100000; i++) {
        if (inb(MOUSE_STATUS_PORT) & 0x01) return;
    }
}

static void mouse_send_command(uint8_t cmd) {
    wait_output_buffer();
    outb(MOUSE_COMMAND_PORT, PS2_WRITE_TO_MOUSE);
    wait_output_buffer();
    outb(MOUSE_DATA_PORT, cmd);
}

static uint8_t mouse_read_response() {
    wait_input_buffer();
    return inb(MOUSE_DATA_PORT);
}

void mouse_init() {
    println("Mouse: Loading ...");
    serial_write("[MOUSE] init start\n");

    // Сброс: контроллер шлёт 3 байта — ACK(0xFA), BAT(0xAA), Device ID(0x00).
    // Раньше Device ID не читался -> все следующие ACK сдвигались на 1,
    // и в буфере навсегда оставался 1 непрочитанный AUX-байт.
    mouse_send_command(MOUSE_RESET);
    uint8_t r_reset_ack = mouse_read_response();
    uint8_t r_bat       = mouse_read_response();
    uint8_t r_devid     = mouse_read_response();
    serial_write("[MOUSE] reset ack=");
    serial_hex8(r_reset_ack);
    serial_write(" bat=");
    serial_hex8(r_bat);
    serial_write(" devid=");
    serial_hex8(r_devid);
    serial_write("\n");

    // Отключаем потоковый режим
    mouse_send_command(MOUSE_DISABLE_DATA_REPORTING);
    uint8_t r_dis = mouse_read_response();

    // Устанавливаем частоту дискретизации
    mouse_send_command(MOUSE_SET_SAMPLE_RATE);
    uint8_t r_sr1 = mouse_read_response();
    mouse_send_command(100); // 100 Hz — было 40, мало для быстрых движений
    uint8_t r_sr2 = mouse_read_response();

    // Включаем передачу данных
    mouse_send_command(MOUSE_ENABLE_DATA_REPORTING);
    uint8_t r_en = mouse_read_response();

    serial_write("[MOUSE] dis=");
    serial_hex8(r_dis);
    serial_write(" rate1=");
    serial_hex8(r_sr1);
    serial_write(" rate2=");
    serial_hex8(r_sr2);
    serial_write(" en=");
    serial_hex8(r_en);
    serial_write("\n");

    // ── Включаем IRQ12 в Configuration Byte контроллера 8042 ───────────────
    // Без этого бита 8042 НИКОГДА не выставит линию IRQ12, даже если PIC
    // её не маскирует — AUX-байты копятся в OBF и блокируют буфер целиком
    // (и для мыши, и для клавиатуры, т.к. буфер общий).
    wait_output_buffer();
    outb(MOUSE_COMMAND_PORT, PS2_READ_CCB);
    uint8_t ccb_old = mouse_read_response();

    uint8_t ccb_new = ccb_old | 0x02;  // бит1: разрешить IRQ12 (мышь)
    ccb_new &= ~0x20;                  // бит5: clock мыши включен

    wait_output_buffer();
    outb(MOUSE_COMMAND_PORT, PS2_WRITE_CCB);
    wait_output_buffer();
    outb(MOUSE_DATA_PORT, ccb_new);

    serial_write("[MOUSE] ccb ");
    serial_hex8(ccb_old);
    serial_write(" -> ");
    serial_hex8(ccb_new);
    serial_write("\n");

    packet_index = 0;
    packet_ready = 0;
    println("Mouse initialized");
    serial_write("[MOUSE] init done\n");
}

void mouse_handle_packet() {
    uint8_t status = inb(MOUSE_STATUS_PORT);

    if (!(status & 0x01)) return;   // нет данных в буфере

    // Читаем байт ВСЕГДА (чтобы освободить буфер)
    uint8_t data = inb(MOUSE_DATA_PORT);

    // Бит 5 = 0 означает данные от клавиатуры попали сюда — игнорируем
    if (!(status & 0x20)) return;

    // Первый байт должен иметь бит 3 установленным (всегда 1 в стандартном пакете)
    if (packet_index == 0 && !(data & 0x08)) {
        return;
    }

    mouse_packet[packet_index++] = data;

    if (packet_index == 3) {
        packet_index = 0;

        uint8_t buttons = mouse_packet[0] & 0x07;
        int8_t dx = (int8_t)mouse_packet[1];
        int8_t dy = (int8_t)mouse_packet[2];

        // Overflow: clamp to max instead of zeroing (keeps fast flicks)
        if (mouse_packet[0] & 0x40) dx = (dx < 0) ? -128 : 127;
        if (mouse_packet[0] & 0x80) dy = (dy < 0) ? -128 : 127;

        // Accumulate raw deltas (userspace reads & clears via syscall)
        mouse_state.raw_dx += dx;
        mouse_state.raw_dy += -dy;

        mouse_state.x += dx;
        mouse_state.y -= dy;

        int scr_w = gfx_get_width();
        int scr_h = gfx_get_height();
        if (scr_w <= 0) scr_w = 640;
        if (scr_h <= 0) scr_h = 480;

        int max_x = scr_w - 1;
        int max_y = scr_h - 1;
        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;

        if (mouse_state.x < 0) mouse_state.x = 0;
        if (mouse_state.x > max_x) mouse_state.x = max_x;
        if (mouse_state.y < 0) mouse_state.y = 0;
        if (mouse_state.y > max_y) mouse_state.y = max_y;

        mouse_state.buttons = buttons;
        packet_ready = 1;
    }
}

int mouse_has_packet() {
    return packet_ready;
}

void mouse_clear_packet() {
    packet_ready = 0;
}
