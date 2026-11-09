/* Software stand-in for the PVR: rasterises the submitted strips with the
 * header's texture, modulation and blend factors into a 640x480 RGB image,
 * written out as a PPM on every pvr_scene_finish(). Edges shared by two
 * triangles of a strip are shaded twice (no fill rule), which shows as thin
 * seams inside translucent quads; the PVR does not do that. */
#include <dc/pvr.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 640
#define H 480
static float fb[H][W][3];
static float bg[3];
static pvr_poly_hdr_t cur;
static pvr_vertex_t strip[64];
static int nstrip, frame, prims, headers;
int pvr_init(const pvr_init_params_t *p) { (void)p; return 0; }
void pvr_set_bg_color(float r, float g, float b) { bg[0] = r; bg[1] = g; bg[2] = b; }
size_t pvr_mem_available(void) { return 4u << 20; }
pvr_ptr_t pvr_mem_malloc(size_t n) { return calloc(1, n); }
void pvr_poly_cxt_txr(pvr_poly_cxt_t *d, pvr_list_t l, int f, int w, int h, pvr_ptr_t t, int fl) {
    memset(d, 0, sizeof(*d)); d->list_type = l; d->txr.enable = 1; d->txr.format = f;
    d->txr.width = w; d->txr.height = h; d->txr.base = t; d->txr.filter = fl;
    d->blend.src = PVR_BLEND_SRCALPHA; d->blend.dst = PVR_BLEND_INVSRCALPHA; d->txr.env = PVR_TXRENV_MODULATE;
}
void pvr_poly_cxt_col(pvr_poly_cxt_t *d, pvr_list_t l) {
    memset(d, 0, sizeof(*d)); d->list_type = l;
    d->blend.src = PVR_BLEND_SRCALPHA; d->blend.dst = PVR_BLEND_INVSRCALPHA;
}
void pvr_poly_compile(pvr_poly_hdr_t *d, const pvr_poly_cxt_t *s) { d->magic = 0x48445221; d->cxt = *s; }
int pvr_wait_ready(void) { return 0; }
void pvr_scene_begin(void) {
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) memcpy(fb[y][x], bg, sizeof bg);
    prims = headers = 0;
}
int pvr_list_begin(pvr_list_t l) { (void)l; return 0; }
int pvr_list_finish(void) { return 0; }
static float Factor(int f, const float *s, float sa, const float *d, int c, int is_dst) {
    switch (f) {
    case PVR_BLEND_ZERO: return 0;
    case PVR_BLEND_ONE: return 1;
    case PVR_BLEND_DESTCOLOR: return is_dst ? s[c] : d[c];   /* "other" colour */
    case PVR_BLEND_INVDESTCOLOR: return is_dst ? 1 - s[c] : 1 - d[c];
    case PVR_BLEND_SRCALPHA: return sa;
    case PVR_BLEND_INVSRCALPHA: return 1 - sa;
    default: return 1;
    }
}
static void Unpack(uint32_t c, float *o) {
    o[0] = ((c >> 24) & 255) / 255.f; o[1] = ((c >> 16) & 255) / 255.f;
    o[2] = ((c >> 8) & 255) / 255.f; o[3] = (c & 255) / 255.f;
}
static void Tri(const pvr_vertex_t *a, const pvr_vertex_t *b, const pvr_vertex_t *c) {
    const pvr_poly_cxt_t *x = &cur.cxt;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (fabsf(area) < 1e-6f) return;
    int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    float ca[4], cb[4], cc[4];
    Unpack(a->argb, ca); Unpack(b->argb, cb); Unpack(c->argb, cc);
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        float sx = px + 0.5f, sy = py + 0.5f;
        float w0 = ((b->x - sx) * (c->y - sy) - (c->x - sx) * (b->y - sy)) / area;
        float w1 = ((c->x - sx) * (a->y - sy) - (a->x - sx) * (c->y - sy)) / area;
        float w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float col[4];
        for (int k = 0; k < 4; k++) col[k] = x->gen.shading ? w0 * ca[k] + w1 * cb[k] + w2 * cc[k] : cc[k];
        float sa = col[0], s[3] = {col[1], col[2], col[3]};
        if (x->txr.enable) {
            float u = w0 * a->u + w1 * b->u + w2 * c->u, v = w0 * a->v + w1 * b->v + w2 * c->v;
            int tw = x->txr.width, th = x->txr.height;
            int tu = (int)floorf(u * tw) & (tw - 1), tv = (int)floorf(v * th) & (th - 1);
            uint16_t t = ((uint16_t *)x->txr.base)[tv * tw + tu];
            float ta, tr, tg, tb;
            if (x->txr.format & PVR_TXRFMT_RGB565) {
                ta = 1; tr = ((t >> 11) & 31) / 31.f; tg = ((t >> 5) & 63) / 63.f; tb = (t & 31) / 31.f;
            } else {
                ta = (t >> 15) & 1; tr = ((t >> 10) & 31) / 31.f; tg = ((t >> 5) & 31) / 31.f; tb = (t & 31) / 31.f;
            }
            s[0] *= tr; s[1] *= tg; s[2] *= tb;
            if (!x->txr.alpha) sa *= ta;
        }
        float *d = fb[py][px];
        float out[3];
        for (int k = 0; k < 3; k++) {
            float v = s[k] * Factor(x->blend.src, s, sa, d, k, 0) + d[k] * Factor(x->blend.dst, s, sa, d, k, 1);
            out[k] = v > 1 ? 1 : v < 0 ? 0 : v;
        }
        memcpy(d, out, sizeof out);
    }
}
int pvr_prim(const void *data, size_t size) {
    const pvr_poly_hdr_t *h = data;
    if (size == sizeof(pvr_poly_hdr_t) && h->magic == 0x48445221) { cur = *h; headers++; return 0; }
    const pvr_vertex_t *v = data;
    strip[nstrip++] = *v;
    if (v->flags == PVR_CMD_VERTEX_EOL) {
        for (int i = 2; i < nstrip; i++) Tri(&strip[i - 2], &strip[i - 1], &strip[i]);
        nstrip = 0; prims++;
    }
    return 0;
}
int pvr_scene_finish(void) {
    char name[64]; snprintf(name, sizeof name, "frame%d.ppm", frame++);
    FILE *f = fopen(name, "wb"); fprintf(f, "P6 %d %d 255\n", W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        for (int k = 0; k < 3; k++) fputc((int)(fb[y][x][k] * 255 + 0.5f), f);
    fclose(f);
    printf("%s: %d strips, %d header changes\n", name, prims, headers);
    return 0;
}
void vid_waitvbl(void) {}
uint64_t timer_us_gettime64(void) { return 1; }
