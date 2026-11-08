// Sega Dreamcast: video presentation entry points.
//
// The desktop backends add internal resolution, widescreen, sprite atlases,
// frame interpolation and keyboard bindings. The Dreamcast renders the
// original 320x240 picture, so these report "not available" and do nothing.
#include <psyz.h>
#include <psyz/log.h>
#include <stdint.h>
#include <stdlib.h>
#include "../draw.h"

void Psyz_SetTitle(const char* str) { (void)str; }
void Psyz_SetWindowScale(int scale) { (void)scale; }

int Psyz_VideoSetInternalResolution(unsigned multiplier) {
    return multiplier == 1 ? 0 : -1;
}
unsigned Psyz_VideoGetInternalResolution(void) { return 1; }
int Psyz_VideoSetGeometryCorrection(int enabled) {
    (void)enabled;
    return -1;
}
int Psyz_VideoGetGeometryCorrection(void) { return 0; }
int Psyz_VideoSetDynamicLighting(int enabled) {
    (void)enabled;
    return -1;
}
int Psyz_VideoGetDynamicLighting(void) { return 0; }
int Psyz_VideoSetEffectGeometryCorrection(int enabled) {
    (void)enabled;
    return -1;
}
int Psyz_VideoGetEffectGeometryCorrection(void) { return 0; }
int Psyz_VideoGetAntialiasing(void) { return 0; }
int Psyz_VideoGetModelFiltering(void) { return 0; }
int Psyz_VideoGetWidescreen(void) { return 0; }
void Psyz_VideoSetWidescreenCentred(int enabled) { (void)enabled; }
void Psyz_VideoSetWidescreenField(int enabled) { (void)enabled; }
void Psyz_VideoSetWidescreenPicture(int buffer_y, int left, int right) {
    (void)buffer_y;
    (void)left;
    (void)right;
}
void Psyz_VideoSetWidescreenPictureShifted(int buffer_y, int left, int right,
                                           float dx, float dy, int camera_x) {
    (void)buffer_y;
    (void)left;
    (void)right;
    (void)dx;
    (void)dy;
    (void)camera_x;
}
int Psyz_VideoSetRgb24Backdrop(const char* filename,
                               const unsigned char* reference, int width,
                               int height, int top, int bottom) {
    (void)filename;
    (void)reference;
    (void)width;
    (void)height;
    (void)top;
    (void)bottom;
    return 0;
}

unsigned char* Psyz_VideoAllocRenderedFrame(int* w, int* h) {
    if (w) {
        *w = 0;
    }
    if (h) {
        *h = 0;
    }
    return NULL;
}

// HD sprite atlases are a desktop feature.
int Psyz_GpuLoadSpriteAtlas(uint64_t key, const char* filename) {
    (void)key;
    (void)filename;
    return 0;
}
void Psyz_GpuResetSpriteAtlases(void) {}
void Psyz_GpuSelectSpriteAtlas(uint64_t key, unsigned int cell) {
    (void)key;
    (void)cell;
}
void Psyz_GpuSetSpriteAtlasesEnabled(int enabled) { (void)enabled; }
int Psyz_GpuGetSpriteAtlasesEnabled(void) { return 0; }

// Frame interpolation is not available on this backend.
int Psyz_GpuSetFrameInterpolation(int enabled) {
    (void)enabled;
    return -1;
}
int Psyz_GpuGetFrameInterpolation(void) { return 0; }
int Psyz_GpuGetFrameInterpolationRequested(void) { return 0; }
void Psyz_GpuCopyInterpolationPacket(const void* from, const void* to) {
    (void)from;
    (void)to;
}

// The game remaps keyboard keys through these SDL3 helpers; there is no
// keyboard binding table on the Dreamcast.
int Psyz_GetKeyboardBinding_SDL3(unsigned button) {
    (void)button;
    return 0;
}
int Psyz_SetKeyboardBinding_SDL3(unsigned button, int key) {
    (void)button;
    (void)key;
    return -1;
}

int Psyz_QuitRequested(void) { return 0; }

int Psyz_DebugServer(int port) {
    (void)port;
    WARNF("debug server not supported on this platform");
    return -1;
}
void Psyz_DebugServerStop(void) {}
