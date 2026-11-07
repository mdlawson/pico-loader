# RTS (save states) for Pico Loader: handover

This branch (`rts-freeze-test`) adds real-time save states to Pico Loader for the DSpico. Press
L+R+Up+Select in a game for a menu that saves, loads, switches mode, or exits to the launcher.
This directory holds what's needed to keep testing it in melonDS on more games: a headless
emulator harness, the melonDS patches it needs, the test launcher, and the notes for playing games
by agent. ROMs, BIOS/firmware and the multi-GB play snapshots are not included (the play sessions are, as inputs); see "What you supply".

```
tools/rts-harness/
  HANDOVER.md          this file
  host/                harness: dshost (headless melonDS), test scripts, lists, analysis tools
    env.sh             every path the scripts use; override by environment variables
    lists/             game lists for rtsall.sh (rom paths relative to $ROMS)
    tools/             one-off analysis scripts from the shadow work (see "Tools")
  melonds-patches/     3 patches on upstream melonDS 1e65444, plus how to build
  launcher/            the tiny launcher.nds the loader boots on exit (source + prebuilt)
  play/PLAYING.md      instructions for an agent playing a game through play.py
  play/sessions/       the 16 played sessions as inputs + starting saves (replayable)
  play/saves/          in-game saves exported from them
  BUILD_NOTES.txt      per-build notes as delivered, builds 13-33
```

## What you supply

| What | Where (default) | Notes |
|---|---|---|
| DS BIOS + firmware | `$BIOS` = `~/emu/bios` | `bios7.bin`, `bios9.bin`, `firmware.bin` |
| DSi BIOS | `$BIOS` | `bios7i.bin`, `bios9i.bin` (only for `dsi` runs). No NAND needed. |
| ROMs | `$ROMS` = `~/emu/roms` | Lists use `<id>/<file>.nds` relative to `$ROMS`; absolute paths work too. Clean dumps; trimmed is fine. |
| melonDS checkout | `$MELONDS` = `~/emu/melonDS` | At 1e65444 with `melonds-patches/*.patch` applied. |
| Wonderful Toolchain + BlocksDS | `/opt/wonderful` | For the loader, the menu and the launcher. |

Everything else the scripts create goes under `$EMU` (`~/emu`): `$SDROOT` (one virtual SD folder per
game), `$OUTDIR` (SD images), `$PLAYROOT` (play sessions), `$PICO_DIR` (built loader files). The
SD images are sparse and the ROMs are hard-linked into the SD folders, so give it a few GB free
**and watch the disk**: when the disk fills, a write through a hard link can truncate the ROM in
`$ROMS` itself. That happened once here. Delete `$OUTDIR/*.img*` after big runs.

## Setup, once

```bash
cd ~/emu/melonDS && git checkout 1e65444 && git am /path/to/pico-loader/tools/rts-harness/melonds-patches/*.patch
```

```bash
tools/rts-harness/host/build_dshost.sh
```

```bash
tools/rts-harness/host/make_pico.sh
```

`make_pico.sh` builds `loader9 loader7` (DSPICO platform) and `rtsmenu/rtsmenu9.bin`, then copies
picoLoader9.bin (renamed from picoLoader9_DSPICO.bin), picoLoader7.bin, rtsmenu9.bin, aplist.bin,
patchlist.bin and savelist.bin to `$PICO_DIR`. Pass another directory as its argument to keep
several builds side by side (e.g. `~/emu/pico_v19`), and point a run at one with `PICO=dir`.
The launcher only needs rebuilding if you change it (`make` in `launcher/` with the same
toolchain variables as the loader; it includes `include/picoLoader7.h` from this repo).

Check it works (about a minute):

```bash
cd tools/rts-harness/host && grep '^pkd' lists/games_all.txt > /tmp/one.txt && ./rtsall.sh /tmp/one.txt /tmp/rts_one ds
```

Expect `pkd (25 s): result: DIFFERS ...` with `screens differing: 0` in `/tmp/rts_one/pkd/report.txt`.
"DIFFERS" is normal (see "Reading a report").

## The restore test (rtsall.sh)

```
rtsall.sh LIST OUTDIR [ds|dsi]
```

Each list line is `name|rom|saveFrame:gapFrames|extra dshost args`, e.g.

```
rs3950|rs/Dragon Quest Heroes - Rocket Slime (USA).nds|3950:300|--key 1400:START:4 --key 1500:A:4 --key 1700:A:4 ...
```

For each game, in parallel (one per CPU), it makes `$SDROOT/name` with the ROM as `/game.nds`,
`launch.txt`, the launcher and `/_pico`, then boots the launcher in dshost (`dsrun.sh`, or
`dsirun.sh` for a DSi). The launcher hands `/game.nds` to Pico Loader. dshost then drives the RTS
menu's own built-in test:

1. At `saveFrame` it holds the combo and picks "Test: save + reference". The menu saves a state;
   the game runs on and dshost records 120 frames (screens, registers, sound) as the reference.
2. `gapFrames` later it opens the menu again and picks "Test: load + compare". The state loads and
   the next 120 frames are compared with the reference, lined up by a frame counter the ARM7 keeps
   (`rts_info_t.testFrames`).

`extra` is any dshost arguments, normally `--key FRAME:KEYS:LEN` / `--touch FRAME:X:Y:LEN` presses
to get from the title screen into gameplay before `saveFrame`. Pick a save frame in real gameplay,
after the game has settled (a scene that's mid-fade gives noise). Expect to try a couple; look at
`at_save.ppm` to see what the save point was.

Per game, `OUTDIR/name/` gets `report.txt`, `stderr.txt`, `at_1200.ppm`, `at_save.ppm`,
`first_diff.ppm` (reference | after-load, side by side) and the sound of both runs. `OUTDIR/summary.txt`
has one line per game.

Useful variables for a run: `PICO=dir` (loader build), `RTS_MODE=basic|full` (forces the game's mode
file), `DSPICO_DELAY=n` (SD latency; dsirun defaults to 50), `DSHOST_SD_MB` (SD image size; by
default sized from the folder), `TRACE_EXTRA="args"` (more dshost args for every game),
`RTS_ALL=1` (write every 4th differing frame as a ppm).

### Reading a report

- `screens differing: N` is the main number: how many of the 120 frames after the load don't match
  the reference pixel for pixel. 0-3 is the norm; the user doesn't mind glitches that clear in a
  few frames.
- `registers differing` lists hardware registers that differ right after the load, including ones
  a DS can't read back. Some always differ and are expected: EXMEMCNT is saved as 0, timers'
  running counts, and in basic mode the sound channel controls (sound that was playing stops
  until the game starts new sounds).
- `memory differing` lists memory parts that differ two frames after the resume. Usually empty
  or tiny.
- `result: DIFFERS` just means something in those lists differs; it's the normal outcome.
  `FAILED (...)` means the test couldn't run: the menu didn't open, no rts_info in memory (the
  exit combo isn't patched in), or the load failed. Those are the real failures.
- The sound line compares levels; a big difference in basic mode is expected.

### Baseline (melonDS, current branch)

DS mode, `lists/games_all.txt`, screens differing after a load:

| game | screens | game | screens |
|---|---|---|---|
| sm64 | 2 | eba | 0 |
| aw | 10 (intro fade) | gsdd | 3 |
| ph | 3 | gta | 0 |
| ml | 2 | st | 3 |
| ct | 0 | mph | 28 (see open items) |
| pkd | 0 | nsmb | 0 |
| layton | 2 | dq9 | 0 |
| cv | 0 | diy | no menu, on purpose (NAND save) |
| rs (+3 later points) | 0 | | |

DSi mode, `lists/g_dsi3.txt` with `dsi`: pkb 0, mvdk 0, solat 1. The load check finds all
memory parts identical in all three.

## Playing games for deeper test points (play.py)

`rtsall.sh` only reaches what scripted key presses reach. For deeper scenes, an agent plays the
game through `play.py` (see `play/PLAYING.md`, written to be handed to a cheap agent as-is): it
steps the emulator a few seconds at a time, shows screenshots, and `mark`s checkpoints (a melonDS
snapshot plus the SD image) under `$PLAYROOT/<session>/checkpoints/`. Then:

- `rtscheck.sh OUTDIR [save:gap] [session glob]` runs the restore test from every checkpoint
  (`rtsfrom.sh` does one). Fast; uses the loader that was running when the checkpoint was taken.
- `replayall.sh PICO_DIR OUTDIR [save:gap] [glob]` replays each session's inputs from a cold boot
  with a different loader build, then tests there. Use this after changing the loader.

Games known to play.py are in its `GAMES` dict (ids → ROM under `$ROMS`); add new ones there.

**The sessions played so far** (16 games, 200 checkpoints) are in `play/sessions/` without their
snapshots: each has its inputs (`meta.json`), its checkpoints' frames (`checkpoints/*/info.json`)
and the save it started from. `play.py restore all` sets them up under `$PLAYROOT` (skipping games
whose ROM isn't in `$ROMS`), and then `replayall.sh` rebuilds and tests every scene from a cold
boot. Replays need the same patched melonDS and should use the same kind of BIOS/firmware dumps;
checked here for pkd1 (all 9 scenes reached, 0-5 screens differing). A restored session is
replay-only: `step`/`undo`/`rtscheck.sh` need snapshots, which a fresh `new` session makes.
`play/saves/` holds in-game saves exported from those sessions (`new ... --save FILE` starts there).

`BUILD_NOTES.txt` is the tester-facing history of builds 13-33: what changed, what was measured,
the known gaps at each point.

## Other scripts

| Script | Use |
|---|---|
| `dsrun.sh SD FRAMES [args]` / `dsirun.sh` | One dshost run of an SD folder as a DS / DSi. |
| `soak.py` / `soakcheck.py` | Long random-input play with and without the shadow; checks for aborts and hangs. Lists: `lists/soak_list*.txt`. |
| `scrsweep.sh` | Restore test with groups of registers scrambled at load (`RTS_SCRAMBLE`), to find what matters. |
| `profall.sh`, `stubprof.sh` | IO-write profiles per frame; shadow stub hits per frame from a checkpoint. |
| `tools/ioscan.py`, `cmpscan*.py`, `disa.py`, `disov.py` | The static scan for shadow store sites vs. the emulator's ground truth (needs `SHADOWLOG` output and RAM dumps under `out/`, run from `host/`; needs capstone). `scantest*/main.cpp` build the loader's scanner on the host. |
| `tools/rtssum.py OUTDIR` | Differing registers across a suite, grouped. |
| `tools/sndcmp.py`, `findexch.py` | Sound comparison; locate the SDK sound driver's channel records. |

dshost options: `--frames N --key F:KEYS:LEN --touch F:X:Y:LEN --shot F:file.ppm --peek F:ADDR:WORDS
--dump F:ADDR:LEN:file --status N --rts-test S:G --rts-out DIR`, plus `LOAD_STATE`/`SAVE_STATE`
(melonDS snapshots) and `SDIMG_ONLY=1` (use the image, don't resync from the folder).

Debug variables (dshost / patched melonDS):

| Variable | Effect |
|---|---|
| `MELON_TRACE_MEM=start[:end]` | Log every write to that range with the PC (both CPUs, DSi map). |
| `MELON_TRACE_IO=addr` | Log writes to one IO register. |
| `MELON_TRACE_DPERM=1` | Log ARM9 data-permission (PU) changes. |
| `PC7_RING=frame` / `PC9_RING=frame` | Arm a 4096-entry PC history at that frame; printed at exit. |
| `IO_DUMP=1` | At exit: IME/IE/IF, SCFG, MBK, WRAMCNT for both CPUs. |
| `ABORT_RING=1` | List the last data/prefetch aborts. |
| `IOPROF=file IOPROF_FROM=frame` | Per-register IO write profile. |
| `SHADOWLOG=file`, `SHADOW7LOG` | Ground-truth log of stores to the registers the shadow keeps. |
| `RTS_WATCH`, `RTS_AFTER`, `RTS_SCRAMBLE`, `CHPOS`, `CENSUS_STATS` | Finer test controls; see `rtstest.h` / `dshost.cpp`. |

## How the RTS works (where to look)

- **Entry.** The exit-combo patch hooks the game's ARM7 IRQ handler
  (`arm9/source/patches/arm7/ExitComboArm7PatchCode.s`). On L+R+Up+Select it signals the ARM9
  (`patches/arm9/ExitComboPatchCode.s`), which loads `/_pico/rtsmenu9.bin` to `RTS_BASE`
  (0x02100000 region, after stashing what's there to `/_pico/rtsstash.bin`) and runs it. The ARM7
  parks in a wait loop meanwhile. For TWL SDK games the IRQ dispatcher is different and has its
  own tail hook.
- **Menu and states.** `rtsmenu/` (C, mostly Thumb for size): `menu.c` UI, `state.c` save/load,
  `check.c` the built-in restore test and load check, `census.c` the measurement build.
  States are `/_pico/states/<code>-<slot>.rts` (DS mode) or `<code>-<slot>-dsi.rts` (DSi mode, about
  18 MB), format v6 in `rtsmenu/state.h`, with a zero map so empty memory isn't written.
  `rtsmenu/tools/rts_inspect.py` reads them (not yet v6/DSi).
- **Register shadow ("full" mode).** Many display/sound registers are write-only, so a state can't
  read them back. At boot the loader scans the game's ARM9 code, ITCM and each overlay as it loads
  (`arm9/source/shadow/ShadowScan.cpp`, `ShadowInstall.cpp`) for stores to those registers and
  patches them into stubs (`ShadowPatchCode.s`, ARM7 sound side `ShadowPatchCode7.s`) that also
  record the value in shadow memory borrowed from the ARM7's main-memory arena. On load the
  menu writes the shadow back. "Basic" mode skips all of it (per game, `/_pico/states/<code>.cfg`,
  `mode=full|basic`; the Mode menu item).
- **Exit.** "Exit to launcher" parks both CPUs and reboots Pico Loader into `/launcher.nds` via
  `launch.txt` (`arm7/source/loader/NdsLoader.cpp` has the reboot/load side).
- **Census** (`RTS_CENSUS=1` build): traps every IO write with the PU to measure which registers a
  game writes; that's how the shadow's register set was chosen.

### Gotchas found the hard way

- DSi BIOS keeps its exception stack in a region the census must not map (`fix_cp15`).
- The TWL SDK's OS init zeroes 0x02FFF684 (the exit block's path magic), so the ARM7 checks a path
  checksum instead.
- In DSi mode the TWL SDK puts DTCM at 0x02FE0000, over main RAM; save/load and the check treat
  that range specially (`dtcmOverMain`).
- Retail DSi-mode games clear SCFG_EXT7 bit 31 at boot, which locks the ARM7 out of SCFG/MBK; the
  loader can't set the console up again after the exit reboot. `Arm7Patcher.cpp` (`keepScfgOpen`)
  NOPs that lock while the exit combo is in.
- MBK9 (new-WRAM slot write-protect) is 0x04004060, not 0x04004058 (that's MBK7).
- Golden Sun DD packs its overlays with its own LZ40; they're unpacked before scanning.
- TWL SDK games running in DS mode use the 3-word autoload list format.
- dshost's SD image must be bigger than the ROM; `dsrun.sh` sizes it from the folder now. A
  256 MB ROM in a too-small image shows as "Failed to open the rom file".

## Open items

1. **DSi mode, step 2: the register shadow.** DSi-mode games only get basic states. The scan and
   stubs need the DSi memory map (the bigger RAM, new WRAM, arm9i/arm7i binaries).
2. **Metroid Prime Hunters:** about 24-28 frames with a wrong screen mode after loading a state saved
   at certain moments. Present in older builds too; it depends on the save frame.
3. **WarioWare DIY** has no menu (NAND save; the combo is left out on purpose).
4. **`rts_inspect.py`** doesn't read v6 / DSi-mode states.
5. **Lights set once per scene through 3D commands** (DQ9 flying) can't be read back or shadowed.
6. An upstream-worthy fix: the 3-word autoload list for TWL SDK games in DS mode.
7. Wider coverage: the main reason for this handover. Add games to a list, find a save frame in
   gameplay, run `rtsall.sh` in DS mode and (for DSi-enhanced games) `dsi` mode, and look at any
   game with more than a few differing screens or a FAILED result.
