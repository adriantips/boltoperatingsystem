#include <stdint.h>
#include "sched.h"
#include "vmm.h"
#include "kheap.h"
#include "string.h"

#define MAXT    16
#define KSTACK  16384          /* per-thread kernel stack */

typedef struct {
    uint64_t rsp;              /* saved register-frame pointer when not running */
    uint64_t cr3;              /* address space (PML4 physical)                 */
    int      used;
    char     name[16];
} kthread_t;

static kthread_t th[MAXT];
static int       cur      = 0;
static int       nthreads = 0;
static int       enabled  = 0;
static uint64_t  nswitch  = 0;

void sched_init(void) {
    th[0].used = 1;
    th[0].cr3  = vmm_kernel_pml4();
    strncpy(th[0].name, "kmain", sizeof th[0].name);   /* current context */
    cur = 0;
    nthreads = 1;
    irq_set_tick_hook(schedule);
    enabled = 1;
}

int sched_add(void (*entry)(void), const char *name) {
    if (nthreads >= MAXT) return -1;
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK);
    if (!stack) return -1;

    /* Hand-craft an initial register frame at the top of the new stack so the
     * ISR epilogue (pop GPRs; iretq) starts the thread at entry() in ring 0. */
    uint64_t top = ((uint64_t)stack + KSTACK) & ~0xFull;
    struct registers *f = (struct registers *)(top - sizeof(struct registers));
    memset(f, 0, sizeof *f);
    f->rip    = (uint64_t)entry;
    f->cs     = 0x08;          /* kernel code */
    f->rflags = 0x202;         /* IF set */
    f->rsp    = top;           /* thread's own stack when running */
    f->ss     = 0x10;          /* kernel data */

    int i = nthreads++;
    th[i].rsp  = (uint64_t)f;
    th[i].cr3  = vmm_kernel_pml4();
    th[i].used = 1;
    strncpy(th[i].name, name, sizeof th[i].name);
    return i;
}

int sched_add_user(uint64_t rip, uint64_t ustack_top, uint64_t cr3, const char *name) {
    if (nthreads >= MAXT) return -1;
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK);    /* kernel stack for first entry */
    if (!stack) return -1;

    /* initial frame is already a ring-3 frame: the ISR epilogue's iretq performs
     * the ring0->ring3 transition straight into the user program. */
    uint64_t top = ((uint64_t)stack + KSTACK) & ~0xFull;
    struct registers *f = (struct registers *)(top - sizeof(struct registers));
    memset(f, 0, sizeof *f);
    f->rip    = rip;
    f->cs     = 0x23;          /* user code, RPL 3 */
    f->rflags = 0x202;         /* IF set */
    f->rsp    = ustack_top;
    f->ss     = 0x1B;          /* user data, RPL 3 */

    int i = nthreads++;
    th[i].rsp  = (uint64_t)f;
    th[i].cr3  = cr3;
    th[i].used = 1;
    strncpy(th[i].name, name, sizeof th[i].name);
    return i;
}

struct registers *schedule(struct registers *r) {
    if (!enabled || nthreads < 2) return r;
    th[cur].rsp = (uint64_t)r;              /* save outgoing context */
    cur = (cur + 1) % nthreads;             /* round robin (slots are contiguous) */
    nswitch++;
    vmm_switch(th[cur].cr3);                /* activate incoming address space */
    return (struct registers *)th[cur].rsp; /* restore incoming context */
}

int         sched_count(void)     { return nthreads; }
int         sched_current(void)   { return cur; }
const char *sched_name(int i)     { return (i >= 0 && i < nthreads) ? th[i].name : ""; }
uint64_t    sched_switches(void)  { return nswitch; }
