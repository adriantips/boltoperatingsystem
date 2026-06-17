/* ===========================================================================
 *  BoltOS  -  kernel/app_python.c
 *  A BoltPython REPL window. Mirrors the Terminal app: keeps its own character
 *  grid, edits a line from the focused-window key stream, and feeds complete
 *  statements to a persistent bpy_vm so globals/functions survive across lines.
 *  Interpreter output (kputc) is captured into the grid via console_set_sink,
 *  exactly like the terminal does for shell commands.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "console.h"
#include "boltpy.h"
#include "string.h"
#include "pit.h"
#include "keyboard.h"
#include "kprintf.h"

#define TCOLS 220
#define TROWS 80
#define PAD   8
#define CELLW 8
#define CELLH 14

typedef struct {
    char    ch [TROWS][TCOLS];
    uint8_t col[TROWS][TCOLS];
    int     cols, rows;
    int     cx, cy;
    uint8_t curcol;
    char    line[256];
    int     len;
    char    acc[4096];          /* multi-line statement accumulator              */
    int     acclen;
    int     cont;               /* 1 = gathering a continuation (... prompt)      */
    int     block;              /* the continuation was opened by a ':' suite     */
    bpy_vm *vm;
} pyterm_t;

static pyterm_t  py;
static pyterm_t *active;        /* grid currently capturing kputc                */

/* palette index -> colour (matches the terminal app) */
static uint32_t pal(uint8_t i) {
    switch (i) {
    case 1:  return 0x4B8BBE;          /* prompt / accent (python blue) */
    case 2:  return 0x7C7C8A;          /* dim                           */
    case 3:  return COL_GOOD;
    case 4:  return COL_BAD;
    default: return 0xD8D8E0;          /* normal                        */
    }
}

/* ---------------------------- character grid ---------------------------- */
static void py_scroll(pyterm_t *t) {
    for (int r = 1; r < t->rows; r++)
        for (int c = 0; c < TCOLS; c++) { t->ch[r - 1][c] = t->ch[r][c]; t->col[r - 1][c] = t->col[r][c]; }
    for (int c = 0; c < TCOLS; c++) { t->ch[t->rows - 1][c] = 0; t->col[t->rows - 1][c] = 0; }
    t->cy = t->rows - 1;
}
static void py_newline(pyterm_t *t) { t->cx = 0; if (++t->cy >= t->rows) py_scroll(t); }

static void py_putc(pyterm_t *t, char c) {
    if (t->cols <= 0 || t->rows <= 0) return;
    if (c == '\n') { py_newline(t); return; }
    if (c == '\r') { t->cx = 0; return; }
    if (c == '\t') { int n = 4 - (t->cx & 3); while (n--) py_putc(t, ' '); return; }
    if (c == '\b') {
        if (t->cx > 0) t->cx--;
        else if (t->cy > 0) { t->cy--; t->cx = t->cols - 1; }
        t->ch[t->cy][t->cx] = 0;
        return;
    }
    if ((unsigned char)c < 32) return;
    t->ch[t->cy][t->cx]  = c;
    t->col[t->cy][t->cx] = t->curcol;
    if (++t->cx >= t->cols) py_newline(t);
}
static void py_puts(pyterm_t *t, const char *s, uint8_t color) {
    uint8_t save = t->curcol; t->curcol = color;
    while (*s) py_putc(t, *s++);
    t->curcol = save;
}

/* kputc sink while the interpreter (or input()) runs */
static void py_sink(char c) { if (active) py_putc(active, c); }

/* input() reader: blocks for a line but keeps the desktop alive by pumping the
 * compositor while idle. Echoes through the active sink into the grid. */
static int py_input(char *buf, int cap) {
    int len = 0;
    for (;;) {
        int ci = kbd_trygetc();
        if (ci < 0) { gui_pump(); __asm__ volatile("hlt"); continue; }
        char c = (char)ci;
        if (c == '\n') { kputc('\n'); buf[len] = 0; return len; }
        if (c == '\b') { if (len) { len--; kputc('\b'); } continue; }
        if ((unsigned char)c >= 32 && len < cap - 1) { buf[len++] = c; kputc(c); }
    }
}

/* ----------------------- source-shape heuristics ------------------------ *
 *  Same rules as the console REPL (kernel/cmd_python.c): bracket depth, a
 *  trailing ':' suite opener, and backslash line continuation decide when a
 *  statement is complete.                                                    */
static int scan_depth(const char *s) {
    int depth = 0; char q = 0; int comment = 0;
    for (; *s; s++) {
        char c = *s;
        if (comment) { if (c == '\n') comment = 0; continue; }
        if (q) { if (c == '\\' && s[1]) s++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '#') { comment = 1; continue; }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
    }
    return depth;
}
static int opens_block(const char *s) {
    char q = 0; int last = 0;
    for (; *s; s++) {
        char c = *s;
        if (q) { if (c == '\\' && s[1]) s++; else if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; last = c; continue; }
        if (c == '#') break;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') last = c;
    }
    return last == ':';
}
static int ends_backslash(const char *s) { int n = 0; while (s[n]) n++; return n > 0 && s[n - 1] == '\\'; }
static int is_blank(const char *s) { for (; *s; s++) if (*s != ' ' && *s != '\t' && *s != '\r') return 0; return 1; }

static void py_prompt(pyterm_t *t) { py_puts(t, t->cont ? "... " : ">>> ", 1); }

/* run a complete statement through the VM with output captured into the grid */
static void py_run(pyterm_t *t, const char *code) {
    active = t; t->curcol = 0;
    console_set_sink(py_sink);
    bpy_exec(t->vm, code, 1);          /* echo=1: print the repr of a bare expr */
    console_set_sink(0);
    active = 0;
}

static void acc_append(pyterm_t *t, const char *s) {
    for (int i = 0; s[i] && t->acclen < (int)sizeof(t->acc) - 2; i++) t->acc[t->acclen++] = s[i];
    t->acc[t->acclen++] = '\n';
    t->acc[t->acclen] = 0;
}

/* called when the user presses Enter; t->line holds the just-entered line */
static void py_submit(pyterm_t *t) {
    if (!t->cont) {
        if (strcmp(t->line, "exit()") == 0 || strcmp(t->line, "quit()") == 0 ||
            strcmp(t->line, "exit")   == 0 || strcmp(t->line, "quit")   == 0) {
            py_puts(t, "Use the window's close button to quit.\n", 2);
            py_prompt(t);
            return;
        }
        if (is_blank(t->line)) { py_prompt(t); return; }

        int block = opens_block(t->line);
        int depth = scan_depth(t->line);
        int cont  = ends_backslash(t->line);
        if (depth <= 0 && !block && !cont) { py_run(t, t->line); py_prompt(t); return; }

        t->acclen = 0; t->acc[0] = 0;
        acc_append(t, t->line);
        t->cont = 1; t->block = block;
        py_prompt(t);
        return;
    }

    /* continuation line */
    if (t->block) {
        if (scan_depth(t->acc) <= 0 && is_blank(t->line)) {   /* blank line ends a suite */
            py_run(t, t->acc);
            t->cont = 0; t->block = 0; t->acclen = 0;
            py_prompt(t);
            return;
        }
    }
    acc_append(t, t->line);
    if (!t->block && scan_depth(t->acc) <= 0 && !ends_backslash(t->line)) {
        py_run(t, t->acc);
        t->cont = 0; t->acclen = 0;
        py_prompt(t);
        return;
    }
    py_prompt(t);
}

/* ------------------------------ callbacks ------------------------------- */
static void py_key(window_t *w, char c) {
    (void)w;
    pyterm_t *t = &py;
    if (c == '\n') {
        py_putc(t, '\n');
        t->line[t->len] = 0;
        py_submit(t);
        t->len = 0;
    } else if (c == '\b') {
        if (t->len > 0) { t->len--; py_putc(t, '\b'); }
    } else if ((unsigned char)c >= 32 && t->len < (int)sizeof(t->line) - 1) {
        t->line[t->len++] = c;
        py_putc(t, c);
    }
}

static void py_draw(window_t *w, int cx, int cy, int cw, int ch) {
    (void)w;
    pyterm_t *t = &py;
    g_fill(cx, cy, cw, ch, 0x0C0C12);

    t->cols = (cw - 2 * PAD) / CELLW; if (t->cols > TCOLS) t->cols = TCOLS; if (t->cols < 1) t->cols = 1;
    t->rows = (ch - 2 * PAD) / CELLH; if (t->rows > TROWS) t->rows = TROWS; if (t->rows < 1) t->rows = 1;

    for (int r = 0; r < t->rows; r++) {
        int py_y = cy + PAD + r * CELLH;
        for (int c = 0; c < t->cols; c++) {
            char g = t->ch[r][c];
            if (g) g_char(cx + PAD + c * CELLW, py_y, g, pal(t->col[r][c]), 1);
        }
    }

    if (gui_window_focused(w) && (pit_ticks() / 500) % 2 == 0) {
        int px = cx + PAD + t->cx * CELLW;
        int pyy = cy + PAD + t->cy * CELLH;
        g_fill(px, pyy, 2, CELLH - 2, 0x4B8BBE);
    }
}

void python_app_init(void) {
    memset(&py, 0, sizeof(py));
    py.cols = 88; py.rows = 29;
    py.vm = bpy_new();
    bpy_set_input(py_input);                     /* keep the desktop alive in input() */

    window_t *w = gui_add_window("Python", 720, 460, 0x4B8BBE, ICON_TERMINAL);
    if (!w) return;
    w->draw = py_draw;
    w->key  = py_key;

    py_puts(&py, bpy_version(), 1);
    py_putc(&py, '\n');
    if (!py.vm) py_puts(&py, "(out of memory: the REPL is unavailable)\n", 4);
    else        py_puts(&py, "Interactive shell. Globals persist across lines.\n\n", 2);
    py_prompt(&py);
}
