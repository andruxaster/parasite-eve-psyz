// Sega Dreamcast GPU backend: link-only stub.
//
// Keeps a CPU copy of the PS1 VRAM so uploads, read-backs, copies and fills
// behave, and returns the exact length of every GP0 drawing packet so the
// command parser in libgpu.c stays in step. Nothing reaches the PowerVR yet.
#include <psyz.h>
#include <psyz/log.h>
#include <libgpu.h>
#include <stdint.h>
#include <string.h>
#include "../draw.h"

#define VRAM_W 1024
#define VRAM_H 512

static uint16_t vram[VRAM_W * VRAM_H] __attribute__((aligned(32)));

static int ClipRect(const RECT* r, int* x, int* y, int* w, int* h) {
    *x = r->x & (VRAM_W - 1);
    *y = r->y & (VRAM_H - 1);
    *w = r->w;
    *h = r->h;
    if (*w <= 0 || *h <= 0) {
        return 0;
    }
    if (*x + *w > VRAM_W) {
        *w = VRAM_W - *x;
    }
    if (*y + *h > VRAM_H) {
        *h = VRAM_H - *y;
    }
    return 1;
}

void Draw_Reset(void) { memset(vram, 0, sizeof(vram)); }
void Draw_DisplayEnable(unsigned int on) { (void)on; }
void Draw_DisplayArea(unsigned int x, unsigned int y) {
    (void)x;
    (void)y;
}
void Draw_DisplayHorizontalRange(unsigned int start, unsigned int end) {
    (void)start;
    (void)end;
}
void Draw_DisplayVerticalRange(unsigned int start, unsigned int end) {
    (void)start;
    (void)end;
}
void Draw_SetDisplayMode(DisplayMode* mode) { (void)mode; }
void Draw_SetTexpageMode(ParamDrawTexpageMode* p) { (void)p; }
void Draw_SetTextureWindow(unsigned int mask_x, unsigned int mask_y,
                           unsigned int off_x, unsigned int off_y) {
    (void)mask_x;
    (void)mask_y;
    (void)off_x;
    (void)off_y;
}
void Draw_SetAreaStart(int x, int y) {
    (void)x;
    (void)y;
}
void Draw_SetAreaEnd(int x, int y) {
    (void)x;
    (void)y;
}
void Draw_SetOffset(int x, int y) {
    (void)x;
    (void)y;
}
int Draw_SetHorizontalGrid(unsigned int source_width, unsigned int target_width) {
    if (source_width == 0 || target_width == 0) {
        return -1;
    }
    return source_width == target_width ? 0 : -1;
}
void Draw_SetMask(int bit0, int bit1) {
    (void)bit0;
    (void)bit1;
}

void Draw_ClearImage(RECT* rect, u_char r, u_char g, u_char b) {
    int x, y, w, h;
    if (!ClipRect(rect, &x, &y, &w, &h)) {
        return;
    }
    uint16_t c = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    for (int row = 0; row < h; ++row) {
        uint16_t* dst = &vram[(y + row) * VRAM_W + x];
        for (int col = 0; col < w; ++col) {
            dst[col] = c;
        }
    }
}

void Draw_LoadImage(RECT* rect, u_long* p) {
    int x, y, w, h;
    if (!ClipRect(rect, &x, &y, &w, &h)) {
        return;
    }
    const uint16_t* src = (const uint16_t*)p;
    for (int row = 0; row < h; ++row) {
        memcpy(&vram[(y + row) * VRAM_W + x], src + row * rect->w, w * 2);
    }
}

void Draw_StoreImage(RECT* rect, u_long* p) {
    int x, y, w, h;
    if (!ClipRect(rect, &x, &y, &w, &h)) {
        return;
    }
    uint16_t* dst = (uint16_t*)p;
    for (int row = 0; row < h; ++row) {
        memcpy(dst + row * rect->w, &vram[(y + row) * VRAM_W + x], w * 2);
    }
}

void Draw_MoveImage(RECT* rect, unsigned int x, unsigned int y) {
    int sx, sy, w, h;
    if (!ClipRect(rect, &sx, &sy, &w, &h)) {
        return;
    }
    x &= VRAM_W - 1;
    y &= VRAM_H - 1;
    if (x + w > VRAM_W) {
        w = VRAM_W - x;
    }
    if (y + h > VRAM_H) {
        h = VRAM_H - y;
    }
    for (int row = 0; row < h; ++row) {
        int r = y > (unsigned)sy ? h - 1 - row : row;
        memmove(&vram[(y + r) * VRAM_W + x], &vram[(sy + r) * VRAM_W + sx], w * 2);
    }
}

void Draw_PutDispEnv(DISPENV* disp) { (void)disp; }

// Words in a GP0 drawing packet, as on the PS1 GPU.
static int PacketLength(const u_long* packets, int max_len) {
    int code = (int)(*packets >> 24) & 0xFF;
    int len;
    if (code >= 0x20 && code < 0x40) {
        int verts = (code & 0x08) ? 4 : 3;
        int tex = (code & 0x04) ? 1 : 0;
        int gouraud = (code & 0x10) ? 1 : 0;
        len = 1 + verts * (1 + tex) + (gouraud ? verts - 1 : 0);
    } else if (code >= 0x40 && code < 0x60) {
        int gouraud = (code & 0x10) ? 1 : 0;
        if (code & 0x08) {
            // polyline: vertices until the 0x5xxx5xxx terminator
            len = 1;
            while (len < max_len &&
                   (packets[len] & 0xF000F000) != 0x50005000) {
                len++;
            }
            len++;
        } else {
            len = 3 + gouraud;
        }
    } else if (code >= 0x60 && code < 0x80) {
        int tex = (code & 0x04) ? 1 : 0;
        int variable = ((code >> 3) & 3) == 0;
        len = 2 + tex + variable;
    } else {
        len = 1;
    }
    return len > max_len ? max_len : len;
}

int Draw_PushPrim(u_long* packets, int max_len) {
    return PacketLength(packets, max_len);
}

void Draw_ResetBuffer(void) {}
void Draw_FlushBuffer(void) {}
int Draw_ExequeSync(void) { return 0; }
