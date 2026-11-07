#!/bin/bash
. "$(dirname "$0")/env.sh"
mkdir -p $OUTDIR/prof
# profall.sh: io-write profile of each game in games.txt, frames 1300-2400
for g in sm64 aw ph ml ct pkd layton cv rs; do
  rm -rf $SDROOT/prof_$g; cp -al $SDROOT/$g $SDROOT/prof_$g
  rm -rf $SDROOT/prof_$g/_pico/states
  IOPROF=$OUTDIR/prof/$g.txt IOPROF_FROM=1300 ./dsrun_new.sh $SDROOT/prof_$g 2400 > /dev/null 2>&1
done
for g in sm64 aw ph ml ct pkd layton cv rs; do echo "== $g"; sed -n '1,3p' $OUTDIR/prof/$g.txt; grep '^other' $OUTDIR/prof/$g.txt | cut -c1-300; done
