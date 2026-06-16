#include <stdint.h>
#include "interrupts.h"
#include "kprintf.h"
#include "pic.h"
#include "io.h"

static irq_handler_t handlers[16];
static tick_hook_t   tick_hook;

void irq_install(int irq, irq_handler_t h) {
    if (irq >= 0 && irq < 16) handlers[irq] = h;
}

void irq_set_tick_hook(tick_hook_t h) { tick_hook = h; }

static const char *exc[20] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND range", "Invalid opcode", "Device not available", "Double fault",
    "Coproc segment", "Invalid TSS", "Segment not present", "Stack-segment",
    "General protection", "Page fault", "Reserved", "x87 FP",
    "Alignment check", "Machine check", "SIMD FP"
};

struct registers *isr_handler(struct registers *r) {
    if (r->int_no < 32) {
        kprintf("\n*** CPU EXCEPTION #%lu", r->int_no);
        if (r->int_no < 20) kprintf(" (%s)", exc[r->int_no]);
        kprintf("\n    err=0x%lx  rip=0x%lx  rflags=0x%lx\n",
                r->err_code, r->rip, r->rflags);
        if (r->int_no == 14) kprintf("    cr2=0x%lx (faulting address)\n", read_cr2());
        kprintf("    System halted.\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    if (r->int_no >= 32 && r->int_no < 48) {
        int irq = (int)r->int_no - 32;
        if (handlers[irq]) handlers[irq](r);
        pic_eoi(irq);
        /* preemptive reschedule on the timer tick; may return a new frame */
        if (irq == 0 && tick_hook) r = tick_hook(r);
    }
    return r;
}
