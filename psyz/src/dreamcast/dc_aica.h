/* The AICA side of the Dreamcast SPU (dc_spu.c): one AICA channel per PS1
 * voice, playing 16-bit PCM that dc_spu.c decodes into a region of sound
 * RAM. dc_aica.c drives the real chip; the PC harness has a software model.
 */
#ifndef PSYZ_DC_AICA_H
#define PSYZ_DC_AICA_H

#include <stddef.h>
#include <stdint.h>

/* Allocates the channels and the PCM region; returns the region's size in
 * bytes (0 if there is no sound RAM). */
size_t DcAica_Init(unsigned voices);

/* Copies bytes (a multiple of 4) to offset bytes into the PCM region. */
void DcAica_Upload(size_t offset, const void* pcm, size_t bytes);

/* A channel's level: tl is the AICA total level (attenuation in 0.375 dB
 * steps, 255 near silence) and pan the AICA DIPAN value (bit 4 set lowers
 * the right side, clear the left, by 3 dB per step of bits 0-3). */
typedef struct {
    uint8_t tl, pan;
} DcAicaLevel;

/* Starts voice at start (in samples into the PCM region) for length
 * samples, looping from loop_start (samples from start) when loop is set.
 * pitch is the PS1 pitch register (0x1000 = 44.1 kHz). */
void DcAica_Start(unsigned voice, uint32_t start, uint32_t length,
                  uint32_t loop_start, int loop, unsigned pitch,
                  DcAicaLevel level);
void DcAica_SetPitch(unsigned voice, unsigned pitch);
void DcAica_SetLevel(unsigned voice, DcAicaLevel level);
void DcAica_Stop(unsigned voice);

/* AICA OCT/FNS register value for a PS1 pitch (0 for pitch 0). */
static inline unsigned DcAica_PitchRegister(unsigned pitch) {
    if (!pitch)
        return 0;
    int octave = 0;
    while (pitch < 0x1000) {
        pitch <<= 1;
        octave--;
    }
    while (pitch >= 0x2000) {
        pitch >>= 1;
        octave++;
    }
    return ((unsigned)(octave & 15) << 11) | ((pitch - 0x1000) >> 2);
}

#endif
