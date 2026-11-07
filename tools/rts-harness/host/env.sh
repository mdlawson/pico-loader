# env.sh - shared paths for the RTS harness. Every script sources this; override any
# variable in the environment before running. Nothing here is checked in except code:
# BIOS/firmware, ROMs and the loader build must be supplied (see ../HANDOVER.md).
HOSTDIR=${HOSTDIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
HARNESS=${HARNESS:-$(dirname "$HOSTDIR")}
PICO_LOADER=${PICO_LOADER:-$(cd "$HARNESS/../.." && pwd)}
EMU=${EMU:-$HOME/emu}                      # scratch root for everything not in git
BIOS=${BIOS:-$EMU/bios}                    # bios7.bin bios9.bin firmware.bin + dsi_* (see HANDOVER)
ROMS=${ROMS:-$EMU/roms}                    # relative rom paths in lists resolve here
SDROOT=${SDROOT:-$EMU/rtssd}               # per-game virtual SD folders
OUTDIR=${OUTDIR:-$EMU/out}                 # sd images and misc output
PLAYROOT=${PLAYROOT:-$EMU/play}            # play.py sessions and checkpoints
PICO_DIR=${PICO_DIR:-$EMU/rts_pico}        # built loader files copied to /_pico
LAUNCHER=${LAUNCHER:-$HARNESS/launcher/launcher.nds}
DLDI=${DLDI:-$PICO_LOADER/arm9/data/dspicoDldi.bin}
DSHOST=${DSHOST:-$HOSTDIR/dshost}
mkdir -p "$OUTDIR" "$SDROOT"
export HOSTDIR HARNESS PICO_LOADER EMU BIOS ROMS SDROOT OUTDIR PLAYROOT PICO_DIR LAUNCHER DLDI DSHOST
# romPath NAME: absolute paths pass through, others are relative to $ROMS
romPath() { case "$1" in /*) echo "$1";; *) echo "$ROMS/$1";; esac; }
export -f romPath
