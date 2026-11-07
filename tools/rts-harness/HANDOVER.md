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
It copies `data/*.bin` as they are: generate them once with `make apList saveList patchList`
(needs the .NET 9 SDK). The launcher only needs rebuilding if you change it (`make` in `launcher/` with the same
toolchain variables as the loader; it includes `include/picoLoader7.h` from this repo).

**macOS (Apple Silicon)** works too: Wonderful's macOS installer, `brew install cmake dotnet@9`
(keg-only: put `/opt/homebrew/opt/dotnet@9/bin` on PATH for the `make` above). `build_dshost.sh`
configures melonDS without the JIT and OpenGL renderer (dshost uses neither; the JIT faults at
start on macOS and the GL header fails under Apple clang) and compiles dshost with the core's own
defines (`GDBSTUB_ENABLED` changes the `NDS` layout). Set `MELONDS` if the checkout isn't at
`$EMU/melonDS`. dshost clears the RTC's power-lost flag and sets the firmware's autostart in its
in-memory copy, so a dump with manual boot or a lost clock still boots the cart.

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

## Sweeping a whole library (mklib.py, sweep.py)

For breadth without playing: `mklib.py ZIPDIR` unpacks a folder of rom zips into `$ROMS/<code>/`
and writes `lists/library.txt` (game code, rom, ds / dsi-enhanced / dsi-only, title). Then

```bash
./sweep.py run lists/library.txt ~/emu/results/sweep_ds
```

```bash
./sweep.py run lists/library.txt ~/emu/results/sweep_dsi --mode dsi
```

runs the restore test at 7 save frames per game (1500-9000, `--frames` to change, `--only
code,...` for a subset), every game from a fresh boot (no game save) with the same generic input:
A every 40 frames, a centre-screen touch every 80, START every 200 until frame 3500, then d-pad
walks. That gets most games through their title screens into menus, dialogue or play; the save
points of a game lie along one deterministic play-through. Input ends 40 frames before each save
point (the test drives the keys from there, and touches during the compare would differ).
`lists/sweep_input.txt` adds per-game presses (`code|--touch 1600:128:150:3`) or turns the generic
input off (`code|-`). Runs clean up after themselves (`RTS_CLEAN=1`: sd image and folder, ram
dumps, sound), so a 700-run sweep needs little disk; about 40 minutes on 12 cores.

`sweep.py report OUTDIR` (run at the end of `run`) writes `summary.tsv`, `triage.txt` (failed or
more than 3 screens differing) and `sheets/<code>.png`: the screen at each save point with a bar
under it (green 0-3 screens differing, amber 4-10, red more or failed). Look at the sheets to see
which save points are real play and which are still a title screen; `sheets/index.png` has every
game at a glance.

`rtsall.sh` now deletes the game's `game.sav` from its SD folder before each run, so an earlier run's
in-game save doesn't change how the game boots (`KEEP_SAV=1` keeps it, `SAV=file` starts from one).

`sweeprepro.sh CODE FRAMES [args]` boots one library game the way the sweep does but without the
restore test, so the exit-time debug output works (`ABORT_RING`, `PC9_RING` with `PC_RING_N=4096`
for the whole ring, `IO_DUMP`). `--peek` reads the arm9's tcms too.

### First library sweep (2026-11-07, 100 roms, 700 DS + 42 DSi runs)

Fixed from it: Mario Kart DS (a thumb shadow stub in a tail-calling routine that never pushed lr:
crash in play), Tetris DS (the FS_StartOverlay hook's sinit register on the sdk 4 path), Animal
Crossing: Wild World (its arm9 uses the arm7's arena, where the shadow goes: basic mode by
default), Mario & Luigi: Bowser's Inside Story (vblank irq off in play, frames by vcount: the hook
takes the combo on a vcount then, through the optional vcount piece), Retro Game Challenge and
Trauma Center 2 (the shadow wrote stale BLDCNT/BLDALPHA over the dump's), Advance Wars (a load lost
the timer irq pending at the freeze). After: 636 of 700 DS runs at 0-3 screens, failures only
WarioWare D.I.Y. (on purpose); DSi mode unchanged (38 of 42).

Still open (`~/emu/results/sweep3_ds/triage.txt` has the list):
- 3D matrix stacks and the projection aren't restored: 3D games show ~11 wrong frames after a load
  (Moon, Nostalgia, DQ9 at some points). Clears by itself.
- BG2/BG3 affine reference points sometimes come back 0 (written outside the stubs?); invisible
  where seen so far, but worth a look.
- A save during a DMA to the geometry fifo (Dragon Quest Monsters: Joker at some points): the game
  goes another way after the load (GX fifo irq on, different DMA3).
- Ninjatown, Planet Puzzle League, Elite Beat Agents (late points), Shin Megami Tensei: Strange
  Journey (late points): go their own way after a load at some points; not looked into.
- Animal Crossing in basic mode: two points at 119 (no shadow there, so scroll/affine registers
  can't come back).
- DSi mode: Solatorobo and Pokemon White 2 at some points (basic mode only in DSi mode).
- Measured timer reloads can be one off (tm9.1 in Trauma Center 2): the wrap is read a tick late.

## Autopilot: the restore test on real hardware, unattended

A separate build (`RTS_AUTOPILOT=1`; release builds have none of it) runs the same kind of test
as the sweep on a real DSpico, one boot per run, with no one pressing anything:

- The launcher (`launcher/`, at `/launcher.nds`) works through `/_pico/autolist.txt`, lines
  `rom|saveFrame|gapFrames`. Before each boot it blanks `/_pico/rtslog.txt` and writes
  `/_pico/autotest.txt`, which the loader reads once and deletes; after each run it appends the
  result to `/_pico/autoresults.txt` (`index|rom|frame|gap|memory N|registers N|run N`, the load
  check's and the comparison's counts, as the menu reports them) and boots the next. Where it is
  goes in `/_pico/autostate.txt` (delete it to start again). A run that never came back (a crash,
  then a power cycle and the launcher started again) is recorded as `no result`.
- The loader points the game's KEYINPUT literal pool words (0x04000130, read pc-relative, in the
  arm9 and its autoloads) at a word in the shared page (`rts_autopilot_t`, block + 0x170).
  97 of the 100 library games have such literals (not: Professor Layton 2-4; overlays aren't done).
- The arm7 hook (ExitComboArm7PatchCode.s, `autopilotTick`) writes that word every vblank: live
  keys with the script's presses (A every 40 frames from 300, START 10 frames after every fifth A
  until 3500, from 2540 a d-pad direction 50 frames in 120; nothing in the last 40 frames), and
  freezes the game at saveFrame (counted from the game's start, not from power on as in dshost).
  No touch yet.
- The menu (`RTS_AUTOPILOT` in rtsmenu) skips its list: save + reference at that freeze, the gap,
  load + compare (no A to confirm), the comparison, then exit to the launcher.

Build and an sd card folder:

```bash
RTS_AUTOPILOT=1 ./make_pico.sh ~/emu/rts_pico_auto
```

```bash
./sweep.py autosd lists/library.txt /Volumes/SDCARD --frames 1500,4500,9000
```

(`--only code,...` for a subset; the roms go to `/roms/<code>.nds`.) In melonDS the same folder
runs with `./dsrun.sh FOLDER 30000` (the launcher cycles by itself; results land in the folder).

On the DS: use an sd card of its own (the autopilot `_pico` files replace the normal ones), start
`/launcher.nds` from your launcher, and leave it. After a crash or hang, power-cycle and start
`/launcher.nds` again: it records the dead run and goes on. Unattended crash recovery needs a
relay on the power switch and a launcher that starts `/launcher.nds` at power on.

The arm7 code is tight in the autopilot build: 3356 bytes, exactly what Mario Kart DS's arm7 patch
space had left; all 99 games with the exit combo still took it (checked by booting each).

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
