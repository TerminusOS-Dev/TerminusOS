#include "cmd_registry.h"
#include "../lib/screen.h"
#include "../lib/string.h"

// ─── Подключаем все команды ───────────────────────────────────────────────────
#include "cmd_clear.h"
#include "cmd_help.h"
#include "cmd_data.h"
#include "cmd_info.h"
#include "cmd_nano.h"
#include "cmd_calc.h"
#include "cmd_wc.h"
#include "cmd_head.h"
#include "cmd_hexdump.h"
#include "cmd_uptime.h"
#include "cmd_banner.h"
#include "cmd_panic.h"
#include "cmd_meminfo.h"
#include "cmd_paint.h"
#include "cmd_widgets.h"
#include "cmd_gite.h"
#include "cmd_run.h"

// Команды из builtin (shell/builtin.h) — сигнатуры char*, не const char*
extern void cmd_fat_check();
extern void cmd_ls_disk();
extern void cmd_disk_cat(char* name);
extern void cmd_pwd();
extern void cmd_history();
extern void cmd_cd(char* path);
extern void cmd_mkdir(char* name);
extern void cmd_rm(char* name);
extern void cmd_mv(char* args);
extern void cmd_cp(char* args);
extern void cmd_create(char* name);
extern void cmd_read_disk();
extern void fat_format_disk();
extern void cmd_reboot();
extern void cmd_shutdown();
extern void cmd_theme(char* name);
extern void cmd_settings(const char* args);
extern void cmd_panic();
extern void cmd_meminfo();

// Обёртки char* → const char* для таблицы
static void wrap_disk_cat(const char* a){ cmd_disk_cat((char*)a); }
static void wrap_cd      (const char* a){ cmd_cd      ((char*)a); }
static void wrap_mkdir   (const char* a){ cmd_mkdir   ((char*)a); }
static void wrap_rm      (const char* a){ cmd_rm      ((char*)a); }
static void wrap_mv      (const char* a){ cmd_mv      ((char*)a); }
static void wrap_cp      (const char* a){ cmd_cp      ((char*)a); }
static void wrap_create  (const char* a){ cmd_create  ((char*)a); }
static void wrap_theme   (const char* a){ cmd_theme   ((char*)a); }
static void wrap_nano    (const char* a){ cmd_nano    ((char*)a); }

// ─── Таблица команд ───────────────────────────────────────────────────────────
//  { "name", ARG_TYPE, fn_noarg, fn_arg, "Usage: ..." }
//
//  ARG_NONE  — fn_noarg вызывается напрямую, аргумент игнорируется
//  ARG_TEXT  — fn_arg вызывается с аргументом; если пусто — печатается usage
//  ARG_FILE  — fn_arg вызывается с аргументом; таб предлагает файлы; usage если пусто
//  ARG_OPT   — fn_arg вызывается всегда (даже с пустой строкой)

const CmdEntry CMD_TABLE[] = {
    // name         arg_type   fn_noarg        fn_arg                    usage
    { "help",      ARG_NONE,  cmd_help,       nullptr,                  nullptr },
    { "clear",     ARG_NONE,  cmd_clear,      nullptr,                  nullptr },
    { "ls",        ARG_NONE,  cmd_ls_disk,    nullptr,                  nullptr },
    { "pwd",       ARG_NONE,  cmd_pwd,        nullptr,                  nullptr },
    { "history",   ARG_NONE,  cmd_history,    nullptr,                  nullptr },
    { "date",      ARG_NONE,  cmd_date,       nullptr,                  nullptr },
    { "fatcheck",  ARG_NONE,  cmd_fat_check,  nullptr,                  nullptr },
    { "read",      ARG_NONE,  cmd_read_disk,  nullptr,                  nullptr },
    { "fsd",       ARG_NONE,  fat_format_disk,nullptr,                  nullptr },
    { "reboot",    ARG_NONE,  cmd_reboot,     nullptr,                  nullptr },
    { "shutdown",  ARG_NONE,  cmd_shutdown,   nullptr,                  nullptr },
    { "exit",      ARG_NONE,  cmd_shutdown,   nullptr,                  nullptr },
    { "meminfo",   ARG_NONE,  cmd_meminfo,    nullptr,                  nullptr },
    { "info",      ARG_NONE,  cmd_info,       nullptr,                  nullptr },
    { "uptime",    ARG_NONE,  cmd_uptime,     nullptr,                  nullptr },
    { "panic",     ARG_NONE,  cmd_panic,       nullptr,                  nullptr },
    { "paint",     ARG_OPT,   nullptr,        cmd_paint,                "paint [640]" },
    { "image", ARG_FILE, nullptr, (void(*)(const char*))cmd_image, "image <file.bmp> - view BMP" },
    { "widgets",  ARG_OPT,   nullptr,        cmd_widgets,              "widgets [640]" },

    { "echo",      ARG_TEXT,  nullptr, [](const char* a){ println(a); }, "Usage: echo <text>" },
    { "theme",     ARG_OPT,   nullptr, wrap_theme,                       nullptr },
    { "settings",  ARG_OPT,   nullptr, cmd_settings,                     nullptr },
    { "calc",      ARG_TEXT,  nullptr, cmd_calc,  "Usage: calc <expr>  e.g. calc 2+2*3" },
    { "banner",    ARG_TEXT,  nullptr, cmd_banner,"Usage: banner <text>" },

    { "cat",       ARG_FILE,  nullptr, wrap_disk_cat, "Usage: cat <file>" },
    { "nano",      ARG_FILE,  nullptr, wrap_nano,   "Usage: nano <file>" },
    { "gite", ARG_OPT, nullptr, (void(*)(const char*))cmd_gite, "gite [file] - graphical editor" },
    { "run", ARG_FILE, nullptr, (void(*)(const char*))cmd_run, "Usage: run [-d] <elf-file>" },
    { "edit",      ARG_FILE,  nullptr, wrap_nano,   "Usage: edit <file>" },
    { "create",    ARG_FILE,  nullptr, wrap_create, "Usage: create <file>" },
    { "rm",        ARG_FILE,  nullptr, wrap_rm,    "Usage: rm <file>" },
    { "cd",        ARG_FILE,  nullptr, wrap_cd,    "Usage: cd <dir>" },
    { "mkdir",     ARG_FILE,  nullptr, wrap_mkdir, "Usage: mkdir <dir>" },
    { "wc",        ARG_FILE,  nullptr, cmd_wc,    "Usage: wc <file>" },
    { "head",      ARG_FILE,  nullptr, cmd_head,  "Usage: head <file> [N]" },
    { "hexdump",   ARG_TEXT,  nullptr, cmd_hexdump,"Usage: hexdump <file> [-f <out.txt> | -c <in.txt>]" },

    { "mv",        ARG_TEXT,  nullptr, wrap_mv,    "Usage: mv <src> <dst>" },
    { "cp",        ARG_TEXT,  nullptr, wrap_cp,    "Usage: cp <src> <dst>" },
};

const int CMD_TABLE_SIZE = sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0]);

// ─── Вспомогательные ─────────────────────────────────────────────────────────

static int levenshtein(const char* a, const char* b) {
    int la = strlen(a), lb = strlen(b);
    if (la > 32) la = 32;
    if (lb > 32) lb = 32;
    static int dp[33][33];
    for (int i = 0; i <= la; i++) dp[i][0] = i;
    for (int j = 0; j <= lb; j++) dp[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int cost = (a[i-1] == b[j-1]) ? 0 : 1;
            int del  = dp[i-1][j] + 1;
            int ins  = dp[i][j-1] + 1;
            int sub  = dp[i-1][j-1] + cost;
            dp[i][j] = del < ins ? (del < sub ? del : sub)
                                 : (ins < sub ? ins : sub);
        }
    return dp[la][lb];
}

// ─── Главный диспетчер ────────────────────────────────────────────────────────

void cmd_dispatch(const char* buffer) {
    if (!buffer || !buffer[0]) return;

    // Разбиваем на cmd + args
    char cmd[64] = {0};
    int ci = 0;
    while (buffer[ci] && buffer[ci] != ' ' && ci < 63) { cmd[ci] = buffer[ci]; ci++; }
    cmd[ci] = 0;

    // Пропускаем пробелы после команды
    const char* args = buffer + ci;
    while (*args == ' ') args++;
    // args теперь указывает на аргументы без ведущих пробелов

    // Ищем в таблице
    for (int i = 0; i < CMD_TABLE_SIZE; i++) {
        if (strcmp(cmd, CMD_TABLE[i].name) != 0) continue;

        const CmdEntry& e = CMD_TABLE[i];

        switch (e.arg_type) {
            case ARG_NONE:
                e.fn_noarg();
                break;

            case ARG_OPT:
                e.fn_arg(args);   // вызываем даже с пустой строкой
                break;

            case ARG_TEXT:
            case ARG_FILE:
                if (*args == '\0') {
                    if (e.usage) println(e.usage);
                } else {
                    e.fn_arg(args);
                }
                break;
        }
        return;
    }

    // Команда не найдена — Левенштейн
    const char* best = nullptr;
    int best_dist = 99;
    for (int i = 0; i < CMD_TABLE_SIZE; i++) {
        int d = levenshtein(cmd, CMD_TABLE[i].name);
        if (d < best_dist) { best_dist = d; best = CMD_TABLE[i].name; }
    }
    print("Unknown command: "); println(cmd);
    if (best && best_dist <= 3) { print("  Did you mean: "); println(best); }
}