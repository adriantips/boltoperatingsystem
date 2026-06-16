#pragma once
#include <stdint.h>
#include "boot.h"

void     pmm_init(struct bootinfo *bi);
uint64_t pmm_alloc_frame(void);              /* returns physical addr, 0 = OOM */
void     pmm_free_frame(uint64_t addr);
void     pmm_reserve_range(uint64_t base, uint64_t len);
uint64_t pmm_free_count(void);               /* free frames */
uint64_t pmm_total_count(void);
