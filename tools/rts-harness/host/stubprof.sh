#!/bin/bash
. "$(dirname "$0")/env.sh"
# stubprof.sh CHECKPOINT OUTFILE [frames] - stores to shadowed registers per frame (stub hits)
# from a play.py checkpoint, by the profiling dshost (SHADOWLOG)
C=$1; O=$2; F=${3:-300}
B=$BIOS
T=$(mktemp -d $OUTDIR/stubprof/tmp.XXXX)
gunzip -c "$C/state.mst.gz" > "$T/state.mst"; cp "$C/sd.img" "$T/sd.img"
SD=$(python3 -c "import json;print(json.load(open('$C/info.json'))['sd'])")
MB=$(( $(wc -c < "$T/sd.img") >> 20 )); [ $MB -gt 256 ] && export DSHOST_SD_MB=512
LOAD_STATE="$T/state.mst" SDIMG_ONLY=1 SHADOWLOG="$O" SHADOWLOG_FROM=0 $HOSTDIR/dshost.prof \
  --dspico $DLDI \
  --bios9 $B/bios9.bin --bios7 $B/bios7.bin --firmware $B/firmware.bin \
  --cart "$SD/launcher.nds" --sd "$SD" --sdimg "$T/sd.img" --frames $F > /dev/null 2> "$O.err"
rm -rf "$T"
echo "$(basename $(dirname $(dirname $C)))/$(basename $C): $(cat $O.sum 2>/dev/null)"
