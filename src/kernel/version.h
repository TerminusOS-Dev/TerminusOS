#ifndef TERMINUS_VERSION_H
#define TERMINUS_VERSION_H

#define OS_NAME        "TerminusOS"
#define OS_CODENAME    "Altair"
#define OS_VERSION     "4.9a"
#define OS_RELEASE     "4a"
#define OS_ARCH        "i386"

#define OS_WELCOME_MSG "Welcome to " OS_NAME " " OS_VERSION " " OS_CODENAME "!"

static const char* OS_UPDATES[] = {
    "New Editor GITE (Graphic Terminus Editor)",
    "Added BMP image viewing and edit support",
    "Minor bug fixes in the bootloader",
    "New version numbering scheme",
    "Linux i386 ABI & ELF executable parsing",
    "VBE 256-color graphics & double buffering",
    nullptr
};

#endif