/* Subpixel screen positions and depths of GTE-projected vertices, for
 * renderers that draw above the PS1's resolution without PGXP. The GTE
 * computes screen X/Y with 16 fractional bits and drops them; with
 * PSYZ_GTE_SUBPIXEL defined, RTPS/RTPT keep them (8 bits) and SZ in a small
 * cache keyed by the integer screen position they produced. A renderer looks
 * up a primitive's vertices by their packet coordinates: when all of them
 * are found it can place them between pixels and correct texture
 * perspective with SZ. A position two different vertices projected to in
 * the same frame is ambiguous and not found. */
#ifndef PSYZ_SUBPIXEL_H
#define PSYZ_SUBPIXEL_H

typedef struct {
    float x, y;    /* fractional offset to add to the integer position */
    unsigned z;    /* SZ: view depth, 1..65535 */
} PsyzSubpixel;

/* 1 and *out set when (x, y) came from one projection in the last three
 * frames (vertical blanks waited). */
int Psyz_SubpixelLookup(int x, int y, PsyzSubpixel* out);

/* The same for a vertex whose packed XY is the word at packet_word: exact
 * when the GTE stored it there (gte_stsxy), else by position. */
int Psyz_SubpixelLookupAt(const void* packet_word, int x, int y,
                          PsyzSubpixel* out);

/* Starts a frame (a vertical blank waited). */
void Psyz_SubpixelNextFrame(void);

#endif
