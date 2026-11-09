#!/bin/sh
# Checks libpress.c's MDEC decoder against the original's output on random
# streams (see mdec_check.c).
set -e
here=$(dirname "$0")
psyz=$here/../../..
gcc -O2 -Wall -D__psyz -I"$psyz/include" -I"$psyz/src" -I"$psyz/../decomp/include" \
    "$here/mdec_check.c" -o mdec_check
./mdec_check
