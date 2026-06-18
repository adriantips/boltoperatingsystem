/* ===========================================================================
 *  BoltOS  -  kernel/gui.c
 *  Compositing window manager: graphics primitives, window framing, focus and
 *  stacking, dragging, minimise/maximise/close, a centred taskbar with a start
 *  menu and clock, and the mouse cursor. One backbuffer, one blit per frame.
 * ===========================================================================*/
#include <stdint.h>
#include "gui.h"
#include "settings.h"
#include "framebuffer.h"
#include "console.h"
#include "keyboard.h"
#include "mouse.h"
#include "kheap.h"
#include "pit.h"
#include "hw.h"
#include "shell.h"
#include "string.h"
#include "fs.h"

extern const unsigned char font8x8_basic[128][8];

#define TITLE_H    32
#define TASKBAR_H  48
#define RADIUS     9
#define MAX_WIN    8
#define DI_W       80              /* desktop icon cell width  */
#define DI_H       86              /* desktop icon cell height */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- backbuffer + clip -------------------------------------------------- */
static uint32_t *BB;            /* compositor backbuffer (alloc PW*PH, use W*H) */
static uint32_t *BG;            /* prerendered desktop wallpaper (W*H) or 0 */
static int       PW, PH;        /* physical panel size (fixed by the bootloader) */
static int       W, H;          /* logical desktop size (the resolution setting)  */
static int       out_x, out_y, out_w, out_h;   /* where the logical desktop lands  */
static int       clx0, cly0, clx1, cly1;   /* active clip rect (exclusive hi) */

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iabs(int v) { return v < 0 ? -v : v; }
static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }

void g_set_clip(int x, int y, int w, int h) {
    clx0 = imax(0, x);     cly0 = imax(0, y);
    clx1 = imin(W, x + w); cly1 = imin(H, y + h);
}
void g_clear_clip(void) { clx0 = 0; cly0 = 0; clx1 = W; cly1 = H; }

static inline void px(int x, int y, uint32_t c) {
    if (x >= clx0 && x < clx1 && y >= cly0 && y < cly1) BB[y * W + x] = c;
}
static inline void px_blend(int x, int y, uint32_t c, uint8_t a) {
    if (x < clx0 || x >= clx1 || y < cly0 || y >= cly1) return;
    uint32_t d = BB[y * W + x];
    uint32_t r = (((c >> 16 & 0xFF) * a + (d >> 16 & 0xFF) * (255 - a)) / 255) & 0xFF;
    uint32_t g = (((c >> 8  & 0xFF) * a + (d >> 8  & 0xFF) * (255 - a)) / 255) & 0xFF;
    uint32_t b = (((c       & 0xFF) * a + (d       & 0xFF) * (255 - a)) / 255) & 0xFF;
    BB[y * W + x] = (r << 16) | (g << 8) | b;
}

void g_fill(int x, int y, int w, int h, uint32_t c) {
    int x0 = imax(x, clx0), y0 = imax(y, cly0);
    int x1 = imin(x + w, clx1), y1 = imin(y + h, cly1);
    for (int j = y0; j < y1; j++) { uint32_t *row = BB + j * W; for (int i = x0; i < x1; i++) row[i] = c; }
}
void g_blend(int x, int y, int w, int h, uint32_t c, uint8_t a) {
    int x0 = imax(x, clx0), y0 = imax(y, cly0);
    int x1 = imin(x + w, clx1), y1 = imin(y + h, cly1);
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) px_blend(i, j, c, a);
}
void g_hline(int x, int y, int w, uint32_t c) { g_fill(x, y, w, 1, c); }
void g_vline(int x, int y, int h, uint32_t c) { g_fill(x, y, 1, h, c); }
void g_rect(int x, int y, int w, int h, uint32_t c) {
    g_hline(x, y, w, c); g_hline(x, y + h - 1, w, c);
    g_vline(x, y, h, c); g_vline(x + w - 1, y, h, c);
}

/* horizontal inset of a rounded corner at distance `dy` from the rounded edge */
static int corner_dx(int r, int dy) {
    if (dy >= r) return 0;
    return r - isqrt(r * r - (r - 1 - dy) * (r - 1 - dy));
}

void g_round(int x, int y, int w, int h, int r, uint32_t c, uint8_t a) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int j = 0; j < h; j++) {
        int dx = 0;
        if (j < r)            dx = corner_dx(r, j);
        else if (j >= h - r)  dx = corner_dx(r, h - 1 - j);
        if (a >= 255) g_fill(x + dx, y + j, w - 2 * dx, 1, c);
        else for (int i = x + dx; i < x + w - dx; i++) px_blend(i, y + j, c, a);
    }
}

/* rounded only on the top two corners (window title bars) */
static void g_round_top(int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int dx = (j < r) ? corner_dx(r, j) : 0;
        g_fill(x + dx, y + j, w - 2 * dx, 1, c);
    }
}

/* ---- text --------------------------------------------------------------- */
void g_char(int x, int y, char ch, uint32_t color, int s) {
    unsigned char u = (unsigned char)ch;
    if (u >= 128) return;
    const unsigned char *g = font8x8_basic[u];
    for (int ry = 0; ry < 8; ry++) {
        unsigned char bits = g[ry];
        for (int rx = 0; rx < 8; rx++)
            if (bits & (1u << rx)) g_fill(x + rx * s, y + ry * s, s, s, color);
    }
}
void g_text(int x, int y, const char *str, uint32_t color, int s) {
    for (; *str; str++) { g_char(x, y, *str, color, s); x += 8 * s; }
}
int g_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

/* ---- proportional text -------------------------------------------------- *
 * Per-glyph metrics are derived once from the bitmap font: the ink box (first
 * and last lit column) gives a left bearing and a tight advance; blanks get a
 * fixed word-space. Glyphs are drawn left-trimmed to their ink so spacing is
 * even. Used by the browser body for prose; the rest of the UI stays monospace.*/
static int gp_adv[128], gp_minc[128], gp_ready = 0;
static void gp_build(void) {
    for (int u = 0; u < 128; u++) {
        const unsigned char *g = font8x8_basic[u];
        int minc = 8, maxc = -1;
        for (int ry = 0; ry < 8; ry++) {
            unsigned char b = g[ry];
            for (int rx = 0; rx < 8; rx++) if (b & (1u << rx)) { if (rx < minc) minc = rx; if (rx > maxc) maxc = rx; }
        }
        if (maxc < 0) { gp_adv[u] = 4; gp_minc[u] = 0; }      /* space / blank glyph */
        else          { gp_adv[u] = (maxc - minc + 1) + 1; gp_minc[u] = minc; }
    }
    gp_ready = 1;
}
int g_glyph_adv(char c, int scale) {
    unsigned char u = (unsigned char)c; if (u >= 128) u = '?';
    if (!gp_ready) gp_build();
    return gp_adv[u] * scale;
}
void g_char_p(int x, int y, char ch, uint32_t color, int s, int italic) {
    unsigned char u = (unsigned char)ch; if (u >= 128) u = '?';
    if (!gp_ready) gp_build();
    const unsigned char *g = font8x8_basic[u];
    int minc = gp_minc[u];
    for (int ry = 0; ry < 8; ry++) {
        unsigned char bits = g[ry];
        int sk = italic ? ((7 - ry) * s) / 3 : 0;            /* fake-italic shear */
        for (int rx = 0; rx < 8; rx++)
            if (bits & (1u << rx)) g_fill(x + (rx - minc) * s + sk, y + ry * s, s, s, color);
    }
}
int g_text_width_pn(const char *s, int len, int scale) {
    int w = 0; for (int i = 0; i < len; i++) w += g_glyph_adv(s[i], scale); return w;
}
int g_text_pn(int x, int y, const char *s, int len, uint32_t color, int scale, int italic) {
    for (int i = 0; i < len; i++) { g_char_p(x, y, s[i], color, scale, italic); x += g_glyph_adv(s[i], scale); }
    return x;
}

/* Blit a w*h ARGB image into the dst rectangle, nearest-neighbour scaled and
 * alpha-blended against the backbuffer. Honours the active clip rect. */
void g_blit(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = imax(dx, clx0), y0 = imax(dy, cly0);
    int x1 = imin(dx + dw, clx1), y1 = imin(dy + dh, cly1);
    for (int y = y0; y < y1; y++) {
        int sy = (y - dy) * sh / dh; if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = src + sy * sw;
        for (int x = x0; x < x1; x++) {
            int sx = (x - dx) * sw / dw; if (sx >= sw) sx = sw - 1;
            uint32_t c = srow[sx];
            uint8_t a = c >> 24;
            if (a == 255)      BB[y * W + x] = c & 0xFFFFFF;
            else if (a == 0)   continue;
            else               px_blend(x, y, c & 0xFFFFFF, a);
        }
    }
}

/* ===========================================================================
 *  Desktop wallpaper
 * ===========================================================================*/
static uint32_t lerp(uint32_t a, uint32_t b, int n, int d) {
    int ar = a >> 16 & 0xFF, ag = a >> 8 & 0xFF, ab = a & 0xFF;
    int br = b >> 16 & 0xFF, bg = b >> 8 & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * n / d, g = ag + (bg - ag) * n / d, c = ab + (bb - ab) * n / d;
    return (uint32_t)(r << 16 | g << 8 | c);
}
static uint32_t add_clamp(uint32_t c, int dr, int dg, int db) {
    int r = (int)(c >> 16 & 0xFF) + dr, g = (int)(c >> 8 & 0xFF) + dg, b = (int)(c & 0xFF) + db;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    if (r < 0) r = 0;     if (g < 0) g = 0;     if (b < 0) b = 0;
    return (uint32_t)(r << 16 | g << 8 | b);
}

/* Render the desktop background per the current wallpaper style + base colour.
 * dst is a packed W*H image (the BG buffer or, as a fallback, BB). */
static void render_wallpaper(uint32_t *dst) {
    uint32_t base   = g_settings.wall_color;
    uint32_t bottom = add_clamp(base, 16, 26, 46);     /* gradient foot   */
    uint32_t glow   = add_clamp(base, 34, 52, 92);     /* radial highlight*/
    uint32_t grid   = lerp(base, g_theme.accent, 26, 100);
    int gx = W / 2, gy = H / 3, gr = (W > H ? W : H) * 2 / 3;
    int style = g_settings.wall_style;

    for (int y = 0; y < H; y++) {
        uint32_t grad = lerp(base, bottom, y, H);
        for (int x = 0; x < W; x++) {
            uint32_t c;
            if (style == WALL_SOLID) {
                c = base;
            } else if (style == WALL_GRID) {
                c = ((x % 28) == 0 || (y % 28) == 0) ? grid : base;
            } else {
                int dx = x - gx, dy = y - gy;
                int dist = isqrt(dx * dx + dy * dy);
                uint32_t fld = (style == WALL_GLOW) ? base : grad;
                c = (dist < gr) ? lerp(glow, fld, dist, gr) : fld;
            }
            dst[x] = c;
        }
        dst += W;
    }
}

/* ===========================================================================
 *  Window registry + stacking
 * ===========================================================================*/
static window_t wins[MAX_WIN];
static int      nwin;
static int      ztop;
static int      focus_id = -1;
static int      drag_id  = -1, drag_dx, drag_dy;
static uint8_t  prev_btns;
static int      menu_open;
static int      cpu_load;
static int      dirty = 1;

int  gui_screen_w(void) { return W; }
int  gui_screen_h(void) { return H; }
int  gui_panel_w(void)  { return PW; }
int  gui_panel_h(void)  { return PH; }
int  gui_cpu_load(void) { return cpu_load; }

/* The pointer reports physical panel pixels; map it into the logical desktop so
 * every hit-test and the cursor stay aligned when the desktop is scaled. */
static int mlx(void) {
    int v = mouse_x() - out_x;
    if (out_w > 0) v = v * W / out_w;
    if (v < 0) v = 0; if (v >= W) v = W - 1;
    return v;
}
static int mly(void) {
    int v = mouse_y() - out_y;
    if (out_h > 0) v = v * H / out_h;
    if (v < 0) v = 0; if (v >= H) v = H - 1;
    return v;
}
void gui_request_redraw(void) { dirty = 1; }
int  gui_window_focused(window_t *win) { return focus_id >= 0 && &wins[focus_id] == win; }

window_t *gui_add_window(const char *title, int w, int h, uint32_t accent, int icon) {
    if (nwin >= MAX_WIN) return 0;
    window_t *win = &wins[nwin];
    memset(win, 0, sizeof(*win));
    strncpy(win->title, title, sizeof(win->title));
    win->w = w; win->h = h;
    win->min_w = 260; win->min_h = 160;
    win->accent = accent; win->icon = icon;
    /* cascade initial placement so windows don't perfectly overlap */
    win->x = 80 + nwin * 48;
    win->y = 60 + nwin * 40;
    win->z = ++ztop;
    nwin++;
    return win;
}

void gui_focus(window_t *win) {
    for (int i = 0; i < nwin; i++)
        if (&wins[i] == win) { win->z = ++ztop; focus_id = i; dirty = 1; return; }
}
void gui_open(window_t *win) {
    win->open = 1; win->minimized = 0;
    gui_focus(win);
}

/* topmost open, non-minimised window index, or -1 */
static int topmost(void) {
    int best = -1, bz = -1;
    for (int i = 0; i < nwin; i++)
        if (wins[i].open && !wins[i].minimized && wins[i].z > bz) { bz = wins[i].z; best = i; }
    return best;
}

/* ===========================================================================
 *  Icons (drawn vectorially; no asset files)
 * ===========================================================================*/
static void draw_bolt(int x, int y, int s, uint32_t c) {     /* lightning glyph */
    for (int i = 0; i < 7 * s; i++) g_fill(x + 4 * s - i / 3, y + i, 2 * s, s, c);
    g_fill(x - 2 * s, y + 6 * s, 5 * s, s, c);
    for (int i = 0; i < 7 * s; i++) g_fill(x + 1 * s - i / 3, y + 7 * s + i, 2 * s, s, c);
}
static void draw_icon(int id, int x, int y, int s, uint32_t c) {
    switch (id) {
    case ICON_TERMINAL:
        g_round(x, y, 16 * s, 13 * s, 2 * s, 0x101018, 255);
        g_rect(x, y, 16 * s, 13 * s, c);
        g_text(x + 2 * s, y + 3 * s, ">", c, s);
        g_fill(x + 7 * s, y + 8 * s, 5 * s, s, c);
        break;
    case ICON_TASKMGR:
        g_fill(x + 1 * s, y + 7 * s, 2 * s, 5 * s, c);
        g_fill(x + 5 * s, y + 4 * s, 2 * s, 8 * s, c);
        g_fill(x + 9 * s,  y + 1 * s, 2 * s, 11 * s, c);
        g_fill(x + 13 * s, y + 5 * s, 2 * s, 7 * s, c);
        break;
    case ICON_START:
        draw_bolt(x + 7 * s, y + 1 * s, s, c);
        break;
    case ICON_SETTINGS: {                       /* gear */
        int cxx = x + 8 * s, cyy = y + 7 * s, r = 5 * s;
        g_fill(cxx - s, y + 1 * s, 2 * s, 12 * s, c);        /* N-S teeth   */
        g_fill(x + 2 * s, cyy - s, 12 * s, 2 * s, c);        /* E-W teeth   */
        g_fill(cxx - 4 * s, cyy - 4 * s, 8 * s, 2 * s, c);   /* diagonal hint */
        g_fill(cxx - 4 * s, cyy + 3 * s, 8 * s, 2 * s, c);
        g_round(cxx - r, cyy - r, 2 * r, 2 * r, r, c, 255);  /* body disc   */
        uint32_t hub = (c >> 1) & 0x7F7F7F;
        g_round(cxx - 2 * s, cyy - 2 * s, 4 * s, 4 * s, 2 * s, hub, 255); /* hub */
        break;
    }
    case ICON_BROWSER: {                        /* globe */
        int cxx = x + 8 * s, cyy = y + 7 * s, r = 6 * s;
        g_round(cxx - r, cyy - r, 2 * r, 2 * r, r, c, 255);     /* disc          */
        uint32_t in = (c >> 1) & 0x7F7F7F;
        g_vline(cxx, cyy - r, 2 * r, in);                       /* meridian      */
        g_hline(cxx - r, cyy, 2 * r, in);                       /* equator       */
        g_hline(cxx - r + s, cyy - 3 * s, 2 * (r - s), in);     /* latitude lines*/
        g_hline(cxx - r + s, cyy + 3 * s, 2 * (r - s), in);
        break;
    }
    case ICON_FOLDER: {                         /* manila folder (Windows-ish) */
        uint32_t body = 0xE7B24A, lid = 0xF3C766, lip = 0xC8932E;
        g_round(x, y + 2 * s, 8 * s, 4 * s, 2 * s, lid, 255);    /* back tab     */
        g_round(x, y + 4 * s, 16 * s, 9 * s, 2 * s, lip, 255);   /* rear panel   */
        g_round(x, y + 5 * s, 16 * s, 8 * s, 2 * s, body, 255);  /* front flap   */
        g_fill(x + 2 * s, y + 6 * s, 12 * s, s, lid);            /* highlight     */
        (void)c;
        break;
    }
    case ICON_FILE: {                           /* document page with folded corner */
        uint32_t pg = 0xF5F7FC, fold = 0xCAD6EE, ln = 0x9AA7C2;
        g_round(x + 2 * s, y, 11 * s, 14 * s, 2 * s, pg, 255);
        g_fill(x + 2 * s + 7 * s, y, 4 * s, 4 * s, fold);        /* dog-ear       */
        for (int i = 0; i < 4; i++)                              /* text lines    */
            g_fill(x + 4 * s, y + 6 * s + i * 2 * s, 7 * s, s, ln);
        (void)c;
        break;
    }
    case ICON_TRASH: {                          /* recycle bin */
        uint32_t metal = 0x9AA7BE, dark = 0x5E6A82;
        g_fill(x + 6 * s, y, 4 * s, s, metal);                   /* handle        */
        g_fill(x + 2 * s, y + s, 12 * s, 2 * s, metal);          /* lid           */
        g_round(x + 3 * s, y + 3 * s, 10 * s, 11 * s, 2 * s, metal, 255); /* body  */
        g_fill(x + 5 * s,  y + 5 * s, s, 7 * s, dark);           /* slots         */
        g_fill(x + 8 * s,  y + 5 * s, s, 7 * s, dark);
        g_fill(x + 11 * s, y + 5 * s, s, 7 * s, dark);
        (void)c;
        break;
    }
    default:
        g_round(x, y, 14 * s, 12 * s, 2 * s, c, 255);
        break;
    }
}

void gui_icon(int id, int x, int y, int scale, uint32_t color) { draw_icon(id, x, y, scale, color); }

/* ===========================================================================
 *  Window frame
 * ===========================================================================*/
/* caption button rects: slot 0 = close, 1 = maximise, 2 = minimise (from right) */
static void caption_btn(window_t *w, int slot, int *bx, int *by, int *bw, int *bh) {
    *bw = 46; *bh = TITLE_H;
    *bx = w->x + w->w - (slot + 1) * 46;
    *by = w->y;
}

static void draw_window(window_t *w) {
    int focused = gui_window_focused(w);
    int mxp = mlx(), myp = mly();

    /* soft drop shadow */
    g_round(w->x - 6, w->y - 4, w->w + 12, w->h + 14, RADIUS + 6, 0x000000, 28);
    g_round(w->x - 2, w->y - 1, w->w + 4,  w->h + 7,  RADIUS + 3, 0x000000, 45);

    /* body + title bar */
    g_round(w->x, w->y, w->w, w->h, RADIUS, COL_PANEL, 255);
    g_round_top(w->x, w->y, w->w, TITLE_H, RADIUS, focused ? COL_PANEL_2 : 0x202028);
    if (focused) g_fill(w->x, w->y, w->w, 2, w->accent);     /* accent strip */

    /* title icon + text */
    draw_icon(w->icon, w->x + 12, w->y + 9, 1, focused ? w->accent : COL_TEXT_DIM);
    g_text(w->x + 34, w->y + 12, w->title, focused ? COL_TEXT : COL_TEXT_DIM, 1);

    /* caption buttons */
    for (int slot = 0; slot < 3; slot++) {
        int bx, by, bw, bh; caption_btn(w, slot, &bx, &by, &bw, &bh);
        int hot = mxp >= bx && mxp < bx + bw && myp >= by && myp < by + bh;
        uint32_t hl = (slot == 0) ? COL_BAD : 0x3A3A48;
        if (hot) { if (slot == 0) g_round_top(bx, by, bw, bh, 0, hl); else g_fill(bx, by, bw, bh, hl); }
        uint32_t gc = (hot && slot == 0) ? 0xFFFFFF : COL_TEXT;
        int cx = bx + bw / 2, cy = by + bh / 2;
        if (slot == 0) {                                     /* close: X */
            for (int i = -4; i <= 4; i++) { px(cx + i, cy + i, gc); px(cx + i, cy - i, gc); }
        } else if (slot == 1) {                              /* maximise: box */
            g_rect(cx - 4, cy - 4, 9, 9, gc);
        } else {                                             /* minimise: dash */
            g_hline(cx - 4, cy + 3, 9, gc);
        }
    }

    /* client area drawn by the app, clipped to it */
    int cx = w->x, cy = w->y + TITLE_H, cw = w->w, ch = w->h - TITLE_H;
    g_fill(cx, cy, cw, ch, COL_PANEL);
    if (w->draw) { g_set_clip(cx, cy, cw, ch); w->draw(w, cx, cy, cw, ch); g_clear_clip(); }
}

/* ===========================================================================
 *  Taskbar + start menu
 * ===========================================================================*/
#define TB_BTN 44
static void taskbar_layout(int *startx, int *y) {
    int count = 1 + nwin;                       /* start button + one per window */
    int total = count * TB_BTN + (count - 1) * 8;
    *startx = (W - total) / 2;
    *y = H - TASKBAR_H + (TASKBAR_H - TB_BTN) / 2;
}
/* slot -1 = start button; 0..nwin-1 = windows */
static void taskbar_btn_rect(int slot, int *bx, int *by) {
    int sx, y; taskbar_layout(&sx, &y);
    *bx = sx + (slot + 1) * (TB_BTN + 8);
    *by = y;
}

static void draw_clock(void) {
    struct rtc_time t; rtc_now(&t);
    char hm[6];
    hm[0] = '0' + t.hour / 10; hm[1] = '0' + t.hour % 10; hm[2] = ':';
    hm[3] = '0' + t.min / 10;  hm[4] = '0' + t.min % 10;  hm[5] = 0;
    g_text(W - g_text_width(hm, 1) - 18, H - TASKBAR_H + 12, hm, COL_TEXT, 1);
    char md[6];
    md[0] = '0' + t.mon / 10; md[1] = '0' + t.mon % 10; md[2] = '/';
    md[3] = '0' + t.day / 10; md[4] = '0' + t.day % 10; md[5] = 0;
    g_text(W - g_text_width(md, 1) - 18, H - TASKBAR_H + 27, md, COL_TEXT_DIM, 1);
}

static void draw_taskbar(void) {
    g_blend(0, H - TASKBAR_H, W, TASKBAR_H, 0x0E0E16, 224);
    g_hline(0, H - TASKBAR_H, W, 0x33333F);

    int top = topmost();
    /* start button */
    int bx, by; taskbar_btn_rect(-1, &bx, &by);
    int mxp = mlx(), myp = mly();
    int hot = mxp >= bx && mxp < bx + TB_BTN && myp >= by && myp < by + TB_BTN;
    if (menu_open || hot) g_round(bx, by, TB_BTN, TB_BTN, 8, 0x2A2A38, 255);
    draw_icon(ICON_START, bx + 8, by + 8, 1, COL_ACCENT);

    /* one button per window */
    for (int i = 0; i < nwin; i++) {
        taskbar_btn_rect(i, &bx, &by);
        window_t *w = &wins[i];
        int active = (i == top) && w->open && !w->minimized;
        int h2 = mxp >= bx && mxp < bx + TB_BTN && myp >= by && myp < by + TB_BTN;
        if (active)      g_round(bx, by, TB_BTN, TB_BTN, 8, 0x33334A, 255);
        else if (h2)     g_round(bx, by, TB_BTN, TB_BTN, 8, 0x262632, 255);
        draw_icon(w->icon, bx + 8, by + 8, 1, w->open ? w->accent : COL_TEXT_DIM);
        if (w->open) {   /* running indicator underline */
            int iw = active ? 18 : 8;
            g_round(bx + TB_BTN / 2 - iw / 2, by + TB_BTN - 3, iw, 3, 1, active ? w->accent : COL_TEXT_DIM, 255);
        }
    }
    draw_clock();
}

static void draw_start_menu(void) {
    if (!menu_open) return;
    int mw = 300, mh = 84 + nwin * 44;
    int sx, sy; taskbar_layout(&sx, &sy);
    int x = sx, y = H - TASKBAR_H - mh - 10;
    if (x + mw > W - 8) x = W - 8 - mw;
    g_round(x, y, mw, mh, 12, 0x16161E, 245);
    g_text(x + 18, y + 16, "BoltOS", COL_TEXT, 2);
    g_text(x + 18, y + 40, "Pinned apps", COL_TEXT_DIM, 1);
    int mxp = mlx(), myp = mly();
    for (int i = 0; i < nwin; i++) {
        int iy = y + 64 + i * 44;
        int hot = mxp >= x + 8 && mxp < x + mw - 8 && myp >= iy && myp < iy + 40;
        if (hot) g_round(x + 8, iy, mw - 16, 40, 8, 0x2A2A3A, 255);
        draw_icon(wins[i].icon, x + 18, iy + 12, 1, wins[i].accent);
        g_text(x + 44, iy + 14, wins[i].title, COL_TEXT, 1);
    }
}

/* ---- mouse cursor (arrow) ---------------------------------------------- */
static const char *CURSOR[19] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ",
    "X....X     ", "X.....X    ", "X......X   ", "X.......X  ", "X........X ",
    "X.....XXXXX", "X..X..X    ", "X.X X..X   ", "XX  X..X   ", "X    X..X  ",
    "     X..X  ", "      X..X ", "      X..X ", "       XX  ",
};
static void draw_cursor(void) {
    g_clear_clip();
    int ox = mlx(), oy = mly();
    for (int j = 0; j < 19; j++)
        for (int i = 0; CURSOR[j][i]; i++) {
            char c = CURSOR[j][i];
            if (c == 'X') px(ox + i, oy + j, 0x000000);
            else if (c == '.') px(ox + i, oy + j, 0xFFFFFF);
        }
}

/* ===========================================================================
 *  Desktop icons + drag-and-drop
 * ===========================================================================*/
#define MAX_DESKICON 16
typedef struct { int used, x, y, icon; char label[40]; fs_node *node; } deskicon_t;
static deskicon_t dicons[MAX_DESKICON];
static int      desk_sel = -1;
static int      desk_drag = -1, desk_dx, desk_dy;
static uint64_t desk_last_tick;
static int      desk_last_idx = -1;

/* drag payload (a file/folder being dragged out of a window onto the desktop) */
static fs_node *dnd_node;
static char     dnd_label[40];
static int      dnd_icon, dnd_armed, dnd_active, dnd_px, dnd_py, press_x, press_y;

static int deskicon_find(fs_node *n) {
    for (int i = 0; i < MAX_DESKICON; i++) if (dicons[i].used && dicons[i].node == n) return i;
    return -1;
}
static void deskicon_autopos(int *ox, int *oy) {
    int used = 0;
    for (int i = 0; i < MAX_DESKICON; i++) if (dicons[i].used) used++;
    int per_col = (H - TASKBAR_H - 16) / DI_H; if (per_col < 1) per_col = 1;
    *ox = 16 + (used / per_col) * DI_W;
    *oy = 16 + (used % per_col) * DI_H;
}
static void deskicon_clamp(deskicon_t *d) {
    if (d->x < 0) d->x = 0; if (d->x > W - DI_W) d->x = W - DI_W;
    if (d->y < 0) d->y = 0; if (d->y > H - TASKBAR_H - DI_H) d->y = H - TASKBAR_H - DI_H;
}

void gui_desktop_add(fs_node *node, const char *label, int icon) {
    if (!node || deskicon_find(node) >= 0) return;
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (dicons[i].used) continue;
        dicons[i].used = 1; dicons[i].node = node; dicons[i].icon = icon;
        strncpy(dicons[i].label, label ? label : "item", sizeof(dicons[i].label));
        deskicon_autopos(&dicons[i].x, &dicons[i].y);
        dirty = 1;
        return;
    }
}

/* place (or move) a dropped item's shortcut centred on the drop point */
static void deskicon_drop(fs_node *n, const char *label, int icon, int x, int y) {
    int idx = deskicon_find(n);
    if (idx < 0) {
        for (int i = 0; i < MAX_DESKICON && idx < 0; i++) if (!dicons[i].used) idx = i;
        if (idx < 0) return;
        dicons[idx].used = 1; dicons[idx].node = n; dicons[idx].icon = icon;
        strncpy(dicons[idx].label, label ? label : "item", sizeof(dicons[idx].label));
    }
    dicons[idx].x = x - DI_W / 2; dicons[idx].y = y - 28;
    deskicon_clamp(&dicons[idx]);
    desk_sel = idx; dirty = 1;
}

void gui_begin_item_drag(fs_node *node, const char *label, int icon) {
    if (!node) return;
    dnd_node = node; dnd_icon = icon; dnd_armed = 1; dnd_active = 0;
    strncpy(dnd_label, label ? label : "item", sizeof(dnd_label));
    press_x = mlx(); press_y = mly();
}

static void deskicon_label(const char *src, char *out, int cap) {
    int maxc = (DI_W - 6) / 8; if (maxc > cap - 1) maxc = cap - 1;
    int len = (int)strlen(src);
    if (len <= maxc) { strncpy(out, src, cap); return; }
    int k = 0; for (; k < maxc - 2; k++) out[k] = src[k];
    out[k++] = '.'; out[k++] = '.'; out[k] = 0;
}

static void draw_desktop_icons(void) {
    int mxp = mlx(), myp = mly();
    for (int i = 0; i < MAX_DESKICON; i++) {
        if (!dicons[i].used) continue;
        deskicon_t *d = &dicons[i];
        int hot = mxp >= d->x && mxp < d->x + DI_W && myp >= d->y && myp < d->y + DI_H;
        if (i == desk_sel) g_round(d->x, d->y, DI_W, DI_H, 8, COL_ACCENT, 70);
        else if (hot)      g_round(d->x, d->y, DI_W, DI_H, 8, 0xFFFFFF, 26);
        int s = 3, iw = 16 * s;
        draw_icon(d->icon, d->x + (DI_W - iw) / 2, d->y + 8, s, COL_TEXT);
        char lb[18]; deskicon_label(d->label, lb, sizeof(lb));
        int tw = g_text_width(lb, 1), tx = d->x + (DI_W - tw) / 2, ty = d->y + 8 + iw + 6;
        g_text(tx + 1, ty + 1, lb, 0x000000, 1);        /* drop shadow for legibility */
        g_text(tx, ty, lb, 0xFFFFFF, 1);
    }
}

static void draw_drag_ghost(void) {
    int s = 2, iw = 16 * s, x = dnd_px - iw / 2, y = dnd_py - iw / 2;
    g_round(x - 6, y - 4, iw + 12, iw + 24, 8, 0x000000, 70);
    draw_icon(dnd_icon, x, y, s, COL_TEXT);
    int tw = g_text_width(dnd_label, 1);
    g_text(dnd_px - tw / 2, y + iw + 4, dnd_label, 0xFFFFFF, 1);
}

static int deskicon_hit(int x, int y) {
    for (int i = MAX_DESKICON - 1; i >= 0; i--) {
        if (!dicons[i].used) continue;
        deskicon_t *d = &dicons[i];
        if (x >= d->x && x < d->x + DI_W && y >= d->y && y < d->y + DI_H) return i;
    }
    return -1;
}

static void deskicon_open(int i) {
    if (i < 0 || i >= MAX_DESKICON || !dicons[i].used || !dicons[i].node) return;
    files_open_node(dicons[i].node);
}

/* drop point over the wallpaper (not a window, taskbar or menu) -> make shortcut */
static void dnd_drop(int x, int y) {
    if (!dnd_node) return;
    if (y >= H - TASKBAR_H) return;
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        if (w->open && !w->minimized && x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return;
    }
    deskicon_drop(dnd_node, dnd_label, dnd_icon, x, y);
}

/* ===========================================================================
 *  Compositor
 * ===========================================================================*/
static void composite(void) {
    g_clear_clip();
    if (BG) { for (int i = 0; i < W * H; i++) BB[i] = BG[i]; }
    else    render_wallpaper(BB);

    draw_desktop_icons();

    /* windows back-to-front by z */
    for (int pass = 0; pass <= ztop; pass++)
        for (int i = 0; i < nwin; i++)
            if (wins[i].open && !wins[i].minimized && wins[i].z == pass)
                draw_window(&wins[i]);

    draw_start_menu();
    draw_taskbar();
    if (dnd_active) draw_drag_ghost();
    draw_cursor();
}

/* ===========================================================================
 *  Input
 * ===========================================================================*/
static void clamp_window(window_t *w) {
    if (w->x > W - 60)        w->x = W - 60;
    if (w->x < 60 - w->w)     w->x = 60 - w->w;
    if (w->y < 0)             w->y = 0;
    if (w->y > H - TASKBAR_H - TITLE_H) w->y = H - TASKBAR_H - TITLE_H;
}

static void toggle_window_from_taskbar(int i) {
    window_t *w = &wins[i];
    int top = topmost();
    if (!w->open)            gui_open(w);
    else if (w->minimized) { w->minimized = 0; gui_focus(w); }
    else if (i == top)       w->minimized = 1;     /* focused -> minimise */
    else                     gui_focus(w);
    dirty = 1;
}

static void do_maximize(window_t *w) {
    if (w->maximized) {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->maximized = 0;
    } else {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = W; w->h = H - TASKBAR_H; w->maximized = 1;
    }
}

static void on_left_down(int x, int y) {
    /* start button */
    int bx, by; taskbar_btn_rect(-1, &bx, &by);
    if (x >= bx && x < bx + TB_BTN && y >= by && y < by + TB_BTN) { menu_open = !menu_open; dirty = 1; return; }

    /* taskbar app buttons */
    if (y >= H - TASKBAR_H) {
        for (int i = 0; i < nwin; i++) {
            taskbar_btn_rect(i, &bx, &by);
            if (x >= bx && x < bx + TB_BTN && y >= by && y < by + TB_BTN) { toggle_window_from_taskbar(i); menu_open = 0; return; }
        }
        return;
    }

    /* start menu interaction */
    if (menu_open) {
        int mw = 300, mh = 84 + nwin * 44;
        int sx, sy; taskbar_layout(&sx, &sy);
        int mxx = sx; if (mxx + mw > W - 8) mxx = W - 8 - mw;
        int myy = H - TASKBAR_H - mh - 10;
        if (x >= mxx && x < mxx + mw && y >= myy && y < myy + mh) {
            for (int i = 0; i < nwin; i++) {
                int iy = myy + 64 + i * 44;
                if (x >= mxx + 8 && x < mxx + mw - 8 && y >= iy && y < iy + 40) { gui_open(&wins[i]); menu_open = 0; return; }
            }
            return;
        }
        menu_open = 0; dirty = 1;                 /* click outside menu closes it */
    }

    /* windows, front to back */
    for (int z = ztop; z >= 0; z--) {
        for (int i = 0; i < nwin; i++) {
            window_t *w = &wins[i];
            if (!w->open || w->minimized || w->z != z) continue;
            if (x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) continue;
            gui_focus(w);
            if (y < w->y + TITLE_H) {              /* title bar */
                for (int slot = 0; slot < 3; slot++) {
                    int qx, qy, qw, qh; caption_btn(w, slot, &qx, &qy, &qw, &qh);
                    if (x >= qx && x < qx + qw && y >= qy && y < qy + qh) {
                        if (slot == 0)      w->open = 0;            /* close   */
                        else if (slot == 1) do_maximize(w);         /* maximise*/
                        else                w->minimized = 1;       /* minimise*/
                        return;
                    }
                }
                if (!w->maximized) { drag_id = i; drag_dx = x - w->x; drag_dy = y - w->y; }
            } else if (w->click) {                  /* client area */
                w->click(w, x - w->x, y - (w->y + TITLE_H));
            }
            return;
        }
    }

    /* nothing else hit: the desktop. Select / double-click-open / arm reposition. */
    int di = deskicon_hit(x, y);
    if (di >= 0) {
        uint64_t now = pit_ticks();
        if (desk_last_idx == di && now - desk_last_tick < 400) {   /* double-click */
            desk_sel = di; desk_last_idx = -1; deskicon_open(di); return;
        }
        desk_sel = di; desk_last_idx = di; desk_last_tick = now;
        desk_drag = di; desk_dx = x - dicons[di].x; desk_dy = y - dicons[di].y;
        dirty = 1;
        return;
    }
    desk_sel = -1;                                   /* click on empty wallpaper */
}

static void handle_mouse(void) {
    int x = mlx(), y = mly();
    uint8_t b = mouse_buttons();
    uint8_t down = b & ~prev_btns;
    uint8_t up   = prev_btns & ~b;

    if (drag_id >= 0) {                              /* dragging a window */
        if (b & MOUSE_LEFT) { wins[drag_id].x = x - drag_dx; wins[drag_id].y = y - drag_dy; clamp_window(&wins[drag_id]); }
        if (up & MOUSE_LEFT)  drag_id = -1;
    } else if (desk_drag >= 0) {                     /* repositioning a desktop icon */
        if (b & MOUSE_LEFT) { dicons[desk_drag].x = x - desk_dx; dicons[desk_drag].y = y - desk_dy; deskicon_clamp(&dicons[desk_drag]); }
        if (up & MOUSE_LEFT)  desk_drag = -1;
    } else if (dnd_active) {                          /* dragging an item to the desktop */
        dnd_px = x; dnd_py = y;
        if (up & MOUSE_LEFT) { dnd_drop(x, y); dnd_active = 0; dnd_armed = 0; dnd_node = 0; }
    } else if (dnd_armed && (b & MOUSE_LEFT)) {       /* armed: promote to a drag once it moves */
        if (iabs(x - press_x) > 5 || iabs(y - press_y) > 5) { dnd_active = 1; dnd_px = x; dnd_py = y; }
        if (up & MOUSE_LEFT) { dnd_armed = 0; dnd_node = 0; }
    } else if (down & MOUSE_LEFT) {
        on_left_down(x, y);
    }
    if ((up & MOUSE_LEFT) && !dnd_active) { dnd_armed = 0; dnd_node = 0; }  /* armed click ended w/o a drag */
    prev_btns = b;
    dirty = 1;                                      /* cursor moved -> repaint */
}

static void handle_key(char c) {
    if (c == 27) { menu_open = 0; dirty = 1; return; }   /* Esc closes start menu */
    int f = topmost();
    if (f >= 0 && wins[f].key) wins[f].key(&wins[f], c);
    dirty = 1;
}

/* ===========================================================================
 *  Display configuration  (resolution, aspect letterbox, wallpaper)
 *  Reads the live g_settings and recomputes the logical desktop size, the
 *  letterboxed output rectangle on the physical panel, and the wallpaper.
 * ===========================================================================*/
void gui_apply_display(void) {
    /* logical desktop size from the resolution setting (clamped to the panel) */
    int lw, lh; settings_res_dims(g_settings.res_index, &lw, &lh);
    if (lw > PW) lw = PW; if (lw < 320) lw = 320;
    if (lh > PH) lh = PH; if (lh < 240) lh = 240;
    W = lw; H = lh;

    /* output-rectangle shape from the aspect setting (Auto = logical aspect),
     * maximised inside the physical panel and centred (letterbox bars around). */
    int an, ad; settings_aspect_ratio(g_settings.aspect_index, &an, &ad);
    if (an <= 0 || ad <= 0) { an = W; ad = H; }
    int ow = PW, oh = PW * ad / an;
    if (oh > PH) { oh = PH; ow = PH * an / ad; }
    if (ow > PW) ow = PW;
    out_w = ow; out_h = oh;
    out_x = (PW - ow) / 2; out_y = (PH - oh) / 2;

    g_clear_clip();                          /* clip now spans the new W,H */

    /* keep every window inside the (possibly smaller) logical desktop */
    for (int i = 0; i < nwin; i++) {
        window_t *w = &wins[i];
        if (w->maximized) { w->x = 0; w->y = 0; w->w = W; w->h = H - TASKBAR_H; }
        else {
            if (w->w > W)             w->w = W;
            if (w->h > H - TASKBAR_H) w->h = H - TASKBAR_H;
            clamp_window(w);
        }
    }

    if (BG) render_wallpaper(BG);
    dirty = 1;
}

static int output_is_native(void) {
    return out_x == 0 && out_y == 0 && out_w == PW && out_h == PH && W == PW && H == PH;
}

/* Force one composite + blit right now. Lets a blocking command (e.g. the
 * `python` REPL waiting on keyboard input) keep the desktop on screen instead
 * of freezing it. No-op before the desktop is up (BB not yet allocated). */
void gui_pump(void) {
    if (!BB) return;
    composite();
    if (output_is_native()) fb_blit(BB);
    else fb_blit_scaled(BB, (uint32_t)W, (uint32_t)H,
                        (uint32_t)out_x, (uint32_t)out_y, (uint32_t)out_w, (uint32_t)out_h);
    dirty = 0;
}

/* ===========================================================================
 *  Boot screen + main loop
 * ===========================================================================*/
void gui_run(void) {
    PW = (int)fb_width();
    PH = (int)fb_height();
    W = PW; H = PH;
    out_x = out_y = 0; out_w = PW; out_h = PH;
    g_clear_clip();

    BB = (uint32_t *)kmalloc((uint64_t)PW * PH * 4);
    if (!BB) { console_detach(); shell_run(); return; }   /* no memory -> shell */
    BG = (uint32_t *)kmalloc((uint64_t)PW * PH * 4);

    settings_init();           /* theme + logical size + letterbox + wallpaper */

    console_detach();                                /* GUI owns the framebuffer now */

    terminal_app_init();
    files_app_init();
    browser_app_init();
    python_app_init();
    taskmgr_app_init();
    settings_app_init();
    for (int i = 0; i < nwin; i++) gui_open(&wins[i]);   /* show the apps */
    if (nwin > 0) gui_focus(&wins[0]);                   /* terminal gets focus */

    uint64_t tick_pit = pit_ticks();
    uint64_t sec_pit  = pit_ticks();
    uint64_t busy = 0, total = 0;
    prev_btns = 0; drag_id = -1; dirty = 1;

    for (;;) {
        uint64_t t0 = rdtsc();

        if (mouse_poll_event()) handle_mouse();
        int ci; while ((ci = kbd_trygetc()) >= 0) handle_key((char)ci);

        uint64_t now = pit_ticks();
        if (now - tick_pit >= 500) {                 /* ~2 Hz: clock, blink, sampling */
            tick_pit = now;
            for (int i = 0; i < nwin; i++) if (wins[i].open && wins[i].tick) wins[i].tick(&wins[i]);
            dirty = 1;
        }

        if (dirty) {
            composite();
            if (output_is_native()) fb_blit(BB);
            else fb_blit_scaled(BB, (uint32_t)W, (uint32_t)H,
                                 (uint32_t)out_x, (uint32_t)out_y,
                                 (uint32_t)out_w, (uint32_t)out_h);
            dirty = 0;
        }

        uint64_t t1 = rdtsc();
        busy += t1 - t0;
        __asm__ volatile("hlt");                     /* sleep until next IRQ */
        total += rdtsc() - t0;

        if (now - sec_pit >= 1000) {                 /* recompute CPU load each second */
            sec_pit = now;
            cpu_load = total ? (int)((busy * 100) / total) : 0;
            if (cpu_load > 100) cpu_load = 100;
            busy = total = 0;
        }
    }
}
