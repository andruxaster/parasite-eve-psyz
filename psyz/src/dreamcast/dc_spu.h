/* Calls between the Dreamcast SPU (dc_spu.c) and the audio backend
 * (dc_audio.c, or the PC harness). */
#ifndef PSYZ_DC_SPU_H
#define PSYZ_DC_SPU_H

/* Advances the voices by frames 44.1 kHz ticks: key-ons and key-offs,
 * envelopes, sample ends, and the channel levels and pitches. */
void DcSpu_Update(unsigned frames);

/* Fills out with frames of CD/XA audio (stereo) at the CD and main volumes,
 * silence where libcd has none. */
void DcSpu_MixCd(short* out, unsigned frames);

#endif
