/* Feeds 20000 random MDEC streams to libpress.c's decode() and checks a
 * checksum of all output against the one the original decoder (the plain
 * two-pass matrix product, PsyZ before the Dreamcast work) gives. Covers
 * the standard and random matrices, all four output depths, padding,
 * unscaled blocks and extreme coefficients.
 *
 * Host: sh mdec_check.sh. SH-4 (the mac.w paths): build with sh-elf-gcc
 * -DFREESTANDING and run under qemu-sh4; qemu up to at least 8.2 loads
 * mac.w operands as 32-bit aligned words and faults, so it needs MO_TESW in
 * target/sh4/translate.c (case 0x400f). */
#define TRIALS 20000
#define EXPECTED 0xe9ab256fu
#include "../../psyz/libpress.c"
/* the matrix DecDCTGetEnv gives, which the game keeps */
static const short std_dct[64] = {23170,23170,23170,23170,23170,23170,23170,23170,32138,27245,18204,6392,-6393,-18205,-27246,-32139,30273,12539,-12540,-30274,-30274,-12540,12539,30273,27245,-6393,-32139,-18205,18204,32138,6392,-27246,23170,-23171,-23171,23170,23170,-23171,-23171,23170,18204,-32139,6392,27245,-27246,-6393,32138,-18205,12539,-30274,30273,-12540,-12540,30273,-30274,12539,6392,-18205,27245,-32139,32138,-27246,18204,-6393};
void psyz_log(unsigned int level, const char* file, unsigned int line, const char* func, const char* fmt, ...) {}
DECDCTENV* DecDCTGetEnv(DECDCTENV* env) { return env; }
void Psyz_DmaComplete(unsigned ch) { (void)ch; }
static unsigned char outbuf[1 << 20];
static unsigned short stream[1 << 15] __attribute__((aligned(4)));
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
#ifdef FREESTANDING
static long sys(long nr, long a, long b, long c) {
    register long r3 __asm__("r3") = nr, r4 __asm__("r4") = a, r5 __asm__("r5") = b, r6 __asm__("r6") = c;
    __asm__ volatile("trapa #0x13" : "+r"(r3) : "r"(r4), "r"(r5), "r"(r6) : "memory");
    return r3;
}
void* realloc(void* p, size_t n) { (void)p; (void)n; return 0; }
static void put(const char* s) { size_t n = 0; while (s[n]) n++; sys(4, 1, (long)s, (long)n); }
static void puthex(unsigned v) { char b[12]; for (int i = 0; i < 8; i++) b[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; b[8] = '\n'; b[9] = 0; put(b); }
void start(void) {
#else
#include <stdio.h>
static void puthex(unsigned v) { printf("%08x %s\n", v, v == EXPECTED ? "ok" : "MISMATCH"); }
int main(void) {
#endif
    unsigned sum = 0;
    for (int trial = 0; trial < TRIALS; ++trial) {
        int kind = trial % 4; /* 0,1: standard matrix; 2: random; 3: small random */
        for (int i = 0; i < 64; ++i) {
            mdec.env.iq_y[i] = (u8)(trial % 7 == 0 ? 255 : rnd() & 0xFF);
            mdec.env.iq_c[i] = (u8)(rnd() & 0xFF);
            mdec.env.dct[i] = kind < 2 ? std_dct[i]
                            : kind == 2 ? (s16)((int)(rnd() & 0xFFFF) - 32768)
                                        : (s16)((int)(rnd() % 2000) - 1000);
        }
        unsigned depth = trial % 9 == 0 ? (rnd() & 1) : 2 + (rnd() & 1);
        mdec.command = (1u << 29) | (depth << 27) | ((rnd() & 1) << 26) | ((rnd() & 1) << 25);
        size_t n = 0;
        int blocks = (depth < 2 ? 1 : 6) * (1 + rnd() % 4);
        for (int b = 0; b < blocks; ++b) {
            if (rnd() % 8 == 0) stream[n++] = 0xfe00; /* padding */
            unsigned scale = trial % 11 == 0 ? 0 : (trial % 13 == 0 ? 63 : rnd() % 64);
            unsigned dc = trial % 5 == 0 ? (rnd() & 1 ? 0x1ff : 0x200) : rnd() & 0x3FF;
            stream[n++] = (unsigned short)((scale << 10) | dc);
            int coeffs = rnd() % (trial % 6 ? 8 : 64);
            for (int c = 0; c < coeffs; ++c) {
                unsigned run = trial % 3 ? rnd() % 4 : rnd() % 16;
                unsigned ac = trial % 5 == 1 ? (rnd() & 1 ? 0x1ff : 0x200) : rnd() & 0x3FF;
                unsigned code = (run << 10) | ac;
                if (code == 0xfe00) code ^= 1;
                stream[n++] = (unsigned short)code;
            }
            stream[n++] = 0xfe00;
        }
        if (n & 1) stream[n++] = 0xfe00;
        mdec.output = outbuf;
        mdec.capacity = sizeof(outbuf);
        mdec.length = mdec.cursor = 0;
        mdec.error = 0;
        decode((const u8*)stream, (const u8*)(stream + n));
        sum = sum * 33 + (unsigned)mdec.length + (unsigned)mdec.error;
        for (size_t i = 0; i < mdec.length; ++i) sum = sum * 31 + mdec.output[i];
    }
    puthex(sum);
#ifndef FREESTANDING
    return sum == EXPECTED ? 0 : 1;
#endif
#ifdef FREESTANDING
    sys(1, 0, 0, 0);
    for (;;);
#endif
}
