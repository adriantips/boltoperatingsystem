#include <stdint.h>
#include "framebuffer.h"
#include "mm.h"

/* The VESA linear framebuffer the bootloader selected. It lives below 4 GiB
 * and is already identity-mapped, so we write straight to it. 32 bpp / xRGB. */
static volatile uint32_t *fb;
static uint32_t fw, fh, pitch_px;

void fb_init(struct bootinfo *bi) {
    if (!bi->fb_addr || !bi->fb_width || bi->fb_bpp != 32) { fb = 0; return; }
    fb       = (volatile uint32_t *)P2V((uint64_t)bi->fb_addr);
    fw       = bi->fb_width;
    fh       = bi->fb_height;
    pitch_px = bi->fb_pitch ? bi->fb_pitch / 4 : bi->fb_width;
}

int      fb_present(void) { return fb != 0; }
uint32_t fb_width(void)   { return fw; }
uint32_t fb_height(void)  { return fh; }

void fb_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x < fw && y < fh) fb[(uint64_t)y * pitch_px + x] = color;
}

void fb_fill(uint32_t color) {
    for (uint32_t y = 0; y < fh; y++)
        for (uint32_t x = 0; x < fw; x++)
            fb[(uint64_t)y * pitch_px + x] = color;
}

void fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            fb_pixel(x + i, y + j, color);
}

/* Blit a tightly-packed fw*fh xRGB image (the GUI compositor's backbuffer)
 * straight onto the visible framebuffer, one scanline at a time so a pitch
 * wider than the width is handled. 32-bit stores keep it fast in a freestanding
 * build with no SSE memcpy. */
void fb_blit(const uint32_t *src) {
    if (!fb) return;
    for (uint32_t y = 0; y < fh; y++) {
        volatile uint32_t *d = fb  + (uint64_t)y * pitch_px;
        const uint32_t    *s = src + (uint64_t)y * fw;
        for (uint32_t x = 0; x < fw; x++) d[x] = s[x];
    }
}

/* Scale a logical sw*sh desktop into the (dx,dy,dw,dh) rect of the panel and
 * paint black bars around it. A per-column source-index map is built once so
 * the inner loop is array lookups, not a divide per pixel. */
void fb_blit_scaled(const uint32_t *src, uint32_t sw, uint32_t sh,
                    uint32_t dx, uint32_t dy, uint32_t dw, uint32_t dh) {
    if (!fb) return;
    for (uint32_t y = 0; y < fh; y++) {           /* clear panel -> letterbox bars */
        volatile uint32_t *d = fb + (uint64_t)y * pitch_px;
        for (uint32_t x = 0; x < fw; x++) d[x] = 0;
    }
    if (!sw || !sh || !dw || !dh || dx >= fw || dy >= fh) return;

    static uint16_t colmap[4096];
    uint32_t w = dw;
    if (dx + w > fw) w = fw - dx;
    if (w > 4096)    w = 4096;
    for (uint32_t i = 0; i < w; i++) colmap[i] = (uint16_t)((uint64_t)i * sw / dw);

    for (uint32_t j = 0; j < dh; j++) {
        uint32_t py = dy + j;
        if (py >= fh) break;
        uint32_t sy = (uint32_t)((uint64_t)j * sh / dh);
        const uint32_t    *s = src + (uint64_t)sy * sw;
        volatile uint32_t *d = fb + (uint64_t)py * pitch_px + dx;
        for (uint32_t i = 0; i < w; i++) d[i] = s[colmap[i]];
    }
}
