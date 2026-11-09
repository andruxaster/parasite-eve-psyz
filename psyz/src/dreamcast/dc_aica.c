/* AICA channels for dc_spu.c, written from the SH-4 over the G2 bus. KOS's
 * sound driver keeps running on the ARM for the CD stream (dc_audio.c); the
 * SPU's channels come from its allocator, so the two never share one.
 * Register layout as in KOS's own driver (kernel/.../sound/arm/aica.c). */
#include <psyz.h>
#include <psyz/log.h>
#include <dc/g2bus.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/spu.h>
#include "dc_aica.h"

#define CHANNEL_REG(ch, offset) (0xa0700000 + 0x80 * (ch) + (offset))
#define STREAM_RESERVE (96 * 1024) // the CD stream's buffers and spare
#define MAX_VOICES 32

static int channel[MAX_VOICES];
static uint32_t pcm_base; // sound RAM offset of the PCM region
static size_t pcm_bytes;

size_t DcAica_Init(unsigned voices) {
    static int done;
    if (done)
        return pcm_bytes;
    done = 1;
    snd_stream_init_ex(2, 16 * 1024);
    for (unsigned v = 0; v < voices && v < MAX_VOICES; v++) {
        channel[v] = snd_sfx_chn_alloc();
        if (channel[v] < 0) {
            ERRORF("no AICA channel for SPU voice %u", v);
            return 0;
        }
    }
    size_t available = snd_mem_available();
    size_t want = (size_t)PSYZ_SPU_RAM_SIZE / 16 * 56;
    if (available < STREAM_RESERVE + 4096)
        return 0;
    if (want > available - STREAM_RESERVE)
        want = (available - STREAM_RESERVE) & ~(size_t)31;
    pcm_base = snd_mem_malloc(want);
    if (!pcm_base) {
        ERRORF("no sound RAM for SPU samples");
        return 0;
    }
    spu_memset(pcm_base, 0, want);
    pcm_bytes = want;
    return pcm_bytes;
}

void DcAica_Upload(size_t offset, const void* pcm, size_t bytes) {
    if (offset + bytes <= pcm_bytes)
        spu_memload(pcm_base + offset, pcm, bytes);
}

static void write_level(int ch, DcAicaLevel level) {
    g2_write_32(CHANNEL_REG(ch, 36), 0x0F00 | level.pan); // DISDL 15, DIPAN
    g2_write_32(CHANNEL_REG(ch, 40), ((uint32_t)level.tl << 8) | 0x24);
}

void DcAica_Start(unsigned voice, uint32_t start, uint32_t length,
                  uint32_t loop_start, int loop, unsigned pitch,
                  DcAicaLevel level) {
    int ch = channel[voice];
    uint32_t address = pcm_base + start * 2;
    g2_write_32(CHANNEL_REG(ch, 0), 0x8000); // key off
    g2_write_32(CHANNEL_REG(ch, 4), address & 0xFFFF);
    g2_write_32(CHANNEL_REG(ch, 8), loop_start & 0xFFFF);
    g2_write_32(CHANNEL_REG(ch, 12), length & 0xFFFF);
    g2_write_32(CHANNEL_REG(ch, 16), 0x1F); // attack at once, no decay
    g2_write_32(CHANNEL_REG(ch, 20), 0x1F); // release at once
    g2_write_32(CHANNEL_REG(ch, 24), DcAica_PitchRegister(pitch));
    g2_write_32(CHANNEL_REG(ch, 28), 0); // no LFO
    g2_write_32(CHANNEL_REG(ch, 32), 0); // no DSP send
    write_level(ch, level);
    // key on, 16-bit PCM
    g2_write_32(CHANNEL_REG(ch, 0),
                0xC000 | (loop ? 0x200 : 0) | ((address >> 16) & 0x7F));
}

void DcAica_SetPitch(unsigned voice, unsigned pitch) {
    g2_write_32(CHANNEL_REG(channel[voice], 24), DcAica_PitchRegister(pitch));
}

void DcAica_SetLevel(unsigned voice, DcAicaLevel level) {
    write_level(channel[voice], level);
}

void DcAica_Stop(unsigned voice) {
    g2_write_32(CHANNEL_REG(channel[voice], 0), 0x8000);
}
