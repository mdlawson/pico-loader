#!/bin/bash
# make_pico.sh [DEST] - builds the loader (DSPICO platform) and the rts menu from this repo
# and assembles the files the harness copies to /_pico on each virtual SD (default $PICO_DIR).
# RTS_AUTOPILOT=1 builds the autopilot test build instead (unattended restore tests: the
# loader plays a key script and the menu runs the test by itself; see ../HANDOVER.md).
# Toolchain: Wonderful Toolchain + BlocksDS at /opt/wonderful (override with the variables).
set -e
. "$(dirname "$0")/env.sh"
DEST=${1:-$PICO_DIR}
W=${WONDERFUL_TOOLCHAIN:-/opt/wonderful}
SUFFIX=; [ "$RTS_AUTOPILOT" = 1 ] && SUFFIX=_autopilot
cd "$PICO_LOADER"
PATH=$W/bin:$PATH BLOCKSDS=${BLOCKSDS:-$W/thirdparty/blocksds/core} WONDERFUL_TOOLCHAIN=$W make loader9 loader7 RTS_AUTOPILOT=${RTS_AUTOPILOT:-0} -j"$(nproc)"
# the menu has one output for both builds: always from clean
( cd rtsmenu && PATH=$W/toolchain/gcc-arm-none-eabi/bin:$PATH make clean >/dev/null && PATH=$W/toolchain/gcc-arm-none-eabi/bin:$PATH make RTS_AUTOPILOT=${RTS_AUTOPILOT:-0} )
mkdir -p "$DEST"
cp "picoLoader9_DSPICO$SUFFIX.bin" "$DEST/picoLoader9.bin"
cp "picoLoader7$SUFFIX.bin" "$DEST/picoLoader7.bin"
cp rtsmenu/rtsmenu9.bin data/aplist.bin data/patchlist.bin data/savelist.bin "$DEST/"
ls -la "$DEST"
