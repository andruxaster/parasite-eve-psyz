// Sega Dreamcast audio. The SPU's voices play on AICA channels (dc_spu.c);
// a thread advances them with the clock every few milliseconds and keeps
// KOS's sound stream fed with CD/XA audio, which libcd decodes on demand.
// XA plays at its own 37.8 kHz: the stream restarts at whatever rate libcd
// delivers, so nothing is resampled on the SH-4.
#include <psyz.h>
#include <psyz/log.h>
#include <psyz/cd.h>
#include <stddef.h>
#include <string.h>
#include <arch/timer.h>
#include <dc/sound/stream.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include "dc_spu.h"

#define UPDATE_MS 5
#define STREAM_FRAMES 2048 // stream chunk: up to about 46 ms

static mutex_t audio_lock = RECURSIVE_MUTEX_INITIALIZER;
static int is_audio_init;
static volatile int is_paused;
static volatile int stop_requested;
static unsigned output_volume = 100;
static PsyzAudioCaptureCallback capture_callback;
static void* capture_userdata;
static snd_stream_hnd_t stream = SND_STREAM_INVALID;
static unsigned stream_rate;
static kthread_t* thread;

void Psyz_AudioSetCaptureCallback(
    PsyzAudioCaptureCallback callback, void* userdata) {
    Psyz_AudioLock();
    capture_callback = callback;
    capture_userdata = userdata;
    Psyz_AudioUnlock();
}

static void* StreamCallback(snd_stream_hnd_t hnd, int bytes, int* got) {
    static short __attribute__((aligned(32))) buf[STREAM_FRAMES * 2];
    (void)hnd;
    unsigned frames = (unsigned)bytes / 4;
    if (frames > STREAM_FRAMES)
        frames = STREAM_FRAMES;
    if (is_paused)
        memset(buf, 0, frames * 4);
    else
        DcSpu_MixCd(buf, frames);
    *got = (int)frames * 4;
    return buf;
}

static void* AudioThread(void* arg) {
    (void)arg;
    uint64_t last = timer_us_gettime64();
    uint64_t remainder = 0; // frames * 1e6 not yet run
    while (!stop_requested) {
        uint64_t now = timer_us_gettime64();
        remainder += (now - last) * 44100;
        last = now;
        unsigned frames = (unsigned)(remainder / 1000000);
        remainder -= (uint64_t)frames * 1000000;
        if (frames && !is_paused)
            DcSpu_Update(frames);
        unsigned rate = Psyz_CdOutputRate();
        if (rate != stream_rate) {
            snd_stream_stop(stream);
            snd_stream_start(stream, rate, 1);
            stream_rate = rate;
        }
        snd_stream_poll(stream);
        thd_sleep(UPDATE_MS);
    }
    return NULL;
}

int Psyz_AudioInit(void) {
    if (is_audio_init) {
        return 0;
    }
    Psyz_SpuInit(); // also starts KOS's sound system (dc_aica.c)
    stream = snd_stream_alloc(StreamCallback, STREAM_FRAMES * 4);
    if (stream == SND_STREAM_INVALID) {
        ERRORF("no sound stream for CD audio");
        return -1;
    }
    snd_stream_volume(stream, 255);
    Psyz_CdSetNativeXaRate(1);
    stream_rate = Psyz_CdOutputRate();
    snd_stream_start(stream, stream_rate, 1);
    stop_requested = 0;
    thread = thd_create(0, AudioThread, NULL);
    if (!thread) {
        ERRORF("no audio thread");
        snd_stream_destroy(stream);
        stream = SND_STREAM_INVALID;
        return -1;
    }
    thd_set_prio(thread, PRIO_DEFAULT - 1);
    is_audio_init = 1;
    return 0;
}

void Psyz_AudioDestroy(void) {
    Psyz_AudioLock();
    capture_callback = NULL;
    capture_userdata = NULL;
    Psyz_AudioUnlock();
    if (!is_audio_init)
        return;
    stop_requested = 1;
    thd_join(thread, NULL);
    snd_stream_stop(stream);
    snd_stream_destroy(stream);
    stream = SND_STREAM_INVALID;
    is_audio_init = 0;
}

int Psyz_AudioWaitForTimer(void) { return 0; }

void Psyz_AudioPause(void) { is_paused = 1; }
int Psyz_AudioIsPaused(void) { return is_paused; }
void Psyz_AudioUnpause(void) { is_paused = 0; }

int Psyz_AudioSetOutputVolume(unsigned percent) {
    output_volume = percent > 100 ? 100 : percent;
    return 0;
}
unsigned Psyz_AudioGetOutputVolume(void) { return output_volume; }

void Psyz_AudioLock(void) { mutex_lock(&audio_lock); }
void Psyz_AudioUnlock(void) { mutex_unlock(&audio_lock); }
