# Sega Dreamcast target (KallistiOS)

Status: **draws through the PowerVR; audio is silent.** The PsyZ core and a
game compile and link for the SH-4 with KallistiOS. The renderer has been
checked on the host against a software stand-in for the PVR: a synthetic
frame (`hosttest/`) and Parasite Eve itself from boot through its movies,
title, name entry and opening field scenes to the opera fire, compared with
the desktop software renderer. Not yet on hardware or in an emulator.

| File | Role | State |
| --- | --- | --- |
| `dc_gpu.c` | `Draw_*` backend on the PVR, frame pacing (`Psyz_VideoVSync`) | first version |
| `dc_video.c` | video, widescreen, sprite-atlas, interpolation and keyboard entry points the desktop backends provide | no-op |
| `dc_input.c` | first Dreamcast controller as a PS1 pad on port 0 (`maple`) | working |
| `dc_audio.c` | audio lock and init around the portable SPU in `psyz/psyz_spu.c` | no output yet |
| `dc_os.c` | threads, locks, bells and the microsecond clock on KOS threads, mutexes and semaphores | working |
| `../platform/plat_unix.c` | file and memory-card directory calls; KOS's VFS (`/cd`, `/pc`, `/sd`...) provides the POSIX calls it uses | shared |
| `dc_log.c` | `psyz_log` to stderr (dc-tool console) | working |

## Renderer

- **Order.** The PS1 has no depth buffer; later primitives cover earlier
  ones. Everything goes into the PVR's translucent list with autosort
  disabled, so the PVR keeps the submission order. A scene opens at the
  first primitive and is shown at the game's next `VSync`.
- **VRAM.** A 1 MB CPU copy of the PS1 VRAM takes uploads, fills and copies,
  and answers `StoreImage`.
- **Textures.** A primitive's texels (4, 8 or 16 bpp, plus its CLUT) are
  decoded from that copy into an ARGB1555 block in PVR memory: the
  smallest power-of-two rectangle (16 to 256 a side, starting on a
  multiple of 16) over its UV box. Pools of 1K, 4K, 16K, 32K and 64K-texel
  slots fill the PVR memory left beside the framebuffers and the vertex
  buffers (about 5 MB). A page+CLUT that misses several small blocks in a
  frame, such as a model part drawn as many triangles, gets one region
  block over all the UVs drawn with it. A slot tracks the texels actually
  drawn from it, so only writes there make it dirty (page 15 wraps into
  the framebuffer, which is cleared every frame). A slot drawn in the
  current frame is never overwritten: the PVR reads textures when it
  renders the finished scene, after the CPU has moved on. Every 300 frames
  the backend logs frame rate, decodes and where the time went.
- **Transparency.** Colour 0 is transparent. Semi-transparency modes 0
  (B/2+F/2), 1 (B+F) and 3 (B+F/4) use PVR blending with vertex alpha.
  Mode 2 (B-F) has no PVR blend, so the primitive is drawn three times:
  invert the framebuffer under it, add F, invert again. That is exact,
  clamping included (character shadows use it).
- **24-bit display.** MDEC movies put packed RGB888 in VRAM and switch
  the display to 24 bits. Then primitives are skipped and, at VSync, the
  displayed rectangle is converted to one RGB565 texture and drawn whole,
  only when VRAM changed.
- **Display range.** The vertical display range places the picture: a
  game showing 224 lines gets them 8 lines down, with the rest black, as
  on a PS1.
- **Uploads on screen.** `LoadImage`, `MoveImage` and `ClearImage` into the
  displayed framebuffer are drawn as quads, as in the PSP backend.
- **Scale.** The PS1 framebuffer (256-640 x 240/480) is stretched to
  640x480; the horizontal display range is ignored.

Not emulated yet: drawing into VRAM and sampling it back as a texture,
reading rendered pixels back, the draw-area clip, texture windows, mask
bits, dithering, per-texel STP on semi-transparent primitives (the whole
primitive blends), and textured colours above 0x80 (the PVR modulates up to
1.0, the PS1 up to 2.0).

## Build

```sh
source /opt/toolchains/dc/kos/environ.sh
cmake -S . -B build-dc -DCMAKE_TOOLCHAIN_FILE=$KOS_BASE/utils/cmake/kallistios.toolchain.cmake
```

`hosttest/build.sh` renders a synthetic frame (every texture depth, CLUTs,
sprites, Gouraud, the four blend modes, lines, an upload into the
framebuffer, a CLUT change mid-frame) through `dc_gpu.c` on a 32-bit host
build and writes `frame0.ppm` and `frame1.ppm`. Like the PVR, its stand-in
draws a scene only when it is finished, so a texture overwritten after use
in the same frame shows up as it would on hardware.

## Next

- Run on hardware or Flycast with the discs; check the frame origin logic
  with the game's real draw/display environments.
- Speed: vertices through the store queues (`pvr_dr_*`) instead of
  `pvr_prim`, twiddled or paletted textures.
- Audio on the AICA.
