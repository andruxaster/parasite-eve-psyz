/* PS1 SPU on the Dreamcast AICA.
 *
 * Mixing the SPU in software (psyz/psyz_spu.c) costs the SH-4 160 to 600
 * million instructions per second of sound, more than it has. As on the PSP
 * (psp/psp_sas.c), the voices play on the sound chip instead, and the CPU
 * keeps what the game can read back:
 *
 * - SPU RAM stays in main RAM. Written blocks of PS1 ADPCM are decoded to
 *   16-bit PCM in AICA sound RAM, 56 bytes per 16-byte block at the same
 *   block index, so a voice's start address maps directly.
 * - Each PS1 voice owns an AICA channel. At key-on the sample is scanned to
 *   its end block; a repeat flag makes the channel loop from the last loop
 *   start block (or the loop address register).
 * - Envelopes run on the CPU with the SPU's own ADSR code, advanced in bulk
 *   every update (about 5 ms), and set the channel level with the voice and
 *   main volumes. ENVX reads back from them. A sample without a repeat ends
 *   when the voice's tracked position passes its length.
 * - CD and XA audio arrive as 44.1 kHz frames from libcd and go out on a
 *   separate stream (dc_audio.c) through DcSpu_MixCd.
 *
 * Not done: reverb, noise, pitch modulation, the capture buffers, and PS1
 * ADPCM history across a loop jump (blocks are decoded in address order).
 */
#include <psyz.h>
#include <psyz/log.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../psyz/spu_voice.h"
#include "dc_aica.h"
#include "dc_spu.h"

#define BLOCKS (PSYZ_SPU_RAM_SIZE / ADPCM_BLOCK_BYTES)
#define PCM_BLOCK_BYTES (ADPCM_BLOCK_SAMPLES * 2)
#define MAX_RUN_BLOCKS (65535 / ADPCM_BLOCK_SAMPLES) // AICA loop end is 16 bits
#define DECODE_CHUNK 64

typedef struct {
    u8 on;           // the AICA channel is playing
    u8 loop;         // the sample repeats
    u16 pitch;       // last pitch sent
    DcAicaLevel level;
    uint32_t length; // samples
    uint64_t pos;    // samples played, 12 fractional bits
} HwVoice;

static struct {
    u8 ram[PSYZ_SPU_RAM_SIZE];
    unsigned transfer_addr;
    u16 capture_pos;
    VoiceState voice[PSYZ_SPU_NUM_VOICES];
    HwVoice hw[PSYZ_SPU_NUM_VOICES];
    unsigned pcm_blocks;            // blocks the PCM region holds
    unsigned dirty_lo, dirty_hi;    // blocks written since the last decode
    unsigned decoded_end;           // block after the last one decoded...
    short decoded_hist1, decoded_hist2; // ...and the ADPCM history there
    u8 initialized;
} spu;

static u8 tl_table[4096];  // linear gain >> 3 -> AICA total level
static u8 pan_table[257];  // quieter/louder side * 256 -> 3 dB steps

u8* Psyz_SpuGetRam(void) { return spu.ram; }

static void build_tables(void) {
    tl_table[0] = 255;
    for (int i = 1; i < 4096; ++i) {
        float db = -20.0f * log10f((i + 0.5f) / 4096.0f);
        int tl = (int)(db / 0.375f + 0.5f);
        tl_table[i] = (u8)(tl > 255 ? 255 : tl);
    }
    pan_table[0] = 15;
    for (int i = 1; i <= 256; ++i) {
        float db = -20.0f * log10f(i / 256.0f);
        int steps = (int)(db / 3.0f + 0.5f);
        pan_table[i] = (u8)(steps > 15 ? 15 : steps);
    }
}

static void mark_dirty(unsigned start, unsigned size) {
    if (!size)
        return;
    unsigned lo = start / ADPCM_BLOCK_BYTES;
    unsigned hi = (start + size + ADPCM_BLOCK_BYTES - 1) / ADPCM_BLOCK_BYTES;
    if (hi > BLOCKS)
        hi = BLOCKS;
    if (spu.dirty_lo >= spu.dirty_hi) {
        spu.dirty_lo = lo;
        spu.dirty_hi = hi;
    } else {
        if (lo < spu.dirty_lo)
            spu.dirty_lo = lo;
        if (hi > spu.dirty_hi)
            spu.dirty_hi = hi;
    }
}

// Decodes the written blocks to PCM in sound RAM. A run that continues
// where the previous one ended (a sample bank sent in pieces) keeps its
// ADPCM history; others start from silence, as samples do.
static void decode_dirty(void) {
    static short pcm[DECODE_CHUNK * ADPCM_BLOCK_SAMPLES];
    unsigned b = spu.dirty_lo, end = spu.dirty_hi;
    spu.dirty_lo = spu.dirty_hi = 0;
    if (end > spu.pcm_blocks)
        end = spu.pcm_blocks;
    if (b >= end)
        return;
    short h1 = 0, h2 = 0;
    if (b == spu.decoded_end) {
        h1 = spu.decoded_hist1;
        h2 = spu.decoded_hist2;
    }
    while (b < end) {
        unsigned n = end - b < DECODE_CHUNK ? end - b : DECODE_CHUNK;
        for (unsigned k = 0; k < n; ++k)
            spu_adpcm_decode_block(&spu.ram[(b + k) * ADPCM_BLOCK_BYTES], &h1,
                                   &h2, &pcm[k * ADPCM_BLOCK_SAMPLES], NULL);
        DcAica_Upload((size_t)b * PCM_BLOCK_BYTES, pcm, n * PCM_BLOCK_BYTES);
        b += n;
    }
    spu.decoded_end = end;
    spu.decoded_hist1 = h1;
    spu.decoded_hist2 = h2;
}

static void spu_reset(int hot) {
    for (unsigned v = 0; v < PSYZ_SPU_NUM_VOICES; v++) {
        _spu_RXX->rxx.voice[v].volumex = 0;
        if (spu.hw[v].on)
            DcAica_Stop(v);
    }
    _spu_RXX->rxx.spustat = 0;
    if (!hot) {
        memset(spu.ram, 0, sizeof(spu.ram));
        mark_dirty(0, PSYZ_SPU_RAM_SIZE);
    }
    spu.transfer_addr = 0;
    spu.capture_pos = 0;
    memset(spu.voice, 0, sizeof(spu.voice));
    memset(spu.hw, 0, sizeof(spu.hw));
}

void Psyz_SpuInit(void) {
    Psyz_AudioLock();
    if (!spu.initialized) {
        memset(&spu, 0, sizeof(spu));
        build_tables();
        size_t bytes = DcAica_Init(PSYZ_SPU_NUM_VOICES);
        spu.pcm_blocks = (unsigned)(bytes / PCM_BLOCK_BYTES);
        if (spu.pcm_blocks > BLOCKS)
            spu.pcm_blocks = BLOCKS;
        spu.initialized = 1;
        INFOF("SPU on the AICA: PCM for %u of %u KB of SPU RAM",
              spu.pcm_blocks * ADPCM_BLOCK_BYTES / 1024, PSYZ_SPU_RAM_SIZE / 1024);
    }
    Psyz_AudioUnlock();
}

void Psyz_SpuReset(int hot) {
    Psyz_AudioLock();
    spu_reset(hot);
    Psyz_AudioUnlock();
}

void Psyz_SpuSetTransferAddr(unsigned int addr) {
    Psyz_AudioLock();
    spu.transfer_addr = addr & (PSYZ_SPU_RAM_SIZE - 1);
    Psyz_AudioUnlock();
}

unsigned int Psyz_SpuGetTransferAddr(void) {
    Psyz_AudioLock();
    unsigned int address = spu.transfer_addr;
    Psyz_AudioUnlock();
    return address;
}

void Psyz_SpuFifoWrite(unsigned short word) {
    Psyz_AudioLock();
    spu.ram[spu.transfer_addr] = (unsigned char)(word & 0xFF);
    spu.ram[spu.transfer_addr + 1] = (unsigned char)(word >> 8);
    mark_dirty(spu.transfer_addr, 2);
    spu.transfer_addr = (spu.transfer_addr + 2) & (PSYZ_SPU_RAM_SIZE - 1);
    Psyz_AudioUnlock();
}

void Psyz_SpuFifoWriteBulk(const unsigned char* src, unsigned int size) {
    Psyz_AudioLock();
    Psyz_SpuMemWrite(spu.transfer_addr, src, size);
    spu.transfer_addr = (spu.transfer_addr + size) & (PSYZ_SPU_RAM_SIZE - 1);
    Psyz_AudioUnlock();
}

void Psyz_SpuMemRead(unsigned int offset, void* dst, unsigned int size) {
    Psyz_AudioLock();
    unsigned int start = offset & (PSYZ_SPU_RAM_SIZE - 1);
    unsigned int head = PSYZ_SPU_RAM_SIZE - start;
    if (size <= head) {
        memcpy(dst, &spu.ram[start], size);
    } else {
        memcpy(dst, &spu.ram[start], head);
        memcpy((unsigned char*)dst + head, &spu.ram[0], size - head);
    }
    Psyz_AudioUnlock();
}

void Psyz_SpuMemWrite(unsigned int offset, const void* src, unsigned int size) {
    Psyz_AudioLock();
    unsigned int start = offset & (PSYZ_SPU_RAM_SIZE - 1);
    unsigned int head = PSYZ_SPU_RAM_SIZE - start;
    if (size <= head) {
        memcpy(&spu.ram[start], src, size);
        mark_dirty(start, size);
    } else {
        memcpy(&spu.ram[start], src, head);
        memcpy(&spu.ram[0], (const unsigned char*)src + head, size - head);
        mark_dirty(start, head);
        mark_dirty(0, size - head);
    }
    Psyz_AudioUnlock();
}

void Psyz_SpuWrite(unsigned int reg_offset, unsigned short value) {
    if (reg_offset >= sizeof(SPU_RXX) || (reg_offset & 1)) {
        WARNF("Psyz_SpuWrite: bad offset 0x%X", reg_offset);
        return;
    }
    if (reg_offset == offsetof(SPU_RXX, spustat)) // read-only
        return;
    Psyz_AudioLock();
    _spu_RXX->raw[reg_offset >> 1] = value;
    if (reg_offset == offsetof(SPU_RXX, key_on[0])) {
        for (int v = 0; v < 16; v++)
            if (value & (1u << v))
                spu.voice[v].key_on_pending = 1;
    } else if (reg_offset == offsetof(SPU_RXX, key_on[1])) {
        for (int v = 0; v < 8; v++)
            if (value & (1u << v))
                spu.voice[16 + v].key_on_pending = 1;
    } else if (reg_offset == offsetof(SPU_RXX, key_off[0])) {
        for (int v = 0; v < 16; v++)
            if (value & (1u << v))
                spu.voice[v].key_off = 1;
    } else if (reg_offset == offsetof(SPU_RXX, key_off[1])) {
        for (int v = 0; v < 8; v++)
            if (value & (1u << v))
                spu.voice[16 + v].key_off = 1;
    } else if (reg_offset == offsetof(SPU_RXX, trans_addr)) {
        Psyz_SpuSetTransferAddr((unsigned)value << 3);
    } else if (reg_offset == offsetof(SPU_RXX, trans_fifo)) {
        Psyz_SpuFifoWrite(value);
    }
    Psyz_AudioUnlock();
}

unsigned short Psyz_SpuRead(unsigned int reg_offset) {
    if (reg_offset >= sizeof(SPU_RXX) || (reg_offset & 1)) {
        WARNF("Psyz_SpuRead: bad offset 0x%X", reg_offset);
        return 0;
    }
    Psyz_AudioLock();
    unsigned short value;
    if (reg_offset == offsetof(SPU_RXX, trans_addr)) {
        value = (unsigned short)((spu.transfer_addr >> 3) & 0xFFFF);
    } else if (reg_offset == offsetof(SPU_RXX, spustat)) {
        // lower 6 bits mirror SPUCNT; bit 11 flips with the capture position
        value = (_spu_RXX->rxx.spucnt & 0x3F) |
                (_spu_RXX->rxx.spustat & (1u << 11));
    } else {
        value = _spu_RXX->raw[reg_offset >> 1];
    }
    Psyz_AudioUnlock();
    return value;
}

// Volume register as a gain of 0..0x7FFF; bit 15 (sweep) is not supported,
// a negative (inverted) volume plays at its magnitude.
static int volume_gain(unsigned short reg) {
    if (reg & 0x8000) {
        LOG_ONCE("voice volume sweep (bit 15) not implemented");
        return 0;
    }
    int value = (short)((reg & 0x7FFF) << 1);
    if (value < 0)
        value = -value;
    return value > 0x7FFF ? 0x7FFF : value;
}

static DcAicaLevel voice_level(unsigned v, int main_left, int main_right) {
    volatile SPU_VOICE_REG* reg = &_spu_RXX->rxx.voice[v];
    int env = spu.voice[v].env_vol;
    int left = ((volume_gain(reg->volume.left) * env >> 15) * main_left) >> 15;
    int right = ((volume_gain(reg->volume.right) * env >> 15) * main_right) >> 15;
    DcAicaLevel level = {255, 0};
    int loud = left > right ? left : right;
    int quiet = left > right ? right : left;
    if (loud <= 0)
        return level;
    level.tl = tl_table[loud >> 3];
    unsigned steps = pan_table[(quiet << 8) / loud];
    // KOS's convention: bit 4 lowers the right side
    level.pan = (u8)(steps ? (left > right ? 0x10 | steps : steps) : 0);
    return level;
}

// Starts a keyed-on voice's channel: from its start block to the end block,
// looping when the end block repeats.
static void hw_start(unsigned v, DcAicaLevel level) {
    VoiceState* vs = &spu.voice[v];
    HwVoice* hw = &spu.hw[v];
    unsigned start = vs->cur_addr / ADPCM_BLOCK_BYTES;
    unsigned limit = start + MAX_RUN_BLOCKS;
    if (limit > spu.pcm_blocks)
        limit = spu.pcm_blocks;
    if (start >= limit) {
        LOG_ONCE("a voice starts beyond the decoded SPU RAM; it is silent");
        if (hw->on)
            DcAica_Stop(v);
        hw->on = 0;
        return;
    }
    unsigned end = limit - 1, loop_block = BLOCKS;
    int loop = 0;
    for (unsigned b = start; b < limit; ++b) {
        u8 flags = spu.ram[b * ADPCM_BLOCK_BYTES + 1];
        if (flags & 4)
            loop_block = b;
        if (flags & 1) {
            end = b;
            loop = (flags & 2) != 0;
            break;
        }
    }
    if (loop && loop_block == BLOCKS) {
        unsigned reg_block = (unsigned)((_spu_RXX->rxx.voice[v].loop_addr << 3) &
                                        (PSYZ_SPU_RAM_SIZE - 1)) /
                             ADPCM_BLOCK_BYTES;
        loop_block = reg_block >= start && reg_block <= end ? reg_block : start;
    }
    hw->length = (end - start + 1) * ADPCM_BLOCK_SAMPLES;
    hw->loop = (u8)loop;
    hw->pos = 0;
    hw->pitch = _spu_RXX->rxx.voice[v].pitch & 0x3FFF;
    hw->level = level;
    DcAica_Start(v, start * ADPCM_BLOCK_SAMPLES, hw->length,
                 loop ? (loop_block - start) * ADPCM_BLOCK_SAMPLES : 0, loop,
                 hw->pitch, level);
    hw->on = 1;
}

static void hw_stop(unsigned v) {
    if (spu.hw[v].on)
        DcAica_Stop(v);
    spu.hw[v].on = 0;
}

void DcSpu_Update(unsigned frames) {
    Psyz_AudioLock();
    if (!spu.initialized) {
        Psyz_AudioUnlock();
        return;
    }
    if (spu.dirty_lo < spu.dirty_hi)
        decode_dirty();
    volatile SPU_RXX* rxx = &_spu_RXX->rxx;
    int muted = !(rxx->spucnt & SPU_CTRL_MASK_MUTE_SPU);
    int main_left = muted ? 0 : volume_gain(rxx->main_vol.left);
    int main_right = muted ? 0 : volume_gain(rxx->main_vol.right);
    for (unsigned v = 0; v < PSYZ_SPU_NUM_VOICES; v++) {
        VoiceState* vs = &spu.voice[v];
        HwVoice* hw = &spu.hw[v];
        volatile SPU_VOICE_REG* reg = &rxx->voice[v];
        int started = 0;
        if (vs->key_on_pending) {
            spu_voice_key_on(vs, reg);
            hw_start(v, (DcAicaLevel){255, 0});
            started = 1;
        }
        if (!vs->active) {
            hw_stop(v);
            continue;
        }
        // AKAO changes attack and release on voices that are already playing
        vs->adsr_lo = reg->adsr[0];
        vs->adsr_hi = reg->adsr[1];
        spu_voice_envelope_advance(vs, frames);
        unsigned pitch = reg->pitch & 0x3FFF;
        if (hw->on && !hw->loop && !started) {
            hw->pos += (uint64_t)pitch * frames;
            if (hw->pos >= (uint64_t)hw->length << 12) {
                // the end block without a repeat stops the voice
                vs->active = 0;
                vs->env_vol = 0;
                vs->env_state = ADSR_OFF;
            }
        }
        reg->volumex = (unsigned short)vs->env_vol;
        if (!vs->active) {
            hw_stop(v);
            continue;
        }
        if (!hw->on)
            continue;
        if (pitch != hw->pitch) {
            DcAica_SetPitch(v, pitch);
            hw->pitch = (u16)pitch;
        }
        DcAicaLevel level = voice_level(v, main_left, main_right);
        if (level.tl != hw->level.tl || level.pan != hw->level.pan) {
            DcAica_SetLevel(v, level);
            hw->level = level;
        }
    }
    // the capture position runs at 44.1 kHz; SPUSTAT bit 11 shows its half
    unsigned end = spu.capture_pos + 2 * frames;
    if (((end >> 9) - (spu.capture_pos >> 9)) & 1)
        rxx->spustat ^= 1u << 11;
    spu.capture_pos = (u16)(end & 0x3FF);
    Psyz_AudioUnlock();
}

void DcSpu_MixCd(short* out, unsigned frames) {
    unsigned got = (unsigned)Psyz_CdPullSamples(out, frames);
    Psyz_AudioLock();
    volatile SPU_RXX* rxx = &_spu_RXX->rxx;
    int on = (rxx->spucnt & SPU_CTRL_MASK_CD_AUDIO_ENABLE) != 0;
    int left = on ? (short)rxx->cd_vol.left : 0;
    int right = on ? (short)rxx->cd_vol.right : 0;
    int main_left = volume_gain(rxx->main_vol.left);
    int main_right = volume_gain(rxx->main_vol.right);
    Psyz_AudioUnlock();
    for (unsigned i = 0; i < got; i++) {
        int l = (out[i * 2] * left) >> 15, r = (out[i * 2 + 1] * right) >> 15;
        out[i * 2] = clamp16((clamp16(l) * main_left) >> 15);
        out[i * 2 + 1] = clamp16((clamp16(r) * main_right) >> 15);
    }
    memset(out + got * 2, 0, (frames - got) * 2 * sizeof(short));
}

void Psyz_SpuFlushCdSamples(void) {}

// Voices play on the AICA: only CD audio comes out of here.
void Psyz_SpuPullSamples(short* out, int num_frames) {
    DcSpu_MixCd(out, (unsigned)num_frames);
}
