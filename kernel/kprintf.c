#include <stdint.h>
#include <stdarg.h>
#include "kprintf.h"
#include "io.h"

static void print_dec(uint64_t v) {
    char buf[24]; int i = 0;
    if (!v) { kputc('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) kputc(buf[i]);
}

static void print_hex(uint64_t v, int pad) {
    static const char *h = "0123456789abcdef";
    char buf[16];
    for (int i = 0; i < 16; i++) { buf[i] = h[v & 0xF]; v >>= 4; }
    int start = 15;
    if (pad > 0 && pad <= 16) start = pad - 1;
    else { while (start > 0 && buf[start] == '0') start--; }
    for (int i = start; i >= 0; i--) kputc(buf[i]);
}

void kprintf(const char *f, ...) {
    va_list ap; va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { kputc(*f); continue; }
        f++;
        int l = 0;
        while (*f == 'l') { l++; f++; }
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) kputc(*s++); } break;
        case 'c':   kputc((char)va_arg(ap, int)); break;
        case 'd': case 'i': {
            long v = l ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) { kputc('-'); v = -v; }
            print_dec((uint64_t)v);
        } break;
        case 'u': { unsigned long v = l ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); print_dec(v); } break;
        case 'x': { unsigned long v = l ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); print_hex(v, 0); } break;
        case 'p': { uint64_t v = (uint64_t)va_arg(ap, void *); kputc('0'); kputc('x'); print_hex(v, 16); } break;
        case '%': kputc('%'); break;
        default:  kputc('%'); kputc(*f); break;
        }
    }
    va_end(ap);
}

void panic(const char *msg) {
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    for (;;) __asm__ volatile("cli; hlt");
}
