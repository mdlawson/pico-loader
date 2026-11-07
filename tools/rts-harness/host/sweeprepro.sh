#!/bin/bash
# sweeprepro.sh CODE FRAMES [more dshost args] - boots one library game as sweep.py does (fresh SD
# folder and image, no game save, the generic input of a save point at FRAMES) and runs it for
# FRAMES frames without the restore test, so the debug variables that print at exit work
# (ABORT_RING, PC9_RING with PC_RING_N, IO_DUMP...). RTS_MODE=basic|full as for rtsall.sh.
. "$(dirname "$0")/env.sh"
CODE=$1; F=$2; shift 2
rom=$(romPath "$(grep "^$CODE|" "$HOSTDIR/lists/library.txt" | cut -d'|' -f2)")
[ -f "$rom" ] || { echo "no rom for $CODE" >&2; exit 1; }
sd=$SDROOT/repro_$CODE
rm -rf "$sd" "$OUTDIR/repro_$CODE.img"*
mkdir -p "$sd/_pico"
ln -f "$rom" "$sd/game.nds" 2>/dev/null || cp "$rom" "$sd/game.nds"
cp "$LAUNCHER" "$sd/"
cp "${PICO:-$PICO_DIR}"/* "$sd/_pico/"
if [ -n "$RTS_MODE" ]; then
  mkdir -p "$sd/_pico/states"
  printf 'mode=%s\r\n' "$RTS_MODE" > "$sd/_pico/states/$(dd if="$rom" bs=1 skip=12 count=4 2>/dev/null | tr -c 'A-Za-z0-9' '_').cfg"
fi
echo -n "/game.nds" > "$sd/launch.txt"
# shellcheck disable=SC2046
"$HOSTDIR/dsrun.sh" "$sd" "$F" --status 600 \
  $(python3 -I -c "import sys; sys.path.insert(0, '$HOSTDIR'); import sweep; print(' '.join(sweep.generic_input($F + 40)))") "$@"
