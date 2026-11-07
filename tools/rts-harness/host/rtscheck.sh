#!/bin/bash
. "$(dirname "$0")/env.sh"
# rtscheck.sh OUTDIR [saveFrame:gap] [glob] - rtsfrom.sh over every play.py checkpoint, 4 at a time
OUT=$1; FR=${2:-120:180}; G=${3:-*}
mkdir -p "$OUT"
ls -d $PLAYROOT/$G/checkpoints/*/ | sed 's#/$##' | \
  xargs -P "$(nproc)" -I{} bash -c 'c={}; s=$(basename $(dirname $(dirname $c))); $HOSTDIR/rtsfrom.sh $c '"$OUT"'/${s}_$(basename $c) '"$FR"' | sed "s/^/$s /"' | sort | tee "$OUT/summary.txt"
