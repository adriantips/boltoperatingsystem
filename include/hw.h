#pragma once
#include <stdint.h>

/* Low-level hardware introspection: CPUID, CMOS real-time clock, PCI config
 * space. All real reads of real silicon (works under QEMU). */

/* --- CPUID ---------------------------------------------------------------- */
static inline void cpuidx(uint32_t leaf, uint32_t sub,
                          uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}
void hw_cpu_vendor(char out[13]);   /* leaf 0 vendor string */
void hw_cpu_brand(char out[49]);    /* leaves 0x80000002..4 brand string */
uint32_t hw_cpu_max_leaf(void);

/* --- CMOS real-time clock ------------------------------------------------- */
struct rtc_time { uint16_t year; uint8_t mon, day, hour, min, sec; };
void rtc_now(struct rtc_time *t);

/* --- PCI ------------------------------------------------------------------ */
struct pci_dev {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if, revision;
};
/* Enumerate PCI devices into buf (up to max); returns count found. */
int         pci_scan(struct pci_dev *buf, int max);
const char *pci_class_name(uint8_t cls);
uint32_t    pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
