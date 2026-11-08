#!/bin/sh
# Renders a synthetic PS1 frame through dc_gpu.c on the host, with a
# software stand-in for the PVR. Needs a 32-bit capable gcc (PS1 packets are
# 32-bit u_long words). Writes frame0.ppm and frame1.ppm.
set -e
here=$(dirname "$0")
psyz=$here/../../..
gcc -m32 -std=gnu11 -O1 -g -Wall -Wno-unused -Wno-misleading-indentation \
    -D__psyz -DPLATFORM_DREAMCAST=1 -I"$here/fake" -I"$psyz/include" \
    -I"$psyz/src" "$here/scene.c" "$here/fakepvr.c" \
    "$psyz/src/dreamcast/dc_gpu.c" -lm -o dc_gpu_scene
./dc_gpu_scene
