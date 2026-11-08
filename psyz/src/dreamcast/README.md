# Sega Dreamcast target (KallistiOS)

Status: **link-only stub backend.** The PsyZ core and a game compile and link
for the SH-4 with KallistiOS, so code and memory size can be measured and the
platform layer can grow one piece at a time. Nothing is drawn yet.

| File | Role | State |
| --- | --- | --- |
| `dc_gpu.c` | `Draw_*` backend: GP0 packet lengths, a 1 MB CPU copy of the PS1 VRAM for load/store/move/clear | stub, draws nothing |
| `dc_video.c` | video, widescreen, sprite-atlas, interpolation and keyboard entry points the desktop backends provide | no-op |
| `dc_input.c` | first Dreamcast controller as a PS1 pad on port 0 (`maple`) | working |
| `dc_audio.c` | audio lock and init around the portable SPU in `psyz/psyz_spu.c` | no output yet |
| `dc_os.c` | threads, locks, bells and the microsecond clock on KOS threads, mutexes and semaphores | working |
| `../platform/plat_unix.c` | file and memory-card directory calls; KOS's VFS (`/cd`, `/pc`, `/sd`...) provides the POSIX calls it uses | shared |
| `dc_log.c` | `psyz_log` to stderr (dc-tool console) | working |

Build with the KallistiOS CMake toolchain:

```sh
source /opt/toolchains/dc/kos/environ.sh
cmake -S . -B build-dc -DCMAKE_TOOLCHAIN_FILE=$KOS_BASE/utils/cmake/kallistios.toolchain.cmake
```

The next step is the PVR renderer in `dc_gpu.c`, modelled on
`src/psp/psp_gpu.c`: decoded texture-page and CLUT caches, the STP passes for
semi-transparency, and textured, Gouraud and flat polygons through the PVR's
polygon lists.
