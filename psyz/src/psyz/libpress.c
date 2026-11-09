#include <psyz.h>
#include <libpress.h>
#include <psyz/log.h>
#include <stdlib.h>
#include <string.h>
#include "../internal.h"
#include "../../../decomp/src/libpress/libpress_private.h"

/* Native implementation of the MDEC commands used by LIBPRESS. Serialized
 * words are always 32 bits, even where the SDK's u_long is a native pointer. */
static struct {
    DECDCTENV env;
    u8* output;
    size_t capacity, length, cursor;
    u32 command;
    int error;
} mdec;

static const u8 scan[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

static int clamp(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}
static int sign_bits(unsigned value, unsigned bits) {
    unsigned sign = 1u << (bits - 1);
    return (int)(value & (sign - 1)) - (int)(value & sign);
}
static u16 read16(const u8* p) { return p[0] | (u16)p[1] << 8; }
static u32 read32(const u8* p) { return read16(p) | (u32)read16(p + 2) << 16; }

// dct >> 3, the matrix both IDCT passes multiply by; set at each decode
static int idct_matrix[64];
// Whether idct_matrix mirrors like the standard one (see inverse_dct_mirrored)
static int idct_mirrored;
// idct_matrix as 16-bit values (they fit in 13 bits) for 16-bit multiplies
static short idct_matrix16[64];

/* A decoded block, by column. Each column lists its rows in slot order
 * 0, 2, 4, 6, 1, 3, 5, 7 (even rows first, see inverse_dct_mirrored). */
typedef struct {
    short column[8][8]; // [column][slot]; valid for the columns in mask
    unsigned mask;      // columns holding a nonzero coefficient
} Block;

static const unsigned char row_slot[8] = {0, 4, 1, 5, 2, 6, 3, 7};

// The MDEC's IDCT: two passes of an 8x8 matrix product, each rounded to
// (sum + 0xfff) >> 13, then the result wrapped to 9 signed bits.
// Pass 1: work[c][x] = sum over r of in[r][c] * M[r][x]
// Pass 2: out[y][x] = sum over c of work[c][y] * M[c][x]
// 32-bit sums are exact: coefficients are clamped to 11 bits and dct >> 3
// is within 13, so pass 1 sums stay under 2^26 and pass 2 (inputs under 2^13)
// under 2^28. This is the plain form, for a matrix other than the SDK's.
static void inverse_dct_generic(const Block* in, int out[64]) {
    int work[8][8];
    for (unsigned c = 0; c < 8; ++c)
        for (unsigned x = 0; x < 8; ++x) {
            int sum = 0;
            if (in->mask & (1u << c))
                for (unsigned r = 0; r < 8; ++r)
                    sum += in->column[c][row_slot[r]] * idct_matrix[r * 8 + x];
            work[c][x] = (sum + 0xfff) >> 13;
        }
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 8; ++x) {
            int sum = 0;
            for (unsigned c = 0; c < 8; ++c)
                sum += work[c][y] * idct_matrix[c * 8 + x];
            out[y * 8 + x] = (int)((unsigned)((sum + 0xfff) >> 13) << 23) >> 23;
        }
}

// The SDK's matrix (the rounded DCT basis, which the game keeps) mirrors
// exactly: an even row reads the same from both ends, and in an odd row
// M[r][7 - x] is ~M[r][x] = -M[r][x] - 1. So for one pass over a vector v,
//   E[x] = sum over even r of v[r] * M[r][x]   (x < 4)
//   F[x] = E[x] + sum over odd r of v[r] * M[r][x]
//   S    = sum over odd r of v[r]
//   result[x] = F[x], result[7 - x] = 2 * E[x] - F[x] - S,
// the same integers as the full product with half the multiplications.
static int matrix_mirrors(void) {
    for (unsigned r = 0; r < 8; ++r)
        for (unsigned x = 0; x < 4; ++x) {
            int a = idct_matrix[r * 8 + x], b = idct_matrix[r * 8 + 7 - x];
            if (r & 1 ? b != ~a : b != a)
                return 0;
        }
    return 1;
}

#define ROUND13(sum) (((sum) + 0xfff) >> 13)
// ROUND13(sum) wrapped to 9 signed bits: bits 13-21 of sum + 0xfff
#define ROUND13_WRAP(sum) ((int)((unsigned)((sum) + 0xfff) << 10) >> 23)

/* Tables for inverse_dct_mirrored, set with idct_matrix when it mirrors.
 * Vectors list the even rows (or columns) first: slot order 0, 2, 4, 6, 1,
 * 3, 5, 7, so each half is one run of multiply-accumulates. */
static short pass1_matrix[4][8];    // [x][slot] = M[row of slot][x]
static short pass2_matrix[5][4][8]; // [n][x][i] = M[2i][x] for i < n, then M[2j + 1][x]
static unsigned char prefix_counts[256];   // column mask -> even count | odd count << 4
static int row0_constant;                  // M[0][x] is the same for every x

static void set_mirrored_tables(void) {
    static const unsigned char slot_row[8] = {0, 2, 4, 6, 1, 3, 5, 7};
    for (unsigned x = 0; x < 4; ++x)
        for (unsigned slot = 0; slot < 8; ++slot)
            pass1_matrix[x][slot] = idct_matrix16[slot_row[slot] * 8 + x];
    for (unsigned n = 0; n <= 4; ++n)
        for (unsigned x = 0; x < 4; ++x) {
            unsigned i = 0;
            for (unsigned k = 0; k < n; ++k)
                pass2_matrix[n][x][i++] = idct_matrix16[2 * k * 8 + x];
            for (unsigned k = 0; k < 4; ++k)
                pass2_matrix[n][x][i++] = idct_matrix16[(2 * k + 1) * 8 + x];
            while (i < 8)
                pass2_matrix[n][x][i++] = 0;
        }
    for (unsigned mask = 0; mask < 256; ++mask) {
        unsigned even = 0, odd = 0;
        for (unsigned c = 0; c < 8; ++c)
            if (mask & (1u << c)) {
                if (c & 1)
                    odd = c / 2 + 1;
                else
                    even = c / 2 + 1;
            }
        prefix_counts[mask] = (unsigned char)(even | odd << 4);
    }
    row0_constant = 1;
    for (unsigned x = 1; x < 8; ++x)
        if (idct_matrix[x] != idct_matrix[0])
            row0_constant = 0;
}

/* Multiply-accumulate runs. On the SH-4, mac.w multiplies two 16-bit values
 * read through post-incremented pointers and adds the product to MACL in one
 * instruction (MACH:MACL is 64 bits with the S flag clear, so 32-bit sums
 * wrap like int arithmetic would; ours never reach 2^31). */
#if defined(__sh__)
#define MAC1 "mac.w @%[a]+,@%[b]+\n\t"
#define MACS_0 ""
#define MACS_1 MAC1
#define MACS_2 MAC1 MAC1
#define MACS_3 MAC1 MAC1 MAC1
#define MACS_4 MAC1 MAC1 MAC1 MAC1
// *even = sum of the first n a*b, *all = that plus the next m
#define DOT_RUNS(n, m, a, b, even, all)                                        \
    __asm__("clrmac\n\t" MACS_##n "sts macl,%[e]\n\t" MACS_##m "sts macl,%[f]"  \
            : [e] "=&r"(even), [f] "=r"(all), [a] "+r"(a), [b] "+r"(b)        \
            : "m"(*(const short(*)[8])(a)), "m"(*(const short(*)[8])(b))      \
            : "mach", "macl")
#else
#define DOT_RUNS(n, m, a, b, even, all)                                        \
    do {                                                                       \
        int sum_ = 0;                                                          \
        for (unsigned i_ = (n); i_; --i_)                                      \
            sum_ += *a++ * *b++;                                               \
        even = sum_;                                                           \
        for (unsigned i_ = (m); i_; --i_)                                      \
            sum_ += *a++ * *b++;                                               \
        all = sum_;                                                            \
    } while (0)
#endif

/* Pass 2 for n even and m odd touched-column slots: output row y takes
 * entry y of each touched column, so the matrix row is the column. */
typedef void (*Pass2)(const short work[8][8], const int odd_sum[8], int out[64]);
#define DEFINE_PASS2(n, m)                                                     \
    static void pass2_##n##_##m(                                               \
        const short work[8][8], const int odd_sum[8], int out[64]) {           \
        for (unsigned y = 0; y < 8; ++y, out += 8) {                           \
            int s = odd_sum[y];                                                \
            for (unsigned x = 0; x < 4; ++x) {                                 \
                const short* a = work[y];                                      \
                const short* b = pass2_matrix[n][x];                           \
                int e, f;                                                      \
                DOT_RUNS(n, m, a, b, e, f);                                    \
                out[x] = ROUND13_WRAP(f);                                      \
                out[7 - x] = ROUND13_WRAP(2 * e - f - s);                      \
            }                                                                  \
        }                                                                      \
    }
#define DEFINE_PASS2_ROW(n)                                                    \
    DEFINE_PASS2(n, 0) DEFINE_PASS2(n, 1) DEFINE_PASS2(n, 2)                   \
    DEFINE_PASS2(n, 3) DEFINE_PASS2(n, 4)
DEFINE_PASS2_ROW(0)
DEFINE_PASS2_ROW(1)
DEFINE_PASS2_ROW(2)
DEFINE_PASS2_ROW(3)
DEFINE_PASS2_ROW(4)
#define PASS2_ROW(n) {pass2_##n##_0, pass2_##n##_1, pass2_##n##_2, pass2_##n##_3, pass2_##n##_4}
static const Pass2 pass2[5][5] = {
    PASS2_ROW(0), PASS2_ROW(1), PASS2_ROW(2), PASS2_ROW(3), PASS2_ROW(4)};

// For a matrix that mirrors: both passes as runs of multiply-accumulates
// over the columns that hold coefficients (pass 1) or up to the last one
// (pass 2), even ones first.
static void inverse_dct_mirrored(const Block* in, int out[64]) {
    unsigned mask = in->mask;
    if (!mask) {
        memset(out, 0, 64 * sizeof(*out));
        return;
    }
    unsigned even = prefix_counts[mask] & 15, odd = prefix_counts[mask] >> 4;
    short work[8][8]; // [y][slot of column]: evens, then odds
    int odd_sum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (unsigned c = 0; c < 8; ++c) {
        unsigned slot = c & 1 ? even + c / 2 : c / 2;
        if (slot >= (c & 1 ? even + odd : even))
            continue;
        if (!(mask & (1u << c))) {
            for (unsigned y = 0; y < 8; ++y)
                work[y][slot] = 0;
            continue;
        }
        const short* v = in->column[c];
        int odd_rows = v[4] | v[5] | v[6] | v[7];
        if (!(odd_rows | v[1] | v[2] | v[3]) && row0_constant) {
            // only row 0: the same value at every x
            short value = (short)ROUND13(v[0] * idct_matrix[0]);
            for (unsigned y = 0; y < 8; ++y)
                work[y][slot] = value;
        } else {
            const short* b = pass1_matrix[0];
            if (odd_rows) {
                int s = v[4] + v[5] + v[6] + v[7];
                for (unsigned x = 0; x < 4; ++x) {
                    const short* a = v;
                    int e, f;
                    DOT_RUNS(4, 4, a, b, e, f);
                    work[x][slot] = (short)ROUND13(f);
                    work[7 - x][slot] = (short)ROUND13(2 * e - f - s);
                }
            } else {
                // even rows only: F = E and S = 0
                for (unsigned x = 0; x < 4; ++x, b += 4) {
                    const short* a = v;
                    int e, f;
                    DOT_RUNS(4, 0, a, b, e, f);
                    (void)f;
                    short value = (short)ROUND13(e);
                    work[x][slot] = value;
                    work[7 - x][slot] = value;
                }
            }
        }
        if (c & 1)
            for (unsigned y = 0; y < 8; ++y)
                odd_sum[y] += work[y][slot];
    }
    if (mask == 1 && row0_constant) {
        // only column 0: every output row is constant
        int m = idct_matrix[0];
        for (unsigned y = 0; y < 8; ++y) {
            int value = ROUND13_WRAP(work[y][0] * m);
            int* o = out + y * 8;
            o[0] = o[1] = o[2] = o[3] = o[4] = o[5] = o[6] = o[7] = value;
        }
        return;
    }
    pass2[even][odd](work, odd_sum, out);
}

static void inverse_dct(const Block* in, int out[64]) {
    if (idct_mirrored)
        inverse_dct_mirrored(in, out);
    else
        inverse_dct_generic(in, out);
}

// Where each coefficient goes in a Block (column * 8 + slot), by its index
// in the stream: zigzag order (scan_slot) or, unscaled, row-major order.
static unsigned char scan_slot[64], plain_slot[64];

static void set_block_tables(void) {
    for (unsigned index = 0; index < 64; ++index) {
        plain_slot[index] = (unsigned char)((index & 7) * 8 + row_slot[index >> 3]);
        scan_slot[index] = (unsigned char)((scan[index] & 7) * 8 + row_slot[scan[index] >> 3]);
    }
}

// The stream's 16-bit words (aligned, little-endian)
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
typedef u16 __attribute__((may_alias)) u16_alias;
#define STREAM16(p) (*(const u16_alias*)(p))
#else
#define STREAM16(p) read16(p)
#endif

// quant[i] * scale for the luminance and chroma tables; a movie frame keeps
// one scale for all its blocks. Reset at each decode (scale 0 never uses it).
typedef struct {
    unsigned scale;
    int table[64];
} QuantScale;
static QuantScale quant_scales[2];

/* 1 = block, 0 = padding/end of input, -1 = incomplete block. */
static int decode_block(
    const u8** source, const u8* end, int block[64], const u8 quant[64]) {
    const u8* p = *source;
    unsigned code;
    Block b;
    do {
        if (p == end) {
            *source = p;
            return 0;
        }
        code = STREAM16(p);
        p += 2;
    } while (code == 0xfe00);
    unsigned scale = code >> 10, index = 0, mask = 0;
    const unsigned char* slots = scale ? scan_slot : plain_slot;
    QuantScale* q = &quant_scales[quant == mdec.env.iq_c];
    if (scale != q->scale) {
        for (unsigned i = 0; i < 64; ++i)
            q->table[i] = quant[i] * (int)scale;
        q->scale = scale;
    }
    int value = sign_bits(code, 10) * quant[0];
    for (;;) {
        if (!scale)
            value = sign_bits(code, 10) * 2;
        value = clamp(value, -1024, 1023);
        if (value) {
            // index only grows, so each position is written once
            unsigned at = slots[index], c = at >> 3;
            if (!(mask & (1u << c))) {
                mask |= 1u << c;
                memset(b.column[c], 0, sizeof(b.column[c]));
            }
            b.column[0][at] = (short)value;
        }
        if (index == 63)
            break;
        if (p == end) {
            *source = p;
            return -1;
        }
        code = STREAM16(p);
        p += 2;
        index += (code >> 10) + 1;
        if (index >= 64)
            break;
        // fits 32 bits: 512 * 255 * 63 < 2^23
        value = (sign_bits(code, 10) * q->table[index] + 4) >> 3;
    }
    b.mask = mask;
    inverse_dct(&b, block);
    *source = p;
    return 1;
}

static int reserve(size_t bytes) {
    if (bytes <= mdec.capacity - mdec.length)
        return 1;
    size_t capacity = mdec.capacity ? mdec.capacity * 2 : 4096;
    if (capacity < mdec.length + bytes)
        capacity = mdec.length + bytes;
    void* allocation = realloc(mdec.output, capacity);
    if (!allocation)
        return 0;
    mdec.output = allocation;
    mdec.capacity = capacity;
    return 1;
}

// Luminance (9 bits) plus a chroma term (see chroma_offsets) spans
// -710..707. These tables turn that sum into an output component:
// 24-bit: (u8)clamp(sum, -128, 127), with bit 7 flipped for unsigned output;
// 15-bit: the unsigned 8-bit value rounded to 5 bits, with bit 4 flipped back
// for signed output.
#define CLIP_BIAS 720
static u8 clip_tables[2][2][CLIP_BIAS * 2];
static unsigned char clip_ready[2][2];

static const u8* clip_table(unsigned five, unsigned flip) {
    u8* table = clip_tables[five][flip];
    if (!clip_ready[five][flip]) {
        for (int i = 0; i < CLIP_BIAS * 2; ++i) {
            unsigned v = (u8)clamp(i - CLIP_BIAS, -128, 127);
            if (five)
                table[i] = (u8)((unsigned)clamp((int)((v ^ 128) + 4) >> 3, 0, 31) ^
                                (flip ? 0 : 16));
            else
                table[i] = (u8)(v ^ (flip ? 128 : 0));
        }
        clip_ready[five][flip] = 1;
    }
    return table + CLIP_BIAS;
}

// The chroma terms of each 2x2 group of pixels.
static void chroma_offsets(
    const int cr[64], const int cb[64], int red[64], int green[64], int blue[64]) {
    for (unsigned i = 0; i < 64; ++i) {
        red[i] = (359 * cr[i] + 128) >> 8;
        green[i] = (-88 * cb[i] - 183 * cr[i] + 128) >> 8;
        blue[i] = (454 * cb[i] + 128) >> 8;
    }
}

// The chroma terms by chroma value (9 bits, biased by 256):
// red (359 * cr + 128) >> 8 and blue (454 * cb + 128) >> 8 as pointers into
// the clip table of the current flip, and green in two parts whose sum,
// shifted right by 8, is (-88 * cb - 183 * cr + 128) >> 8 plus 512 (the
// bias keeps the sum positive, so a logical shift floors it).
static const u8* chroma_red[2][512];
static const u8* chroma_blue[2][512];
static int chroma_green_cb[512], chroma_green_cr[512];

static void set_chroma_tables(void) {
    if (chroma_red[0][0])
        return;
    for (unsigned flip = 0; flip < 2; ++flip) {
        const u8* clip = clip_table(0, flip);
        for (int v = -256; v < 256; ++v) {
            chroma_red[flip][v + 256] = clip + ((359 * v + 128) >> 8);
            chroma_blue[flip][v + 256] = clip + ((454 * v + 128) >> 8);
        }
    }
    for (int v = -256; v < 256; ++v) {
        chroma_green_cb[v + 256] = -88 * v;
        chroma_green_cr[v + 256] = -183 * v + 128 + (512 << 8);
    }
}

// Each chroma sample's clip table pointers for 24-bit output, as
// {red, green, blue}.
static void chroma_pointers(unsigned flip, const int cr[64], const int cb[64],
                            const u8* rgb[64][3]) {
    const u8* const* red = chroma_red[flip] + 256;
    const u8* const* blue = chroma_blue[flip] + 256;
    const int* green_cb = chroma_green_cb + 256;
    const int* green_cr = chroma_green_cr + 256;
    const u8* green_base = clip_table(0, flip) - 512;
#if defined(__sh__)
    const u8** o = rgb[0];
    unsigned n = 64;
    int v, g, b;
    __asm__("1:\n\t"
            "mov.l @%[cr]+,%[v]\n\t"
            "mov.l @%[cb]+,r0\n\t"
            "shll2 r0\n\t"
            "mov.l @(r0,%[bt]),%[b]\n\t"
            "mov.l @(r0,%[gb]),%[g]\n\t"
            "mov %[v],r0\n\t"
            "shll2 r0\n\t"
            "mov.l @(r0,%[rt]),%[v]\n\t"
            "mov.l @(r0,%[gr]),r0\n\t"
            "add r0,%[g]\n\t"
            "shlr8 %[g]\n\t"
            "add %[base],%[g]\n\t"
            "mov.l %[v],@%[o]\n\t"
            "mov.l %[g],@(4,%[o])\n\t"
            "mov.l %[b],@(8,%[o])\n\t"
            "dt %[n]\n\t"
            "bf.s 1b\n\t"
            "add #12,%[o]"
            : [cr] "+r"(cr), [cb] "+r"(cb), [o] "+r"(o), [n] "+r"(n), [v] "=&r"(v),
              [g] "=&r"(g), [b] "=&r"(b), "=m"(*(const u8* (*)[64][3])rgb)
            : [rt] "r"(red), [bt] "r"(blue), [gb] "r"(green_cb), [gr] "r"(green_cr),
              [base] "r"(green_base), "m"(*(const int(*)[64])cr), "m"(*(const int(*)[64])cb)
            : "r0", "t");
#else
    for (unsigned i = 0; i < 64; ++i) {
        rgb[i][0] = red[cr[i]];
        rgb[i][1] = green_base + ((unsigned)(green_cb[cb[i]] + green_cr[cr[i]]) >> 8);
        rgb[i][2] = blue[cb[i]];
    }
#endif
}

// Eight 24-bit pixels: luminance[0..7], each pair of pixels sharing one
// chroma sample's pointers (see chroma_pointers).
#if defined(__sh__)
// Stored backwards from the end with pre-decrement, which takes any register
// (a byte store with displacement only takes r0, the table index here).
#define RGB24_PIXEL(l)                                                         \
    "mov.l @(" #l ",%[l]),r0\n\t"                                             \
    "mov.b @(r0,%[B]),%[t]\n\t"                                               \
    "mov.b %[t],@-%[o]\n\t"                                                   \
    "mov.b @(r0,%[G]),%[t]\n\t"                                               \
    "mov.b %[t],@-%[o]\n\t"                                                   \
    "mov.b @(r0,%[R]),%[t]\n\t"                                               \
    "mov.b %[t],@-%[o]\n\t"
#define RGB24_PAIR(c, l1, l0)                                                  \
    "mov.l @(" #c ",%[c]),%[R]\n\t"                                           \
    "mov.l @(" #c "+4,%[c]),%[G]\n\t"                                         \
    "mov.l @(" #c "+8,%[c]),%[B]\n\t" RGB24_PIXEL(l1) RGB24_PIXEL(l0)
static inline void rgb24_eight(u8* o, const int* luminance, const u8* const (*rgb)[3]) {
    u8* end = o + 24;
    const u8 *r, *g, *b;
    int t;
    __asm__(RGB24_PAIR(36, 28, 24) RGB24_PAIR(24, 20, 16) RGB24_PAIR(12, 12, 8)
                RGB24_PAIR(0, 4, 0)
            : [o] "+r"(end), [t] "=&r"(t), [R] "=&r"(r), [G] "=&r"(g), [B] "=&r"(b),
              "=m"(*(u8(*)[24])o)
            : [l] "r"(luminance), [c] "r"(rgb), "m"(*(const int(*)[8])luminance),
              "m"(*(const u8* const(*)[4][3])rgb)
            : "r0");
}
#else
static inline void rgb24_eight(u8* o, const int* luminance, const u8* const (*rgb)[3]) {
    for (unsigned x = 0; x < 8; ++x, o += 3) {
        int l = luminance[x];
        o[0] = rgb[x / 2][0][l];
        o[1] = rgb[x / 2][1][l];
        o[2] = rgb[x / 2][2][l];
    }
}
#endif

static void decode(const u8* source, const u8* end) {
    unsigned depth = (mdec.command >> 27) & 3;
    unsigned flip_bit = !(mdec.command & (1u << 26));
    int blocks[6][64];
    for (unsigned i = 0; i < 64; ++i)
        idct_matrix16[i] = (short)(idct_matrix[i] = mdec.env.dct[i] >> 3);
    set_chroma_tables();
    quant_scales[0].scale = quant_scales[1].scale = 0;
    if (!plain_slot[1])
        set_block_tables();
    idct_mirrored = matrix_mirrors();
    if (idct_mirrored)
        set_mirrored_tables();
    for (;;) {
        int result = decode_block(
            &source, end, blocks[0], depth < 2 ? mdec.env.iq_y : mdec.env.iq_c);
        if (result == 0)
            break;
        if (result < 0) {
            mdec.error = -1;
            break;
        }
        if (depth >= 2) {
            for (unsigned b = 1; b < 6; ++b) {
                if (decode_block(&source, end, blocks[b],
                                 b == 1 ? mdec.env.iq_c : mdec.env.iq_y) != 1) {
                    mdec.error = -1;
                    return;
                }
            }
        }
        unsigned bytes = depth == 0   ? 32
                         : depth == 1 ? 64
                         : depth == 2 ? 768
                                      : 512;
        if (!reserve(bytes)) {
            mdec.error = -1;
            return;
        }
        u8* out = mdec.output + mdec.length;
        if (depth < 2) {
            for (unsigned i = 0; i < 64; ++i) {
                unsigned v = (u8)clamp(blocks[0][i], -128, 127);
                if (!(mdec.command & (1u << 26)))
                    v ^= 128;
                if (depth)
                    out[i] = (u8)v;
                else if (!(i & 1))
                    out[i / 2] = (u8)(v >> 4);
                else
                    out[i / 2] |= (u8)(v & 0xf0);
            }
        } else if (depth == 2) {
            const u8* rgb[64][3];
            chroma_pointers(flip_bit, blocks[0], blocks[1], rgb);
            for (unsigned y = 0; y < 16; ++y) {
                const int* row = blocks[2 + (y >> 3) * 2] + (y & 7) * 8;
                const u8* const (*c)[3] = rgb + (y >> 1) * 8;
                u8* o = out + y * 48;
                rgb24_eight(o, row, c);
                rgb24_eight(o + 24, row + 64, c + 4);
            }
        } else {
            const u8* clip = clip_table(1, flip_bit);
            unsigned stp = (mdec.command >> 10) & 0x8000;
            int red[64], green[64], blue[64];
            chroma_offsets(blocks[0], blocks[1], red, green, blue);
            for (unsigned y = 0; y < 16; ++y) {
                const int* row = blocks[2 + (y >> 3) * 2] + (y & 7) * 8;
                unsigned c = (y >> 1) * 8;
                u8* o = out + y * 32;
                for (unsigned half = 0; half < 2; ++half, row += 64)
                    for (unsigned x = 0; x < 8; ++x, o += 2) {
                        unsigned k = c + half * 4 + x / 2;
                        int l = row[x];
                        unsigned packed = clip[l + red[k]] | clip[l + green[k]] << 5 |
                                          clip[l + blue[k]] << 10 | stp;
                        o[0] = (u8)packed;
                        o[1] = (u8)(packed >> 8);
                    }
            }
        }
        mdec.length += bytes;
    }
}

void MDEC_reset(int mode) {
    if (mode != 0 && mode != 1) {
        mdec.error = -1;
        return;
    }
    mdec.length = mdec.cursor = 0;
    mdec.command = 0;
    mdec.error = 0;
    if (!mode)
        DecDCTGetEnv(&mdec.env);
}

void MDEC_in(void* buf, int size) {
    mdec.error = 0;
    if (!buf || size < 0) {
        mdec.error = -1;
        return;
    }
    const u8* data = (const u8*)buf;
    mdec.command = read32(data);
    data += 4;
    switch (mdec.command >> 29) {
    case 1: {
        mdec.length = mdec.cursor = 0;
        unsigned words = mdec.command & 0xffff;
        if ((unsigned)size < words) {
            mdec.error = -1;
            break;
        }
        decode(data, data + words * 4);
        break;
    }
    case 2: {
        unsigned bytes = (mdec.command & 1) ? 128 : 64;
        if ((unsigned)size < bytes / 4) {
            mdec.error = -1;
            break;
        }
        memcpy(mdec.env.iq_y, data, 64);
        if (bytes == 128)
            memcpy(mdec.env.iq_c, data + 64, 64);
        break;
    }
    case 3:
        if (size < 32) {
            mdec.error = -1;
            break;
        }
        for (unsigned i = 0; i < 64; ++i)
            mdec.env.dct[i] = (s16)read16(data + i * 2);
        break;
    default:
        break;
    }
    if (!mdec.error)
        Psyz_DmaComplete(0);
}

void MDEC_out(void* buf, int size) {
    if (!size)
        return;
    if (!buf || size < 0 || mdec.error ||
        (size_t)size > (mdec.length - mdec.cursor) / 4) {
        mdec.error = -1;
        return;
    }
    memcpy(buf, mdec.output + mdec.cursor, (size_t)size * 4);
    mdec.cursor += (size_t)size * 4;
    Psyz_DmaComplete(1);
}
int MDEC_in_sync(void) { return mdec.error; }
int MDEC_out_sync(void) { return mdec.error; }
u_long MDEC_status(void) {
    u32 status = 0x0004ffffu | ((mdec.command >> 2) & 0x07800000u);
    return status | (mdec.cursor == mdec.length ? 0x80000000u : 0x28000000u);
}

int DecDCTvlc(u_long* bs, u_long* buf) {
    NOT_IMPLEMENTED;
    return -1;
}
