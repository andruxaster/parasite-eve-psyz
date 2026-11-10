#ifndef PSYZ_HOT_H
#define PSYZ_HOT_H

/**
 * @file hot.h
 * @brief Where the hottest code of a frame goes on the Dreamcast.
 *
 * The SH-4's instruction cache is 8 KB and direct-mapped: code 8 KB apart
 * shares cache lines, and two hot functions that happen to overlap evict
 * each other on every call. KOS's linker script has sections .sub0 to .sub9,
 * each aligned to 8 KB and checked to fit in it. Code in one of them never
 * evicts other code of the same section.
 *
 * A frame runs in two phases that do not interleave: the game transforms
 * its models (GTE) and fills the ordering table, then DrawOTag walks it and
 * draws (GPU queue, renderer). Each phase's hot functions share a section.
 * Elsewhere the macros expand to nothing.
 */
#if defined(__sh__) && defined(_arch_dreamcast)
#define PSYZ_HOT_DRAW __attribute__((section(".sub0.psyz_draw")))
#define PSYZ_HOT_GEOM __attribute__((section(".sub1.psyz_geom")))
#else
#define PSYZ_HOT_DRAW
#define PSYZ_HOT_GEOM
#endif

#endif
