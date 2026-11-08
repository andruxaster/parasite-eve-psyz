// Sega Dreamcast audio: lock and init around the portable SPU mixer
// (psyz/psyz_spu.c). There is no AICA output yet, so nothing is heard; the
// game's SPU state still advances because the mixer runs when pulled.
#include <psyz.h>
#include <psyz/log.h>
#include <stddef.h>
#include <kos/mutex.h>

static mutex_t audio_lock = RECURSIVE_MUTEX_INITIALIZER;
static int is_audio_init;
static int is_paused;
static unsigned output_volume = 100;
static PsyzAudioCaptureCallback capture_callback;
static void* capture_userdata;

void Psyz_AudioSetCaptureCallback(
    PsyzAudioCaptureCallback callback, void* userdata) {
    Psyz_AudioLock();
    capture_callback = callback;
    capture_userdata = userdata;
    Psyz_AudioUnlock();
}

int Psyz_AudioInit(void) {
    if (is_audio_init) {
        return 0;
    }
    Psyz_SpuInit();
    is_audio_init = 1;
    LOG_ONCE("no AICA output yet: audio is silent");
    return 0;
}

void Psyz_AudioDestroy(void) {
    Psyz_AudioLock();
    capture_callback = NULL;
    capture_userdata = NULL;
    is_audio_init = 0;
    Psyz_AudioUnlock();
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
