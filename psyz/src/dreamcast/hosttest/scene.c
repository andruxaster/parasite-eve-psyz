/* Synthetic PS1 frame through the Dreamcast GPU backend (dc_gpu.c) and the
 * software PVR stand-in: textures of every depth, CLUTs, sprites, Gouraud,
 * semi-transparency modes, lines and an upload into the framebuffer. */
#include <psyz.h>
#include <libgpu.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "draw.h"
#include <psyz/log.h>

void psyz_log(unsigned int level, const char *file, unsigned int line, const char *func, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%d] %s: ", level, func); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}
int Psyz_VideoVSync(int mode);

#define C15(r, g, b) ((u16)(((r) >> 3) | (((g) >> 3) << 5) | (((b) >> 3) << 10)))
#define XY(x, y) ((u_long)(((y) & 0xFFFF) << 16 | ((x) & 0xFFFF)))
#define UV(u, v, extra) ((u_long)((extra) << 16 | (v) << 8 | (u)))
#define RGB(r, g, b) ((u_long)((b) << 16 | (g) << 8 | (r)))
#define TPAGE(page, abr, bpp) ((u16)((page) | ((abr) << 5) | ((bpp) << 7)))
#define CLUT(x, y) ((u16)(((y) << 6) | ((x) >> 4)))

static void Load(int x, int y, int w, int h, const u16 *data) {
    RECT r = {x, y, w, h};
    Draw_LoadImage(&r, (u_long *)data);
}
static void Prim(u_long *p, int n) {
    int used = Draw_PushPrim(p, n);
    if (used != n) printf("packet %02lX: parsed %d words, expected %d\n", p[0] >> 24, used, n);
}
static void SetTpage(u16 tp) {
    ParamDrawTexpageMode m; u_short raw = tp; memcpy(&m, &raw, sizeof raw);
    Draw_SetTexpageMode(&m);
}

int main(void) {
    Draw_Reset();
    DisplayMode dm = {0}; dm.horizontal_resolution = 1; Draw_SetDisplayMode(&dm);
    Draw_DisplayArea(0, 0);
    Draw_SetAreaStart(0, 0); Draw_SetAreaEnd(319, 239); Draw_SetOffset(0, 0);

    /* 4bpp 32x32 checker in page 10 (x 640), index 0 = transparent border */
    static u16 t4[32 * 8];
    for (int v = 0; v < 32; v++) for (int i = 0; i < 8; i++) {
        u16 h = 0;
        for (int k = 0; k < 4; k++) {
            int u = i * 4 + k;
            int idx = (u < 3 || v < 3 || u > 28 || v > 28) ? 0 : (((u >> 3) + (v >> 3)) & 1) ? 1 : 2;
            h |= (u16)(idx << (k * 4));
        }
        t4[v * 8 + i] = h;
    }
    Load(640, 0, 8, 32, t4);
    static u16 clut4[16] = {0, C15(255, 0, 0), C15(255, 255, 0)};
    Load(0, 480, 16, 1, clut4);

    /* 8bpp 32x32 diagonal ramp in page 9 (x 576) with a 256-colour CLUT */
    static u16 t8[32 * 16];
    for (int v = 0; v < 32; v++) for (int i = 0; i < 16; i++)
        t8[v * 16 + i] = (u16)(((i * 2 + v) * 4 & 255) | (((i * 2 + 1 + v) * 4 & 255) << 8));
    Load(576, 0, 16, 32, t8);
    static u16 clut8[256];
    for (int i = 0; i < 256; i++) clut8[i] = C15(0, i, 255 - i) | (i ? 0 : 0);
    clut8[0] = C15(0, 0, 8);
    Load(0, 481, 256, 1, clut8);

    /* 16bpp 32x32 in page 28 (x 768, y 256): rings, colour 0 in the centre */
    static u16 t16[32 * 32];
    for (int v = 0; v < 32; v++) for (int u = 0; u < 32; u++) {
        int d = (u - 16) * (u - 16) + (v - 16) * (v - 16);
        t16[v * 32 + u] = d < 25 ? 0 : (d / 40) & 1 ? C15(255, 255, 255) : C15(255, 0, 255);
    }
    Load(768, 256, 32, 32, t16);

    /* frame: clear, then an upload straight into the framebuffer */
    RECT all = {0, 0, 320, 240};
    Draw_ClearImage(&all, 20, 20, 60);
    static u16 grad[48 * 24];
    for (int y = 0; y < 24; y++) for (int x = 0; x < 48; x++) grad[y * 48 + x] = C15(x * 5, y * 10, 128);
    Load(8, 8, 48, 24, grad);

    /* POLY_FT4 4bpp, 2x scaled */
    u_long ft4[] = {0x2C808080, XY(70, 10), UV(0, 0, CLUT(0, 480)), XY(134, 10), UV(32, 0, TPAGE(10, 0, 0)),
                    XY(70, 74), UV(0, 32, 0), XY(134, 74), UV(32, 32, 0)};
    Prim(ft4, 9);
    /* SPRT 8bpp at 1:1, texture page from the draw mode */
    SetTpage(TPAGE(9, 0, 1));
    u_long sprt[] = {0x64808080, XY(150, 10), UV(0, 0, CLUT(0, 481)), (32u << 16) | 32u};
    Prim(sprt, 4);
    /* POLY_FT4 16bpp, darkened to half by the colour */
    u_long ft16[] = {0x2C404040, XY(200, 10), UV(0, 0, 0), XY(264, 10), UV(32, 0, TPAGE(28, 0, 2)),
                     XY(200, 74), UV(0, 32, 0), XY(264, 74), UV(32, 32, 0)};
    Prim(ft16, 9);
    /* POLY_G3 */
    u_long g3[] = {0x30FF0000 | 0x0000FF, XY(20, 100), RGB(0, 255, 0), XY(120, 100), RGB(0, 0, 255), XY(70, 190)};
    g3[0] = 0x30000000 | RGB(255, 0, 0);
    Prim(g3, 6);
    /* TILE mode 1 (additive) over the triangle */
    SetTpage(TPAGE(0, 1, 0));
    u_long tile_add[] = {0x62000000 | RGB(90, 90, 90), XY(50, 120), (60u << 16) | 100u};
    Prim(tile_add, 3);
    /* TILE mode 0 (average) white over the 8bpp sprite */
    SetTpage(TPAGE(0, 0, 0));
    u_long tile_avg[] = {0x62000000 | RGB(255, 255, 255), XY(140, 30), (40u << 16) | 60u};
    Prim(tile_avg, 3);
    /* TILE mode 2 (subtract) over the gradient upload */
    SetTpage(TPAGE(0, 2, 0));
    u_long tile_sub[] = {0x62000000 | RGB(0, 200, 0), XY(30, 8), (24u << 16) | 30u};
    Prim(tile_sub, 3);
    /* LINE_G2 and a 3-point polyline */
    u_long lg2[] = {0x50000000 | RGB(255, 0, 0), XY(10, 220), RGB(0, 128, 255), XY(310, 180)};
    Prim(lg2, 4);
    u_long lf3[] = {0x48000000 | RGB(255, 255, 255), XY(200, 200), XY(250, 230), XY(300, 200), 0x55555555};
    Prim(lf3, 5);
    /* raw-texture POLY_FT4 (no modulation) and a mode-3 textured quad */
    u_long raw4[] = {0x2D101010, XY(260, 90), UV(0, 0, CLUT(0, 480)), XY(292, 90), UV(32, 0, TPAGE(10, 0, 0)),
                     XY(260, 122), UV(0, 32, 0), XY(292, 122), UV(32, 32, 0)};
    Prim(raw4, 9);
    u_long st3[] = {0x2E808080, XY(250, 130), UV(0, 0, CLUT(0, 480)), XY(314, 130), UV(32, 0, TPAGE(10, 3, 0)),
                    XY(250, 194), UV(0, 32, 0), XY(314, 194), UV(32, 32, 0)};
    Prim(st3, 9);
    /* 8x8 tile and gouraud-textured triangle */
    u_long t8x8[] = {0x70000000 | RGB(0, 255, 255), XY(140, 150)};
    Prim(t8x8, 2);
    u_long gt3[] = {0x34FFFFFF & (0x34000000 | RGB(255, 128, 128)), XY(160, 110), UV(0, 0, CLUT(0, 481)),
                    RGB(128, 255, 128), XY(240, 110), UV(31, 0, TPAGE(9, 0, 1)),
                    RGB(128, 128, 255), XY(200, 170), UV(16, 31, 0)};
    Prim(gt3, 9);
    Psyz_VideoVSync(0);

    /* second frame: the 4bpp texture changes after being drawn once */
    Draw_ClearImage(&all, 0, 0, 0);
    Prim(ft4, 9);
    clut4[1] = C15(0, 255, 0);
    Load(0, 480, 16, 1, clut4);
    u_long ft4b[] = {0x2C808080, XY(150, 10), UV(0, 0, CLUT(0, 480)), XY(214, 10), UV(32, 0, TPAGE(10, 0, 0)),
                     XY(150, 74), UV(0, 32, 0), XY(214, 74), UV(32, 32, 0)};
    Prim(ft4b, 9);
    Psyz_VideoVSync(0);
    return 0;
}
