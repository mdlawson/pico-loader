#!/bin/bash
# make_pico.sh [DEST] - builds the loader (DSPICO platform) and the rts menu from this repo
# and assembles the files the harness copies to /_pico on each virtual SD (default $PICO_DIR).
# Toolchain: Wonderful Toolchain + BlocksDS at /opt/wonderful (override with the variables).
set -e
. "$(dirname "$0")/env.sh"
DEST=${1:-$PICO_DIR}
W=${WONDERFUL_TOOLCHAIN:-/opt/wonderful}
cd "$PICO_LOADER"
PATH=$W/bin:$PATH BLOCKSDS=${BLOCKSDS:-$W/thirdparty/blocksds/core} WONDERFUL_TOOLCHAIN=$W make loader9 loader7 -j"$(nproc)"
( cd rtsmenu && PATH=$W/toolchain/gcc-arm-none-eabi/bin:$PATH make )
mkdir -p "$DEST"
cp picoLoader9_DSPICO.bin "$DEST/picoLoader9.bin"
cp picoLoader7.bin rtsmenu/rtsmenu9.bin data/aplist.bin data/patchlist.bin data/savelist.bin "$DEST/"
ls -la "$DEST"
