#!/bin/bash
. "$(dirname "$0")/env.sh"
# dsirun.sh SDDIR FRAMES [more dshost args] - the DSpico on a DSi
B=$BIOS
SD=$1; F=$2; shift 2
# sd image size: room for the folder (the image is sparse); DSHOST_SD_MB overrides
[ -n "$DSHOST_SD_MB" ] || { m=$(du -smL "$SD" | cut -f1); export DSHOST_SD_MB=$(( m < 200 ? 256 : m + m / 4 + 64 )); }
exec $DSHOST --dspico $DLDI --dspico-delay ${DSPICO_DELAY:-50} \
  --bios9 $B/bios9.bin --bios7 $B/bios7.bin --bios9i $B/bios9i.bin --bios7i $B/bios7i.bin --firmware $B/firmware.bin \
  --cart $SD/launcher.nds --sd $SD --sdimg $OUTDIR/$(basename $SD).img --frames $F "$@"
