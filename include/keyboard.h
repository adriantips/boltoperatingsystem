#pragma once
/* PS/2 keyboard driver. The IRQ1 handler decodes scancodes (with shift) into
 * ASCII and pushes them onto a ring buffer; the shell drains it below. */
void keyboard_init(void);
int  kbd_trygetc(void);   /* next char, or -1 if the buffer is empty (non-blocking) */
char kbd_getc(void);      /* block (hlt) until a key is available, then return it */
