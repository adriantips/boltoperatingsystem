#pragma once
#include <stdint.h>

/* ring-3 selectors (see kernel/gdt.c ordering) */
#define USER_CS 0x23
#define USER_SS 0x1B

/* syscall numbers (rax) */
#define SYS_WRITE  1   /* (ptr, len)  -> bytes written to serial */
#define SYS_YIELD  2   /* ()          -> 0                       */
#define SYS_GETPID 3   /* ()          -> pid                     */

void     syscall_init(void);                 /* program STAR/LSTAR/FMASK/EFER.SCE */
void     syscall_entry(void);                /* asm prologue (kernel/syscall.asm) */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);
