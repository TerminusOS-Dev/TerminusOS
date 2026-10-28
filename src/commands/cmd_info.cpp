#include "cmd_info.h"
#include "../lib/screen.h"
#include "../kernel/version.h"

void cmd_info() {
    println("================================================================================");
    println("             _____ ____ ___  __  __ ___ _  _ _   _ ___   ___  ___");
    println("            |_   _| __|| _ \\|  \\/  |_ _| \\| | | / __| / _ \\/ __|");
    println("              | | | _| |   /| |\\/| || || .` | |_\\__ \\| (_) \\__ \\");
    println("              |_| |___|_|_\\|_|  |_|___|_|\\_|____||___/ \\___/|___/");
    println("                                 v" OS_VERSION " " OS_CODENAME);
    println("================================================================================");
    println("");
    println("  Youtube: Zero-Logic-dev  |  Telegram: @den2010991  |  GitHub: deniss123va");
    println("");
    println("  [" OS_VERSION " " OS_CODENAME "] UPDATES");
    for (int i = 0; OS_UPDATES[i] != nullptr; i++) {
            print("  - ");
            println(OS_UPDATES[i]);
        }
    println("  Type 'help' for all commands");
    println("================================================================================");
}