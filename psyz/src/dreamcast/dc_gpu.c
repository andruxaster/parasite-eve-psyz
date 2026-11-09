// Sega Dreamcast GPU backend on the PowerVR (KallistiOS).
//
// The PS1 GPU draws into its own VRAM with no depth buffer: later primitives
// cover earlier ones. Here every primitive goes into the PVR's translucent
// list with autosort disabled, so the PVR draws them in submission order too,
// and the frame is shown when the game waits for the vertical blank.
//
// A CPU copy of the PS1 VRAM receives uploads, fills and copies. Textured
// primitives decode their PS1 texture page (4, 8 or 16 bits per pixel, with
// its CLUT) from that copy into a 256x256 ARGB1555 texture in PVR memory,
// kept in a small LRU cache that VRAM writes invalidate. Uploads, copies and
// fills that land in the displayed framebuffer are drawn as quads, the same
// way the PSP backend does.
//
// Coordinates are mapped from the PS1 framebuffer (256-640 x 240/480) to the
// 640x480 Dreamcast screen.
//
// In 24-bit display mode (the MDEC movies) the displayed VRAM rectangle holds
// packed RGB888; it is converted to an RGB565 texture and shown whole at
// VSync, and primitives are not drawn.
//
// Not emulated yet: drawing into VRAM and sampling it back as a texture,
// reading rendered pixels with StoreImage/MoveImage, the draw-area clip,
// texture windows, mask bits, dithering, per-texel STP for semi-transparent
// primitives (the whole primitive blends) and brightening above 0x80 on
// textured colors. Subtractive blending (mode 2) takes three passes.
#include <psyz.h>
#include <psyz/log.h>
#include <libgpu.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <kos/timer.h>
#include "../draw.h"

#define VRAM_W 1024
#define VRAM_H 512
#define SCREEN_W 640.0f
#define SCREEN_H 480.0f

static uint16_t vram[VRAM_W * VRAM_H] __attribute__((aligned(32)));

// ===== state =====
static bool is_init;
static bool display_enabled = true;
static bool rgb24;
static struct {
    int x, y;
} disp_area;
// visible lines from the display vertical range: the PS1 shows `disp_lines`
// framebuffer rows starting `disp_top` lines below the standard 240-line
// window (NTSC range start 16)
static int disp_top, disp_lines = 240;
static int game_w = 320, game_h = 240;
static float scale_x = SCREEN_W / 320.0f, scale_y = SCREEN_H / 240.0f;
static u16 cur_tpage;
static struct {
    int x, y;
} draw_offset, draw_area_start, draw_area_end, fb_origin;
static bool fb_origin_locked;
static struct {
    int x, y;
} disp_origins[2];
static int disp_origin_count;

// ===== PVR scene =====
#define PVR_VERTEX_BUF (1024 * 1024)
// TA parameter space is finite; stop adding geometry before it overflows
#define MAX_FRAME_VERTICES (PVR_VERTEX_BUF / 48)
static bool in_scene;
static uint32_t frame_no;
static int frame_vertices;
static uint64_t boot_us;

// blend modes: 0-3 are the PS1 semi-transparency modes, 4 is opaque, 5
// inverts the framebuffer (used for mode 2)
#define BLEND_OPAQUE 4
#define BLEND_INVERT 5
#define BLEND_MODES 6
static const uint8_t blend_alpha[BLEND_MODES] = {128, 255, 255, 64, 255, 255};

// ===== texture cache =====
#define TEX_SLOTS_MAX 32
#define TEX_BYTES (256 * 256 * 2)
#define RAW_TEXTURE 1 // variant: every texel opaque (VRAM region blits)

typedef struct {
    pvr_ptr_t ptr;
    bool valid;
    bool dirty;
    uint32_t key;
    uint32_t last_used;
    uint32_t used_frame;
    uint8_t hdr_ready; // bitmask of compiled blend headers
    pvr_poly_hdr_t hdr[BLEND_MODES];
} TexSlot;

static TexSlot tex_slots[TEX_SLOTS_MAX];
static int tex_slot_count;
static uint32_t tex_clock;
static TexSlot* memo_slot;
static uint32_t memo_key;
static pvr_poly_hdr_t col_hdr[BLEND_MODES];

// 24-bit display: the whole picture as one RGB565 texture
#define FRAME24_W 512
#define FRAME24_H 512
static pvr_ptr_t frame24;
static pvr_poly_hdr_t frame24_hdr;
static int frame24_hdr_h;
static bool frame24_dirty = true;
static int cur_hdr = -1; // slot index * BLEND_MODES + mode, or -1 - mode

static inline uint32_t TexKey(u16 tpage, u16 clut, int variant) {
    uint32_t bpp = (tpage >> 7) & 3;
    if (bpp > 2) {
        bpp = 2;
    }
    uint32_t key = (tpage & 0x1F) | (bpp << 5) | ((uint32_t)variant << 7);
    if (bpp < 2) {
        key |= (uint32_t)clut << 8;
    }
    return key;
}

static void KeyPageRect(uint32_t key, int* x, int* y, int* w) {
    *x = (int)(key & 0xF) * 64;
    *y = (int)((key >> 4) & 1) * 256;
    *w = 64 << ((key >> 5) & 3); // halfwords: 64 4bpp, 128 8bpp, 256 16bpp
}

static inline bool Overlaps(
    int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

static void InvalidateVram(int x, int y, int w, int h) {
    memo_slot = NULL;
    frame24_dirty = true;
    for (int i = 0; i < tex_slot_count; i++) {
        TexSlot* s = &tex_slots[i];
        if (!s->valid || s->dirty) {
            continue;
        }
        int px, py, pw;
        KeyPageRect(s->key, &px, &py, &pw);
        bool hit = Overlaps(x, y, w, h, px, py, pw, 256);
        // pages at the right edge of VRAM wrap to x 0
        if (!hit && px + pw > VRAM_W) {
            hit = Overlaps(x, y, w, h, 0, py, px + pw - VRAM_W, 256);
        }
        uint32_t bpp = (s->key >> 5) & 3;
        if (!hit && bpp < 2) {
            uint32_t clut = s->key >> 8 & 0xFFFF;
            int cx = (int)(clut & 0x3F) * 16;
            int cy = (int)(clut >> 6) & 0x1FF;
            hit = Overlaps(x, y, w, h, cx, cy, bpp ? 256 : 16, 1);
        }
        if (hit) {
            s->dirty = true;
        }
    }
}

// PS1 0bbbbbgggggrrrrr (bit 15 = STP) to ARGB1555; colour 0 is transparent
static inline uint16_t Convert(uint16_t c, int raw) {
    uint16_t rgb = (uint16_t)(((c & 0x1F) << 10) | (c & 0x3E0) |
                              ((c >> 10) & 0x1F));
    return raw || c ? (uint16_t)(0x8000 | rgb) : 0;
}

static void DecodeSlot(TexSlot* s) {
    uint32_t key = s->key;
    int x0, y0, w;
    KeyPageRect(key, &x0, &y0, &w);
    int bpp = (int)(key >> 5) & 3;
    int raw = (int)(key >> 7) & 1;
    uint32_t* dst = (uint32_t*)s->ptr;
    if (bpp == 2) {
        for (int v = 0; v < 256; v++) {
            const uint16_t* row = &vram[((y0 + v) & (VRAM_H - 1)) * VRAM_W];
            for (int u = 0; u < 256; u += 2) {
                uint16_t a = Convert(row[(x0 + u) & (VRAM_W - 1)], raw);
                uint16_t b = Convert(row[(x0 + u + 1) & (VRAM_W - 1)], raw);
                *dst++ = a | ((uint32_t)b << 16);
            }
        }
        return;
    }
    uint32_t clut = (key >> 8) & 0xFFFF;
    int cx = (int)(clut & 0x3F) * 16;
    int cy = (int)(clut >> 6) & 0x1FF;
    int colors = bpp ? 256 : 16;
    uint16_t pal[256];
    for (int i = 0; i < colors; i++) {
        pal[i] = Convert(vram[cy * VRAM_W + ((cx + i) & (VRAM_W - 1))], raw);
    }
    for (int v = 0; v < 256; v++) {
        const uint16_t* row = &vram[((y0 + v) & (VRAM_H - 1)) * VRAM_W];
        if (bpp == 0) {
            for (int i = 0; i < 64; i++) {
                uint16_t h = row[(x0 + i) & (VRAM_W - 1)];
                *dst++ = pal[h & 0xF] | ((uint32_t)pal[(h >> 4) & 0xF] << 16);
                *dst++ = pal[(h >> 8) & 0xF] | ((uint32_t)pal[h >> 12] << 16);
            }
        } else {
            for (int i = 0; i < 128; i++) {
                uint16_t h = row[(x0 + i) & (VRAM_W - 1)];
                *dst++ = pal[h & 0xFF] | ((uint32_t)pal[h >> 8] << 16);
            }
        }
    }
}

static TexSlot* PickVictim(void) {
    TexSlot* best = NULL;
    for (int i = 0; i < tex_slot_count; i++) {
        TexSlot* s = &tex_slots[i];
        if (!s->valid) {
            return s;
        }
        // a slot drawn this frame is still read by the pending render
        if (s->used_frame == frame_no) {
            continue;
        }
        if (!best || s->last_used < best->last_used) {
            best = s;
        }
    }
    if (!best) {
        LOG_ONCE("texture cache full within one frame; textures may glitch");
        best = &tex_slots[0];
        for (int i = 1; i < tex_slot_count; i++) {
            if (tex_slots[i].last_used < best->last_used) {
                best = &tex_slots[i];
            }
        }
    }
    return best;
}

static TexSlot* FindTexture(u16 tpage, u16 clut, int variant) {
    uint32_t key = TexKey(tpage, clut, variant);
    if (memo_slot && memo_key == key) {
        return memo_slot;
    }
    TexSlot* s = NULL;
    for (int i = 0; i < tex_slot_count; i++) {
        if (tex_slots[i].valid && tex_slots[i].key == key) {
            s = &tex_slots[i];
            break;
        }
    }
    if (s && s->dirty && s->used_frame == frame_no) {
        // changed after being drawn this frame: decode the new contents
        // elsewhere so earlier primitives keep the old ones
        s->valid = false;
        s = NULL;
    }
    if (!s) {
        s = PickVictim();
        if (!s) {
            return NULL;
        }
        s->valid = true;
        s->dirty = true;
        s->key = key;
    }
    if (s->dirty) {
        DecodeSlot(s);
        s->dirty = false;
    }
    s->last_used = ++tex_clock;
    s->used_frame = frame_no;
    memo_slot = s;
    memo_key = key;
    return s;
}

static void CompileHeaderFmt(pvr_poly_hdr_t* hdr, pvr_ptr_t txr, int fmt,
                             int w, int h, int mode) {
    pvr_poly_cxt_t cxt;
    if (txr) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                         fmt | PVR_TXRFMT_NONTWIDDLED, w, h, txr,
                         PVR_FILTER_NONE);
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.alpha = false; // false enables the texture's alpha
        cxt.txr.uv_clamp = PVR_UVCLAMP_NONE;
    } else {
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    }
    cxt.gen.alpha = true;
    cxt.gen.shading = true;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = false;
    switch (mode) {
    case 0: // B/2 + F/2
    case BLEND_OPAQUE: // alpha 0 texels stay transparent
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        break;
    case 1: // B + F
    case 3: // B + F/4
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_ONE;
        break;
    case 2: // drawn by SubmitStrip as invert, add, invert
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_ONE;
        break;
    case BLEND_INVERT: // 1 - B
        cxt.blend.src = PVR_BLEND_INVDESTCOLOR;
        cxt.blend.dst = PVR_BLEND_ZERO;
        break;
    }
    pvr_poly_compile(hdr, &cxt);
}

static void CompileHeader(pvr_poly_hdr_t* hdr, pvr_ptr_t txr, int mode) {
    CompileHeaderFmt(hdr, txr, PVR_TXRFMT_ARGB1555, 256, 256, mode);
}

static void SendHeader(TexSlot* t, int mode) {
    int id = t ? (int)(t - tex_slots) * BLEND_MODES + mode : -1 - mode;
    if (id == cur_hdr) {
        return;
    }
    cur_hdr = id;
    if (!t) {
        pvr_prim(&col_hdr[mode], sizeof(pvr_poly_hdr_t));
        return;
    }
    if (!(t->hdr_ready & (1 << mode))) {
        CompileHeader(&t->hdr[mode], t->ptr, mode);
        t->hdr_ready |= (uint8_t)(1 << mode);
    }
    pvr_prim(&t->hdr[mode], sizeof(pvr_poly_hdr_t));
}

// ===== frame =====
static void UpdateScale(void) {
    scale_x = SCREEN_W / (float)game_w;
    scale_y = SCREEN_H / (float)game_h;
}

static bool InitPlatform(void) {
    if (is_init) {
        return true;
    }
    pvr_init_params_t params = {
        .opb_sizes = {PVR_BINSIZE_0, PVR_BINSIZE_0, PVR_BINSIZE_32,
                      PVR_BINSIZE_0, PVR_BINSIZE_0},
        .vertex_buf_size = PVR_VERTEX_BUF,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 1, // keep the PS1 drawing order
        .opb_overflow_count = 4,
    };
    if (pvr_init(&params) < 0) {
        WARNF("pvr_init failed");
        return false;
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    frame24 = pvr_mem_malloc(FRAME24_W * FRAME24_H * 2);
    size_t avail = pvr_mem_available();
    size_t reserve = 256 * 1024;
    tex_slot_count = avail > reserve ? (int)((avail - reserve) / TEX_BYTES) : 0;
    if (tex_slot_count > TEX_SLOTS_MAX) {
        tex_slot_count = TEX_SLOTS_MAX;
    }
    for (int i = 0; i < tex_slot_count; i++) {
        tex_slots[i].ptr = pvr_mem_malloc(TEX_BYTES);
        if (!tex_slots[i].ptr) {
            tex_slot_count = i;
            break;
        }
    }
    if (tex_slot_count < 4) {
        WARNF("only %d texture slots in PVR memory", tex_slot_count);
    }
    for (int m = 0; m < BLEND_MODES; m++) {
        CompileHeader(&col_hdr[m], NULL, m);
    }
    INFOF("PVR: %d texture slots, %u KB free", tex_slot_count,
          (unsigned)(pvr_mem_available() / 1024));
    is_init = true;
    return true;
}

static void BeginScene(void) {
    if (in_scene) {
        return;
    }
    pvr_wait_ready(); // the previous frame has finished reading textures
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_TR_POLY);
    in_scene = true;
    frame_no++;
    frame_vertices = 0;
    cur_hdr = -1;
    memo_slot = NULL;
    fb_origin_locked = false;
}

static void MaskBorders(void);

static void EndScene(void) {
    if (!in_scene) {
        return;
    }
    MaskBorders();
    pvr_list_finish();
    pvr_scene_finish();
    in_scene = false;
}


// 24-bit display: convert the displayed RGB888 rectangle and show it whole
static void Present24(void) {
    if (!display_enabled || !frame24 || !frame24_dirty) {
        return; // the PVR keeps showing the previous picture
    }
    int w = game_w > FRAME24_W ? FRAME24_W : game_w;
    int vis = game_h > 240 ? disp_lines * 2 : disp_lines;
    int h = vis > FRAME24_H ? FRAME24_H : vis;
    float top = (float)(game_h > 240 ? disp_top * 2 : disp_top) * scale_y;
    int tex_h = h > 256 ? 512 : 256;
    BeginScene(); // waits for the previous render, which read frame24
    for (int y = 0; y < h; y++) {
        const uint8_t* src = (const uint8_t*)&vram[((disp_area.y + y) &
                                                    (VRAM_H - 1)) * VRAM_W];
        src += disp_area.x * 2;
        uint32_t* dst = (uint32_t*)((uint16_t*)frame24 + y * FRAME24_W);
        for (int x = 0; x < w; x += 2, src += 6) {
            uint32_t a = ((src[0] >> 3) << 11) | ((src[1] >> 2) << 5) |
                         (src[2] >> 3);
            uint32_t b = ((src[3] >> 3) << 11) | ((src[4] >> 2) << 5) |
                         (src[5] >> 3);
            *dst++ = a | (b << 16);
        }
    }
    frame24_dirty = false;
    if (frame24_hdr_h != tex_h) {
        CompileHeaderFmt(&frame24_hdr, frame24, PVR_TXRFMT_RGB565, FRAME24_W,
                         tex_h, BLEND_OPAQUE);
        frame24_hdr_h = tex_h;
    }
    cur_hdr = -100; // a header no cache entry matches
    pvr_prim(&frame24_hdr, sizeof(frame24_hdr));
    uint32_t c = 0xFFFFFFFF;
    float u1 = (float)w / FRAME24_W, v1 = (float)h / tex_h;
    pvr_vertex_t out = {0};
    out.z = 1.0f;
    out.argb = c;
    const float xs[4] = {0, SCREEN_W, 0, SCREEN_W};
    float bottom = top + (float)h * scale_y;
    const float ys[4] = {top, top, bottom, bottom};
    const float us[4] = {0, u1, 0, u1};
    const float vs[4] = {0, 0, v1, v1};
    for (int i = 0; i < 4; i++) {
        out.flags = i < 3 ? PVR_CMD_VERTEX : PVR_CMD_VERTEX_EOL;
        out.x = xs[i];
        out.y = ys[i];
        out.u = us[i];
        out.v = vs[i];
        pvr_prim(&out, sizeof(out));
    }
}

// mode < 0: vertical blanks since start-up; 1: no wait; 0: wait for the
// next blank; n > 1: wait n blanks. The frame drawn so far is shown.
int Psyz_VideoVSync(int mode) {
    if (!boot_us) {
        boot_us = timer_us_gettime64();
    }
    if (mode < 0) {
        return (int)((timer_us_gettime64() - boot_us) / 16683u);
    }
    if (mode == 1) {
        return 0;
    }
    if (rgb24 && is_init) {
        Present24();
    }
    EndScene();
    int count = mode > 1 ? mode : 1;
    while (count--) {
        vid_waitvbl();
    }
    return 0;
}

// ===== primitive output =====
typedef struct {
    float x, y, u, v;
    uint32_t argb;
} Vtx;

static inline bool CanDraw(int vertices) {
    if (!display_enabled || rgb24) {
        return false;
    }
    if (!is_init && !InitPlatform()) {
        return false;
    }
    BeginScene();
    if (frame_vertices + vertices > MAX_FRAME_VERTICES) {
        LOG_ONCE("PVR vertex budget exhausted, dropping primitives");
        return false;
    }
    frame_vertices += vertices;
    return true;
}

static inline float MapX(int x) {
    return (float)(x + draw_offset.x - fb_origin.x) * scale_x;
}
static inline float MapY(int y) {
    return (float)(y + draw_offset.y - fb_origin.y + disp_top) * scale_y;
}

static inline float FbY(int y) {
    return (float)(y - fb_origin.y + disp_top) * scale_y;
}

// the PS1 shows nothing outside the display range: cover it
static void MaskBorders(void) {
    int lines = game_h > 240 ? disp_lines * 2 : disp_lines;
    int top = game_h > 240 ? disp_top * 2 : disp_top;
    float y0 = (float)top * scale_y;
    float y1 = (float)(top + lines) * scale_y;
    float bands[2][2] = {{0.0f, y0}, {y1, SCREEN_H}};
    for (int i = 0; i < 2; i++) {
        if (bands[i][1] - bands[i][0] < 0.5f) {
            continue;
        }
        cur_hdr = -1 - BLEND_OPAQUE;
        pvr_prim(&col_hdr[BLEND_OPAQUE], sizeof(pvr_poly_hdr_t));
        pvr_vertex_t out = {0};
        out.z = 1.0f;
        out.argb = 0xFF000000;
        const float xs[4] = {0, SCREEN_W, 0, SCREEN_W};
        const float ys[4] = {bands[i][0], bands[i][0], bands[i][1], bands[i][1]};
        for (int k = 0; k < 4; k++) {
            out.flags = k < 3 ? PVR_CMD_VERTEX : PVR_CMD_VERTEX_EOL;
            out.x = xs[k];
            out.y = ys[k];
            pvr_prim(&out, sizeof(out));
        }
    }
}

static void SubmitVertices(const Vtx* v, int n, bool white) {
    pvr_vertex_t out;
    out.z = 1.0f;
    out.oargb = 0;
    for (int i = 0; i < n; i++) {
        out.flags = i + 1 < n ? PVR_CMD_VERTEX : PVR_CMD_VERTEX_EOL;
        out.x = v[i].x;
        out.y = v[i].y;
        out.u = v[i].u;
        out.v = v[i].v;
        out.argb = white ? 0xFFFFFFFF : v[i].argb;
        pvr_prim(&out, sizeof(out));
    }
}

// n vertices in triangle-strip order
static void SubmitStrip(const Vtx* v, int n, TexSlot* t, int mode) {
    if (mode != 2) {
        SendHeader(t, mode);
        SubmitVertices(v, n, false);
        return;
    }
    // B - F has no PVR blend: invert the covered pixels (1 - B), add F
    // (clamped at 1), invert again: B - F clamped at 0, as on the PS1
    if (frame_vertices + 2 * n > MAX_FRAME_VERTICES) {
        return;
    }
    frame_vertices += 2 * n;
    SendHeader(NULL, BLEND_INVERT);
    SubmitVertices(v, n, true);
    SendHeader(t, 2);
    SubmitVertices(v, n, false);
    SendHeader(NULL, BLEND_INVERT);
    SubmitVertices(v, n, true);
}

static inline uint32_t Argb(uint8_t r, uint8_t g, uint8_t b, int mode) {
    return ((uint32_t)blend_alpha[mode] << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | b;
}

// PS1 texture modulation: 0x80 is 1.0. The PVR tops out at 1.0.
static inline uint8_t TexMod(uint8_t c) { return c >= 0x80 ? 255 : c * 2; }

static inline uint32_t TexArgb(uint8_t r, uint8_t g, uint8_t b, int mode) {
    return Argb(TexMod(r), TexMod(g), TexMod(b), mode);
}

static inline int BlendMode(bool semitrans, u16 tpage) {
    return semitrans ? (tpage >> 5) & 3 : BLEND_OPAQUE;
}

// axis-aligned rectangle in PS1 draw coordinates
static void EmitRect(int x0, int y0, int x1, int y1, int u0, int v0, int u1,
                     int v1, uint8_t r, uint8_t g, uint8_t b, bool textured,
                     bool raw_color, u16 tpage, u16 clut, bool semitrans) {
    if (!CanDraw(4)) {
        return;
    }
    int mode = BlendMode(semitrans, tpage);
    TexSlot* t = NULL;
    uint32_t c;
    if (textured) {
        t = FindTexture(tpage, clut, 0);
        c = raw_color ? Argb(255, 255, 255, mode) : TexArgb(r, g, b, mode);
    } else {
        c = Argb(r, g, b, mode);
    }
    float fx0 = MapX(x0), fx1 = MapX(x1), fy0 = MapY(y0), fy1 = MapY(y1);
    float fu0 = u0 / 256.0f, fu1 = u1 / 256.0f;
    float fv0 = v0 / 256.0f, fv1 = v1 / 256.0f;
    Vtx v[4] = {
        {fx0, fy0, fu0, fv0, c},
        {fx1, fy0, fu1, fv0, c},
        {fx0, fy1, fu0, fv1, c},
        {fx1, fy1, fu1, fv1, c},
    };
    SubmitStrip(v, 4, t, mode);
}

// one framebuffer-space rectangle drawn from VRAM itself (uploads and copies
// that land on screen), split along 16bpp texture pages
static void EmitVramRegionQuad(int x, int y, int w, int h) {
    if (!display_enabled || rgb24 || (!is_init && !InitPlatform())) {
        return;
    }
    int x0 = x > fb_origin.x ? x : fb_origin.x;
    int y0 = y > fb_origin.y ? y : fb_origin.y;
    int x1 = x + w < fb_origin.x + game_w ? x + w : fb_origin.x + game_w;
    int y1 = y + h < fb_origin.y + game_h ? y + h : fb_origin.y + game_h;
    for (int py = y0; py < y1;) {
        int page_y = py & ~255;
        int ny = page_y + 256 < y1 ? page_y + 256 : y1;
        for (int px = x0; px < x1;) {
            int page_x = px & ~63;
            int nx = page_x + 256 < x1 ? page_x + 256 : x1;
            if (!CanDraw(4)) {
                return;
            }
            u16 tpage = (u16)((page_x >> 6) | ((page_y >> 8) << 4) | (2 << 7));
            TexSlot* t = FindTexture(tpage, 0, RAW_TEXTURE);
            uint32_t c = Argb(255, 255, 255, BLEND_OPAQUE);
            float sx0 = (float)(px - fb_origin.x) * scale_x;
            float sx1 = (float)(nx - fb_origin.x) * scale_x;
            float sy0 = FbY(py);
            float sy1 = FbY(ny);
            float u0 = (px - page_x) / 256.0f, u1 = (nx - page_x) / 256.0f;
            float v0 = (py - page_y) / 256.0f, v1 = (ny - page_y) / 256.0f;
            Vtx v[4] = {
                {sx0, sy0, u0, v0, c},
                {sx1, sy0, u1, v0, c},
                {sx0, sy1, u0, v1, c},
                {sx1, sy1, u1, v1, c},
            };
            SubmitStrip(v, 4, t, BLEND_OPAQUE);
            px = nx;
        }
        py = ny;
    }
}

static void EmitFill(int x, int y, int w, int h, uint8_t r, uint8_t g,
                     uint8_t b) {
    int x0 = x > fb_origin.x ? x : fb_origin.x;
    int y0 = y > fb_origin.y ? y : fb_origin.y;
    int x1 = x + w < fb_origin.x + game_w ? x + w : fb_origin.x + game_w;
    int y1 = y + h < fb_origin.y + game_h ? y + h : fb_origin.y + game_h;
    if (x0 >= x1 || y0 >= y1 || !CanDraw(4)) {
        return;
    }
    uint32_t c = Argb(r, g, b, BLEND_OPAQUE);
    float sx0 = (float)(x0 - fb_origin.x) * scale_x;
    float sx1 = (float)(x1 - fb_origin.x) * scale_x;
    float sy0 = FbY(y0);
    float sy1 = FbY(y1);
    Vtx v[4] = {
        {sx0, sy0, 0, 0, c},
        {sx1, sy0, 0, 0, c},
        {sx0, sy1, 0, 0, c},
        {sx1, sy1, 0, 0, c},
    };
    SubmitStrip(v, 4, NULL, BLEND_OPAQUE);
}

// a line as a quad one PS1 pixel wide
static void EmitLine(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1,
                     int mode) {
    if (!CanDraw(4)) {
        return;
    }
    float ax = MapX(x0), ay = MapY(y0), bx = MapX(x1), by = MapY(y1);
    int dx = x1 - x0, dy = y1 - y0;
    float ox, oy;
    if ((dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy)) {
        ox = 0.0f; // mostly horizontal: thicken downwards
        oy = scale_y;
        bx += dx >= 0 ? scale_x : -scale_x; // PS1 lines include the end pixel
    } else {
        ox = scale_x;
        oy = 0.0f;
        by += dy >= 0 ? scale_y : -scale_y;
    }
    Vtx v[4] = {
        {ax, ay, 0, 0, c0},
        {bx, by, 0, 0, c1},
        {ax + ox, ay + oy, 0, 0, c0},
        {bx + ox, by + oy, 0, 0, c1},
    };
    SubmitStrip(v, 4, NULL, mode);
}

// ===== draw environment =====
void Draw_Reset(void) {
    if (!is_init && !InitPlatform()) {
        return;
    }
    memset(vram, 0, sizeof(vram));
    for (int i = 0; i < tex_slot_count; i++) {
        tex_slots[i].valid = false;
    }
    memo_slot = NULL;
    cur_tpage = 0;
    draw_offset.x = draw_offset.y = 0;
    draw_area_start.x = draw_area_start.y = 0;
    draw_area_end.x = game_w - 1;
    draw_area_end.y = game_h - 1;
    fb_origin.x = fb_origin.y = 0;
    disp_origin_count = 0;
    display_enabled = true;
}

void Draw_DisplayEnable(unsigned int on) { display_enabled = on != 0; }

static void TryLockFbOrigin(void) {
    if (fb_origin_locked) {
        return;
    }
    int x = draw_area_start.x, y = draw_area_start.y;
    bool is_framebuffer = disp_origin_count == 0;
    int ox = x, oy = y;
    for (int i = 0; i < disp_origin_count; i++) {
        if (x >= disp_origins[i].x && x < disp_origins[i].x + game_w &&
            y >= disp_origins[i].y && y < disp_origins[i].y + game_h) {
            ox = disp_origins[i].x;
            oy = disp_origins[i].y;
            is_framebuffer = true;
            break;
        }
    }
    if (is_framebuffer) {
        fb_origin_locked = true;
        fb_origin.x = ox;
        fb_origin.y = oy;
    }
}

void Draw_DisplayArea(unsigned int x, unsigned int y) {
    if (disp_area.x != (int)x || disp_area.y != (int)y) {
        frame24_dirty = true;
    }
    disp_area.x = (int)x;
    disp_area.y = (int)y;
    // remember the framebuffer locations (typically two, alternating)
    for (int i = 0; i < disp_origin_count; i++) {
        if (disp_origins[i].x == (int)x && disp_origins[i].y == (int)y) {
            return;
        }
    }
    if (disp_origin_count < 2) {
        disp_origins[disp_origin_count].x = (int)x;
        disp_origins[disp_origin_count].y = (int)y;
        disp_origin_count++;
    } else {
        disp_origins[0] = disp_origins[1];
        disp_origins[1].x = (int)x;
        disp_origins[1].y = (int)y;
    }
    TryLockFbOrigin();
}

void Draw_DisplayHorizontalRange(unsigned int start, unsigned int end) {
    (void)start;
    (void)end;
}

void Draw_DisplayVerticalRange(unsigned int start, unsigned int end) {
    int top = (int)start - 16;
    int lines = (int)end - (int)start;
    if (lines <= 0 || lines > 256 || top < -16 || top > 64) {
        top = 0;
        lines = 240;
    }
    if (top != disp_top || lines != disp_lines) {
        frame24_dirty = true;
    }
    disp_top = top;
    disp_lines = lines;
}

void Draw_SetDisplayMode(DisplayMode* mode) {
    if (rgb24 != (mode->rgb24 != 0)) {
        frame24_dirty = true;
    }
    rgb24 = mode->rgb24 != 0;
    if (mode->horizontal_resolution_368) {
        game_w = 368;
    } else {
        static const int widths[4] = {256, 320, 512, 640};
        game_w = widths[mode->horizontal_resolution & 3];
    }
    game_h = mode->vertical_resolution ? 480 : 240;
    UpdateScale();
}

void Draw_SetTexpageMode(ParamDrawTexpageMode* p) {
    cur_tpage = *(u_short*)p & 0x1FF;
}

void Draw_SetTextureWindow(unsigned int mask_x, unsigned int mask_y,
                           unsigned int off_x, unsigned int off_y) {
    if (off_x || off_y || mask_x != 0xFFFFFFFF || mask_y != 0xFFFFFFFF) {
        LOG_ONCE("texture windows not supported");
    }
}

void Draw_SetAreaStart(int x, int y) {
    draw_area_start.x = x;
    draw_area_start.y = y;
    TryLockFbOrigin();
}

void Draw_SetAreaEnd(int x, int y) {
    draw_area_end.x = x;
    draw_area_end.y = y;
}

void Draw_SetOffset(int x, int y) {
    x %= VRAM_W;
    y %= VRAM_H;
    draw_offset.x = x < 0 ? x + VRAM_W : x;
    draw_offset.y = y < 0 ? y + VRAM_H : y;
}

int Draw_SetHorizontalGrid(unsigned int source_width, unsigned int target_width) {
    if (source_width == 0 || target_width == 0) {
        return -1;
    }
    return source_width == target_width ? 0 : -1;
}

void Draw_SetMask(int bit0, int bit1) {
    if (bit0 || bit1) {
        LOG_ONCE("mask bits not supported");
    }
}

void Draw_PutDispEnv(DISPENV* disp) { (void)disp; }

// ===== image transfers =====
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
    InvalidateVram(x, y, w, h);
    EmitFill(x, y, w, h, r, g, b);
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
    InvalidateVram(x, y, w, h);
    EmitVramRegionQuad(x, y, w, h);
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
        memmove(&vram[(y + r) * VRAM_W + x], &vram[(sy + r) * VRAM_W + sx],
                w * 2);
    }
    InvalidateVram((int)x, (int)y, w, h);
    EmitVramRegionQuad((int)x, (int)y, w, h);
}

// ===== PS1 GP0 packets =====
#define SEMITRANSP 0x02
#define TEXTURED 0x04
#define EXTRA_VERTEX 0x08
#define GOURAUD 0x10

// real hardware uses XY coords as signed 11-bit
static inline int s11(uint32_t v) {
    return (int)((((v & 0x7FF) ^ 1024)) - 1024);
}

// Words in a GP0 drawing packet, as on the PS1 GPU.
static int PacketLength(const u_long* packets, int max_len) {
    int code = (int)(*packets >> 24) & 0xFF;
    int len;
    if (code >= 0x20 && code < 0x40) {
        int verts = (code & EXTRA_VERTEX) ? 4 : 3;
        int tex = (code & TEXTURED) ? 1 : 0;
        int gouraud = (code & GOURAUD) ? 1 : 0;
        len = 1 + verts * (1 + tex) + (gouraud ? verts - 1 : 0);
    } else if (code >= 0x40 && code < 0x60) {
        int gouraud = (code & GOURAUD) ? 1 : 0;
        if (code & EXTRA_VERTEX) {
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
        int tex = (code & TEXTURED) ? 1 : 0;
        int variable = ((code >> 3) & 3) == 0;
        len = 2 + tex + variable;
    } else {
        len = 1;
    }
    return len > max_len ? max_len : len;
}

static void PushPoly(const u_long* p, int code) {
    bool textured = (code & TEXTURED) != 0;
    bool gouraud = (code & GOURAUD) != 0;
    bool semitrans = (code & SEMITRANSP) != 0;
    bool raw = textured && (code & 1); // texture without colour modulation
    int n = (code & EXTRA_VERTEX) ? 4 : 3;
    if (!CanDraw(n)) {
        return;
    }
    int x[4], y[4], u[4] = {0}, v[4] = {0};
    uint32_t col[4];
    u16 clut = 0, tpage = cur_tpage;
    const u_long* w = p;
    col[0] = (uint32_t)*w++;
    for (int i = 0; i < n; i++) {
        if (gouraud && i > 0) {
            col[i] = (uint32_t)*w++;
        }
        uint32_t xy = (uint32_t)*w++;
        x[i] = s11(xy);
        y[i] = s11(xy >> 16);
        if (textured) {
            uint32_t uv = (uint32_t)*w++;
            u[i] = uv & 0xFF;
            v[i] = (uv >> 8) & 0xFF;
            if (i == 0) {
                clut = (u16)(uv >> 16);
            } else if (i == 1) {
                tpage = (u16)(uv >> 16);
            }
        }
    }
    if (!gouraud || raw) {
        for (int i = 1; i < n; i++) {
            col[i] = col[0];
        }
    }
    int mode = BlendMode(semitrans, tpage);
    TexSlot* t = NULL;
    if (textured) {
        // flipped UVs on the PS1 sample one texel further
        bool fix_u = ((x[0] > x[1]) ^ (u[0] > u[1])) |
                     ((x[0] > x[2]) ^ (u[0] > u[2]));
        bool fix_v = ((y[0] > y[1]) ^ (v[0] > v[1])) |
                     ((y[0] > y[2]) ^ (v[0] > v[2]));
        for (int i = 0; i < n; i++) {
            u[i] += fix_u;
            v[i] += fix_v;
        }
        t = FindTexture(tpage, clut, 0);
    }
    Vtx out[4];
    for (int i = 0; i < n; i++) {
        uint8_t r = (uint8_t)col[i], g = (uint8_t)(col[i] >> 8),
                b = (uint8_t)(col[i] >> 16);
        out[i].x = MapX(x[i]);
        out[i].y = MapY(y[i]);
        out[i].u = u[i] / 256.0f;
        out[i].v = v[i] / 256.0f;
        out[i].argb = !textured ? Argb(r, g, b, mode)
                      : raw     ? Argb(255, 255, 255, mode)
                                : TexArgb(r, g, b, mode);
    }
    SubmitStrip(out, n, t, mode);
}

static void PushRect(const u_long* p, int code) {
    bool textured = (code & TEXTURED) != 0;
    bool semitrans = (code & SEMITRANSP) != 0;
    bool raw = textured && (code & 1);
    uint32_t c = (uint32_t)p[0];
    uint32_t xy = (uint32_t)p[1];
    int x = s11(xy), y = s11(xy >> 16);
    int u = 0, v = 0;
    u16 clut = 0;
    int i = 2;
    if (textured) {
        uint32_t uv = (uint32_t)p[i++];
        u = uv & 0xFF;
        v = (uv >> 8) & 0xFF;
        clut = (u16)(uv >> 16);
    }
    int w, h;
    switch ((code >> 3) & 3) {
    case 1:
        w = h = 1;
        break;
    case 2:
        w = h = 8;
        break;
    case 3:
        w = h = 16;
        break;
    default: {
        uint32_t wh = (uint32_t)p[i];
        w = (int)(wh & 0x3FF);
        h = (int)((wh >> 16) & 0x1FF);
        break;
    }
    }
    EmitRect(x, y, x + w, y + h, u, v, u + w, v + h, (uint8_t)c,
             (uint8_t)(c >> 8), (uint8_t)(c >> 16), textured, raw, cur_tpage,
             clut, semitrans);
}

static void PushLine(const u_long* p, int len, int code) {
    bool gouraud = (code & GOURAUD) != 0;
    int mode = BlendMode((code & SEMITRANSP) != 0, cur_tpage);
    uint32_t c1 = (uint32_t)p[0];
    int i = 1;
    uint32_t xy = (uint32_t)p[i++];
    int x0 = s11(xy), y0 = s11(xy >> 16);
    while (i < len) {
        uint32_t c0 = c1;
        if (gouraud) {
            if ((p[i] & 0xF000F000) == 0x50005000) {
                break;
            }
            c1 = (uint32_t)p[i++];
            if (i >= len) {
                break;
            }
        }
        if ((p[i] & 0xF000F000) == 0x50005000 && (code & EXTRA_VERTEX)) {
            break;
        }
        xy = (uint32_t)p[i++];
        int x1 = s11(xy), y1 = s11(xy >> 16);
        EmitLine(x0, y0,
                 Argb((uint8_t)c0, (uint8_t)(c0 >> 8), (uint8_t)(c0 >> 16),
                      mode),
                 x1, y1,
                 Argb((uint8_t)c1, (uint8_t)(c1 >> 8), (uint8_t)(c1 >> 16),
                      mode),
                 mode);
        x0 = x1;
        y0 = y1;
        if (!(code & EXTRA_VERTEX)) {
            break;
        }
    }
}

int Draw_PushPrim(u_long* packets, int max_len) {
    int len = PacketLength(packets, max_len);
    int code = (int)(*packets >> 24) & 0xFF;
    int need = code >= 0x40 && code < 0x60 && (code & EXTRA_VERTEX) ? 3 : len;
    if (len < need || max_len <= 0) {
        return len; // truncated packet
    }
    if (code >= 0x20 && code < 0x40) {
        PushPoly(packets, code);
    } else if (code >= 0x40 && code < 0x60) {
        PushLine(packets, len, code);
    } else if (code >= 0x60 && code < 0x80) {
        PushRect(packets, code);
    }
    return len;
}

void Draw_ResetBuffer(void) {}
void Draw_FlushBuffer(void) {}
int Draw_ExequeSync(void) { return 0; }
