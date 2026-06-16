#pragma once
#include <stdint.h>
#include "interrupts.h"

/* Minimal round-robin preemptive scheduler over kernel threads. The timer
 * IRQ0 calls schedule() (installed as the tick hook); it saves the outgoing
 * thread's register frame and returns the incoming thread's frame so the ISR
 * epilogue restores it. All threads run in ring 0 sharing the kernel CR3. */
void sched_init(void);                              /* adopt current context as thread 0 */
int  sched_add(void (*entry)(void), const char *name);
/* Ring-3 thread: starts at user rip on user stack in address space cr3. */
int  sched_add_user(uint64_t rip, uint64_t ustack_top, uint64_t cr3, const char *name);
struct registers *schedule(struct registers *r);   /* tick hook */

int          sched_count(void);
int          sched_current(void);
const char  *sched_name(int i);
uint64_t     sched_switches(void);
