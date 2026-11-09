# Sega Dreamcast target (KallistiOS)

Status: **draws through the PowerVR; sound plays on the AICA (no reverb yet).** The PsyZ core and a
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
| `dc_audio.c` | audio thread: advances the SPU every 5 ms, feeds CD/XA audio to a KOS sound stream | first version |
| `dc_spu.c` | the SPU API: registers, envelopes, sample decoding, PS1 voices on AICA channels | first version |
| `dc_aica.c` | AICA channel registers and sound RAM, from the SH-4 | first version |
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

## Sound

The portable SPU (`psyz/psyz_spu.c`) mixes 24 voices with Gaussian
interpolation and reverb per 44.1 kHz sample. On the SH-4 that is 160 to 600
million instructions per second of sound, more than the CPU has. As the
PSP backend does with sceSasCore, `dc_spu.c` plays the voices on the sound
chip and keeps on the CPU what the game reads back:

- **Samples.** SPU RAM stays in main RAM. Written blocks of PS1 ADPCM are
  decoded to 16-bit PCM in AICA sound RAM (56 bytes per 16-byte block, so
  512 KB of SPU RAM takes 1.8 MB of the 2 MB).
- **Voices.** Each PS1 voice owns an AICA channel. At key-on the sample is
  scanned to its end block; a repeat flag loops it from the last loop-start
  block or the loop address register. Pitch maps to OCT/FNS.
- **Envelopes.** The SPU's own ADSR code runs on the CPU, advanced in bulk
  every update; with the voice and main volumes it sets the channel's total
  level and pan. ENVX reads back from it. A sample without a repeat ends
  when its tracked position passes its length.
- **CD and XA audio** go out on a KOS sound stream at 44.1 kHz.

Not done yet: reverb (the AICA DSP could), noise, pitch modulation, volume
sweeps and the capture buffers. Interpolation is linear rather than
Gaussian. In the PC harness, a software model of the channels against the
software SPU (reverb muted) gives the same envelopes frame for frame and
levels within 10%.

## Movies (MDEC)

The MDEC is the portable one in `../psyz/libpress.c`, exact to the bit
(each IDCT pass rounded as the PS1 does). On the SH-4 it is built for speed:

- **IDCT.** The SDK's matrix mirrors (even rows symmetric, odd rows
  `M[7-x] = ~M[x]`), so each pass needs half the products, and only the
  columns holding coefficients are transformed. The products run on
  `mac.w`, which reads two 16-bit values and accumulates in one
  instruction; a column with only its DC term, or a block with only
  column 0, skips them.
- **Colour.** Each 2x2 chroma sample becomes three pointers into a
  clamping table, and pixels are written backwards with pre-decrement
  byte stores (a byte store with displacement only takes r0).

A 320x240 frame went from about 20 to under 5 million SH-4 instructions
(about 120 ms to about 30 ms in Flycast), so movies keep their 15 frames
per second. `hosttest/mdec_check.sh` checks the output against the
original decoder on random streams.

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
- Reverb on the AICA DSP.
