#!/bin/bash
# build_dshost.sh - builds the patched melonDS core and links dshost against it.
#   MELONDS=path to a melonDS checkout at the BASE commit with ../melonds-patches applied
#   (see ../HANDOVER.md). Run from anywhere; dshost lands next to this script.
set -e
. "$(dirname "$0")/env.sh"
M=${MELONDS:-$EMU/melonDS}
mkdir -p "$M/build"
( cd "$M/build" && [ -f CMakeCache.txt ] || cmake .. -DBUILD_QT_SDL=OFF -DCMAKE_BUILD_TYPE=Release -DENABLE_JIT=OFF -DENABLE_OGLRENDERER=OFF )
# dshost uses neither the JIT nor OpenGL (software renderer); on macOS the JIT's memory setup
# faults and the OpenGL header include fails under Apple clang
cmake --build "$M/build" --target core -j"$(nproc)"
cd "$HOSTDIR"
# the core's public defines (e.g. GDBSTUB_ENABLED) change class layouts: dshost must match
DEFS=$(sed -n 's/^CXX_DEFINES = //p' "$M/build/src/CMakeFiles/core.dir/flags.make")
g++ -std=c++20 -O2 $DEFS -I"$M/src" -I"$M/build/src" -I"$M/src/teakra/include" dshost.cpp platform.cpp \
  "$M/build/src/libcore.a" "$M/build/src/teakra/src/libteakra.a" -lpthread -o "${1:-dshost}"
echo "built $HOSTDIR/${1:-dshost}"
