#!/bin/bash
. "$(dirname "$0")/env.sh"
# dsrun.sh SDDIR FRAMES [more dshost args] - the DSpico on a DS (your dumps)
B=$BIOS
SD=$1; F=$2; shift 2
# sd image size: room for the folder (the image is sparse); DSHOST_SD_MB overrides
[ -n "$DSHOST_SD_MB" ] || { m=$(du -smL "$SD" | cut -f1); export DSHOST_SD_MB=$(( m < 200 ? 256 : m + m / 4 + 64 )); }
exec $HOSTDIR/dshost.new --dspico $DLDI --dspico-delay ${DSPICO_DELAY:-0} \
  --bios9 $B/bios9.bin --bios7 $B/bios7.bin --firmware $B/firmware.bin \
  --cart $SD/launcher.nds --sd $SD --sdimg $OUTDIR/$(basename $SD).img --frames $F "$@"
