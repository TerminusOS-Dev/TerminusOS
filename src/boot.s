// Multiboot Header 
.intel_syntax noprefix

.section .multiboot
.align 4
.long 0x1BADB002
.long 0x00000004
.long -(0x1BADB002 + 0x00000004)
.long 0, 0, 0, 0, 0
.long 0
.long 640
.long 480
.long 32

// Entry Point
.global _start
.section .text
_start:
    cli
    push ebx
    call kmain
    hlt
