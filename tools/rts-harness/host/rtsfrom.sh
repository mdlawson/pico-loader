#!/bin/bash
. "$(dirname "$0")/env.sh"
# rtsfrom.sh CHECKPOINT_DIR OUTDIR [saveFrame:gapFrames] - the RTS restore test starting from a
# play.py checkpoint (its snapshot and sd image), so it runs in a scene reached by playing.
C=$1; D=$2; FR=${3:-120:180}
B=$BIOS
mkdir -p "$D"; rm -f "$D"/*.ppm "$D"/report.txt
gunzip -c "$C/state.mst.gz" > "$D/state.mst"
cp "$C/sd.img" "$D/sd.img"; rm -f "$D/sd.img.idx"
SD=$(python3 -c "import json;print(json.load(open('$C/info.json'))['sd'])")
DSI=$(python3 -c "import json;print('1' if json.load(open('$C/info.json'))['dsi'] else '')")
X=(); [ -n "$DSI" ] && X=(--bios9i $B/bios9i.bin --bios7i $B/bios7i.bin)
MB=$(( $(wc -c < "$D/sd.img") >> 20 )); [ $MB -gt 256 ] && export DSHOST_SD_MB=512
SECONDS=0
LOAD_STATE="$D/state.mst" SDIMG_ONLY=1 $DSHOST --dspico $DLDI \
  --bios9 $B/bios9.bin --bios7 $B/bios7.bin --firmware $B/firmware.bin "${X[@]}" \
  --cart "$SD/launcher.nds" --sd "$SD" --sdimg "$D/sd.img" --frames 60000 --status 600 \
  --rts-test "$FR" --rts-out "$D" --shot $(( ${FR%%:*} - 1 )):"$D/at_save.ppm" > "$D/stdout.txt" 2> "$D/stderr.txt"
rm -f "$D/state.mst" "$D/sd.img"  # the image is big and the report is what matters
echo "$(basename "$C") ($SECONDS s): $(grep '^result' "$D/report.txt" 2>/dev/null || echo 'no report')"
