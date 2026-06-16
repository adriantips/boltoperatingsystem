#include <stdint.h>
#include "syscall.h"
#include "serial.h"
#include "mm.h"

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* dedicated kernel stack for the SYSCALL prologue (IF is masked during a
 * syscall, so a single global stack is safe for one ring-3 task). */
static uint8_t   syscall_stack[16384] __attribute__((aligned(16)));
uint64_t         syscall_kstack_top;     /* read by kernel/syscall.asm */
uint64_t         syscall_user_rsp;       /* scratch save of user RSP    */

void syscall_init(void) {
    syscall_kstack_top = (uint64_t)(syscall_stack + sizeof syscall_stack);

    /* STAR[63:48]=0x10 -> SYSRET SS=0x18|3, CS=0x20|3 (user data/code).
     * STAR[47:32]=0x08 -> SYSCALL CS=0x08, SS=0x10 (kernel code/data).      */
    wrmsr(IA32_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(IA32_FMASK, 0x200);                       /* clear IF on entry */
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1);         /* EFER.SCE */
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a3;
    switch (num) {
    case SYS_WRITE: {
        const char *p = (const char *)a1;
        if (a1 >= PHYS_BASE) return (uint64_t)-1;    /* reject non-user pointers */
        for (uint64_t i = 0; i < a2; i++) serial_putc(p[i]);
        return a2;
    }
    case SYS_YIELD:  return 0;
    case SYS_GETPID: return 1;
    default:         return (uint64_t)-1;
    }
}
