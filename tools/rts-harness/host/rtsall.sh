#!/bin/bash
. "$(dirname "$0")/env.sh"
# rtsall.sh LIST OUTDIR [ds|dsi] - the automated RTS restore test over a list of games.
# LIST lines: name|rom path|saveFrame:gapFrames|extra dshost args (keys to reach gameplay)
# Each game gets OUTDIR/name/{report.txt,rtslog.txt,first_diff.ppm,stderr.txt}; a summary
# goes to OUTDIR/summary.txt. Games run in parallel, one per cpu.
LIST=$1; OUT=$2; MODE=${3:-ds}
mkdir -p "$OUT"
run() {
  IFS='|' read -r name rom frames extra <<< "$1"
  [ -n "$rom" ] && rom=$(romPath "$rom")
  [ -z "$name" ] || [ "${name:0:1}" = "#" ] && return
  d="$OUT/$name"; sd="$SDROOT/$name"
  mkdir -p "$d" "$sd/_pico"
  ln -f "$rom" "$sd/game.nds" 2>/dev/null || cp "$rom" "$sd/game.nds"
  cp $LAUNCHER "$sd/"
  cp "${PICO:-$PICO_DIR}"/* "$sd/_pico/"
  if [ -n "$KEEP_SHD" ]; then rm -f "$sd/_pico/states/"*.rts "$sd/_pico/rtslog.txt"; else rm -rf "$sd/_pico/states" "$sd/_pico/rtslog.txt"; fi
  # RTS_MODE=basic|full: the game's mode file, short (the loader pads it)
  if [ -n "$RTS_MODE" ]; then
    mkdir -p "$sd/_pico/states"
    printf 'mode=%s\r\n' "$RTS_MODE" > "$sd/_pico/states/$(dd if="$rom" bs=1 skip=12 count=4 2>/dev/null | tr -c 'A-Za-z0-9' '_').cfg"
  fi
  echo -n "/game.nds" > "$sd/launch.txt"
  # the game's own save from an earlier run would change how it boots: start without one
  # (KEEP_SAV=1 keeps it; SAV=file starts from that save)
  [ -n "$KEEP_SAV" ] || rm -f "$sd/game.sav"
  [ -n "$SAV" ] && cp "$SAV" "$sd/game.sav"
  [ -n "$KEEP_SHD" ] || rm -f $OUTDIR/$name.img*
  script=$HOSTDIR/dsrun.sh; [ "$MODE" = dsi ] && script=$HOSTDIR/dsirun.sh
  # shellcheck disable=SC2086
  SECONDS=0; $script "$sd" 60000 --status 600 --shot 1200:"$d/at_1200.ppm" --rts-test "$frames" --rts-out "$d" $TRACE_EXTRA \
     --shot $(( ${frames%%:*} - 1 )):"$d/at_save.ppm" $extra > "$d/stdout.txt" 2> "$d/stderr.txt"
  echo "$name ($SECONDS s): $(grep '^result' "$d/report.txt" 2>/dev/null || echo 'no report') ($(tail -1 "$d/stderr.txt"))"
  # RTS_CLEAN=1: drop the sd image and folder, the ram dumps and the sound after the run (sweeps)
  if [ -n "$RTS_CLEAN" ]; then
    rm -rf "$OUTDIR/$name.img"* "$sd"
    rm -f "$d"/ram_*.bin "$d"/*.wav
  fi
}
export -f run; export OUT MODE PICO KEEP_SHD RTS_MODE TRACE_EXTRA KEEP_SAV SAV RTS_CLEAN
grep -v '^[[:space:]]*$' "$LIST" | tr '\n' '\0' | xargs -0 -n1 -P "$(nproc)" bash -c 'run "$1"' _ | tee "$OUT/summary.txt"
