# Sega Dreamcast target (KallistiOS)

Status: **draws through the PowerVR; audio is silent.** The PsyZ core and a
game compile and link for the SH-4 with KallistiOS. The renderer has been
checked on the host against a software stand-in for the PVR (see
`hosttest/`), not yet on hardware or in an emulator.

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
- **Textures.** A primitive's texture page (4, 8 or 16 bpp, plus its CLUT)
  is decoded from that copy into a 256x256 ARGB1555 texture in PVR memory.
  Up to 32 pages are cached (about 30 fit beside the framebuffers and the
  1 MB vertex buffer); writes to VRAM mark them dirty. A page already drawn
  in the current frame is decoded into another slot rather than overwritten,
  so earlier primitives keep their texture.
- **Transparency.** Colour 0 is transparent. Semi-transparency modes 0
  (B/2+F/2), 1 (B+F) and 3 (B+F/4) use PVR blending with vertex alpha;
  mode 2 (B-F) has no PVR equivalent and is approximated as B*(1-F).
- **Uploads on screen.** `LoadImage`, `MoveImage` and `ClearImage` into the
  displayed framebuffer are drawn as quads, as in the PSP backend.
- **Scale.** The PS1 framebuffer (256-640 x 240/480) is stretched to
  640x480.

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
build and writes `frame0.ppm` and `frame1.ppm`.

## Next

- Run on hardware or Flycast with the discs; check the frame origin logic
  with the game's real draw/display environments.
- Speed: vertices through the store queues (`pvr_dr_*`) instead of
  `pvr_prim`, twiddled or paletted textures.
- Audio on the AICA.
