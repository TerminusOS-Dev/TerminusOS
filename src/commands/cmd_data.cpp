#include "cmd_data.h"
#include "../lib/screen.h"
#include "../drivers/rtc.h"

void cmd_date() {
    RTC_Time time = rtc_read_time();
    
    // Вывод даты
    print("Date: ");
    if (time.day < 10) print("0");
    print_hex_byte(time.day);
    print("/");
    if (time.month < 10) print("0");
    print_hex_byte(time.month);
    print("/");
    
    // Год (уже в формате 2026)
    char year_str[5];
    year_str[0] = '0' + (time.year / 1000);
    year_str[1] = '0' + ((time.year / 100) % 10);
    year_str[2] = '0' + ((time.year / 10) % 10);
    year_str[3] = '0' + (time.year % 10);
    year_str[4] = '\0';
    print(year_str);
    
    // Вывод времени
    char time_str[16];
    rtc_get_time_string(time_str);
    print(" ");
    println(time_str);
}
