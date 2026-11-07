#!/bin/bash
. "$(dirname "$0")/env.sh"
# replayall.sh PICO_DIR OUTDIR [saveFrame:gap] [session glob] - every play.py checkpoint, replayed
# from a cold boot with the given loader build, with the rts test there; 4 at a time
P=$1; OUT=$2; FR=${3:-120:180}; G=${4:-*}
mkdir -p "$OUT"
for c in $PLAYROOT/$G/checkpoints/*/; do
  s=$(basename $(dirname $(dirname $c))); l=$(basename $c)
  f=$(python3 -c "import json;print(json.load(open('$c/info.json'))['frame'])")
  echo "$s $l $f"
done | xargs -P "$(nproc)" -L1 bash -c 'd='"$OUT"'/$0_$1; python3 $HOSTDIR/play.py replay $0 $1 $d '"$FR"' --pico '"$P"' >/dev/null 2>&1; r=$(grep -h "^result\|^screens differing" $d/report.txt 2>/dev/null | tr "\n" " "); echo "$0 $1: ${r:-no report}"' | sort | tee "$OUT/summary.txt"
