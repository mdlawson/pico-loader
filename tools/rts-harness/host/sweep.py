#!/usr/bin/env python3
"""sweep.py - the RTS restore test at many save points across a library, from a fresh boot.

  sweep.py run LIBRARY OUTDIR [--frames 1500,2500,...] [--mode ds|dsi] [--only code,...]
  sweep.py report OUTDIR          summary.tsv, contact sheets and the triage list
  sweep.py profile LIBRARY OUTDIR [--frames ...] [--only ...] [--mode ds|dsi]
                                  the shadow's cost: plays each game with the generic
                                  input to each frame and counts the stores to shadowed
                                  registers (each one a stub run) over the 600 frames
                                  before it (SHADOWLOG); ranking.tsv, most first
  sweep.py autosd LIBRARY SDDIR [--frames ...] [--only ...] [--pico DIR] [--card-roms DIR] [--ab]
                                  an sd card folder for the autopilot (real hardware or
                                  dsrun.sh): launcher, the autopilot _pico set (make_pico.sh
                                  with RTS_AUTOPILOT=1, default ~/emu/rts_pico_auto), the
                                  roms in /roms and /_pico/autolist.txt, one run per game
                                  and save frame; --card-roms /Games uses the roms already
                                  under that folder of the card (matched by game code);
                                  --ab: each run twice, full then basic, to time the shadow

LIBRARY is mklib.py's list (code|rom|unit|title). Every game gets the same generic input
(generic_input below): A taps, centre-screen touches and START presses to get through title
screens, menus and dialogue, then d-pad walks, all deterministic, so the save points of one
game are points along one and the same play-through. The input stops before each save point
(the restore test drives the keys itself from there; touches it doesn't, so they must end).
Each (game, save frame) is one rtsall.sh run named code@frame, cleaned up after (RTS_CLEAN),
without a game save (fresh boot each time). --mode dsi runs only the dsi-enhanced/dsi-only
games, as a DSi.

Per game override: lists/sweep_input.txt, lines `code|extra dshost args` added before the
generic input (e.g. a touch on a game's own start button), and `code|-` to leave the
generic input out for that game.

report writes OUTDIR/summary.tsv (code, frame, result, screens differing, title),
OUTDIR/sheets/<code>.png (the screen at each save point, top over bottom, in frame order,
with the differing count drawn as a bar under each: green 0-3, amber 4-10, red more or
failed), OUTDIR/sheets/index.png (every game's row, small) and OUTDIR/triage.txt.
"""
import os, sys, subprocess, struct, zlib, re, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FRAMES = [1500, 2500, 3500, 4500, 6000, 7500, 9000]


def generic_input(save_frame, start=300):
    """Key and touch presses up to 40 frames before save_frame."""
    end = save_frame - 40
    args = []
    f = start
    while f + 3 < end:
        args += ["--key", f"{f}:A:3"]
        f += 40
    f = start + 20
    while f + 3 < end:
        args += ["--touch", f"{f}:128:96:3"]
        f += 80
    f = start + 10
    while f + 3 < min(end, 3500):
        args += ["--key", f"{f}:START:3"]
        f += 200
    # from 2500: walk, 50 frames a direction every 120
    dirs = ["RIGHT", "DOWN", "LEFT", "UP"]
    f, i = 2500 + 25, 0
    while f + 50 < end:
        args += ["--key", f"{f}:{dirs[i % 4]}:50"]
        f += 120
        i += 1
    return args


def read_lib(path):
    games = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        code, rom, unit, title = line.split("|", 3)
        games.append((code, rom, unit, title))
    return games


def read_overrides():
    p = os.path.join(HERE, "lists", "sweep_input.txt")
    o = {}
    if os.path.exists(p):
        for line in open(p):
            line = line.strip()
            if line and not line.startswith("#") and "|" in line:
                c, a = line.split("|", 1)
                o[c] = a.strip()
    return o


def cmd_run(argv):
    lib, out = argv[0], argv[1]
    frames, mode, only = DEFAULT_FRAMES, "ds", None
    i = 2
    while i < len(argv):
        if argv[i] == "--frames": frames = [int(x) for x in argv[i + 1].split(",")]; i += 2
        elif argv[i] == "--mode": mode = argv[i + 1]; i += 2
        elif argv[i] == "--only": only = set(argv[i + 1].split(",")); i += 2
        else: sys.exit(f"unknown argument {argv[i]}")
    games = read_lib(lib)
    if mode == "dsi":
        games = [g for g in games if g[2].startswith("dsi")]
    if only:
        games = [g for g in games if g[0] in only]
    over = read_overrides()
    os.makedirs(out, exist_ok=True)
    lst = os.path.join(out, "list.txt")
    with open(lst, "w") as w:
        # longest runs first, so the parallel tail is short
        for fr in sorted(frames, reverse=True):
            for code, rom, unit, title in games:
                o = over.get(code, "")
                extra = ([] if o == "-" else generic_input(fr)) + (o.split() if o and o != "-" else [])
                w.write(f"{code}@{fr}|{rom}|{fr}:300|{' '.join(extra)}\n")
    env = dict(os.environ, RTS_CLEAN="1")
    print(f"{len(games)} games x {len(frames)} save points, {mode} mode -> {out}", flush=True)
    subprocess.run([os.path.join(HERE, "rtsall.sh"), lst, out, mode], env=env, check=False)
    cmd_report([out])


# ---------------------------------------------------------------- report

def read_ppm(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    c = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + c(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + c(b"IDAT", zlib.compress(raw, 6)) + c(b"IEND", b""))


def shrink(w, h, px, f):
    """Every f-th pixel (nearest), as rows of bytes."""
    nw, nh = w // f, h // f
    rows = []
    for y in range(nh):
        row = px[(y * f) * w * 3:(y * f + 1) * w * 3]
        rows.append(b"".join(row[x * f * 3:x * f * 3 + 3] for x in range(nw)))
    return nw, nh, rows


def parse_report(p):
    r = {"result": "no report", "screens": None}
    if not os.path.exists(p):
        return r
    t = open(p, errors="replace").read()
    m = re.search(r"^result: (.*)$", t, re.M)
    if m: r["result"] = m.group(1).strip()
    m = re.search(r"^screens differing: (\d+)", t, re.M)
    if m: r["screens"] = int(m.group(1))
    return r


def color(r):
    s = r["screens"]
    if s is None or r["result"].startswith("FAILED") or r["result"] == "no report":
        return (220, 40, 40)
    return (40, 180, 70) if s <= 3 else (230, 160, 30) if s <= 10 else (220, 40, 40)


# no exit combo (the loader leaves it out: NAND saves), so an autopilot run would never end
NO_EXIT = {"uore", "uorp", "uorj", "uxbp", "uskv", "ugda"}
# basic by default in the loader (NdsLoader.cpp rtsDefaultBasic): no forced full run
BASIC_ONLY = {"adme", "admp", "admj", "admk"}


def cmd_autosd(argv):
    lib, sd = argv[0], argv[1]
    frames, only, pico = [1500, 4500, 9000], None, os.path.expanduser("~/emu/rts_pico_auto")
    cardRoms = None
    ab = False
    i = 2
    while i < len(argv):
        if argv[i] == "--frames": frames = [int(x) for x in argv[i + 1].split(",")]; i += 2
        elif argv[i] == "--only": only = set(argv[i + 1].split(",")); i += 2
        elif argv[i] == "--pico": pico = argv[i + 1]; i += 2
        elif argv[i] == "--card-roms": cardRoms = argv[i + 1]; i += 2
        elif argv[i] == "--ab": ab = True; i += 1
        else: sys.exit(f"unknown argument {argv[i]}")
    roms = os.environ.get("ROMS", os.path.expanduser("~/emu/roms"))
    # roms already on the card: game code -> path on the card
    onCard = {}
    if cardRoms:
        for root, _, files in os.walk(os.path.join(sd, cardRoms.lstrip("/"))):
            for f in sorted(files):
                if f.lower().endswith(".nds") and not f.startswith("._"):
                    full = os.path.join(root, f)
                    with open(full, "rb") as fh:
                        code = fh.read(0x10)[0xC:0x10].decode("ascii", "replace").lower()
                    onCard.setdefault(code, "/" + os.path.relpath(full, sd).replace(os.sep, "/"))
    games = [g for g in read_lib(lib) if (not only or g[0] in only) and g[0] not in NO_EXIT]
    os.makedirs(os.path.join(sd, "_pico"), exist_ok=True)
    for f in os.listdir(pico):
        shutil.copyfile(os.path.join(pico, f), os.path.join(sd, "_pico", f))
    shutil.copyfile(os.path.join(os.path.dirname(HERE), "launcher", "launcher.nds"), os.path.join(sd, "launcher.nds"))
    lines = []
    copied = 0
    for code, rom, unit, title in games:
        path = onCard.get(code)
        if not path:
            os.makedirs(os.path.join(sd, "roms"), exist_ok=True)
            path = f"/roms/{code}.nds"
            dest = os.path.join(sd, "roms", code + ".nds")
            if not os.path.exists(dest):
                shutil.copyfile(os.path.join(roms, rom), dest)
                copied += 1
        for fr in frames:
            if ab:
                if code not in BASIC_ONLY:
                    lines.append(f"{path}|{fr}|300|full")
                lines.append(f"{path}|{fr}|300|basic")
            else:
                lines.append(f"{path}|{fr}|300")
    with open(os.path.join(sd, "_pico", "autolist.txt"), "w") as w:
        w.write("# rom|save frame|gap frames[|full|basic]: one restore test run a line (see HANDOVER.md, Autopilot)\n")
        w.write("\n".join(lines) + "\n")
    for f in ("autostate.txt", "autoresults.txt", "autotest.txt"):
        p = os.path.join(sd, "_pico", f)
        if os.path.exists(p):
            os.remove(p)
    print(f"{len(lines)} runs ({len(games)} games x {len(frames)} save frames) in {sd}; "
          f"{len(games) - sum(1 for g in games if g[0] not in onCard)} roms already on the card, {copied} copied")


def cmd_profile(argv):
    lib, out = argv[0], argv[1]
    frames, only, mode = DEFAULT_FRAMES, None, "ds"
    i = 2
    while i < len(argv):
        if argv[i] == "--frames": frames = [int(x) for x in argv[i + 1].split(",")]; i += 2
        elif argv[i] == "--only": only = set(argv[i + 1].split(",")); i += 2
        elif argv[i] == "--mode": mode = argv[i + 1]; i += 2
        else: sys.exit(f"unknown argument {argv[i]}")
    games = [g for g in read_lib(lib) if not only or g[0] in only]
    os.makedirs(out, exist_ok=True)
    jobs = os.path.join(out, "jobs.txt")
    runner = "dsirun.sh" if mode == "dsi" else "dsrun.sh"
    with open(jobs, "w") as w:
        for fr in sorted(frames, reverse=True):
            for code, rom, unit, title in games:
                w.write(f"{code} {fr}\n")
    # one sweeprepro run each: the generic input to the frame, the log over the last 600
    script = (f'c=$0; f=$1; d={out}/$c@$f; mkdir -p $d; '
              f'SHADOWLOG=$d/shadow.log SHADOWLOG_FROM=$((f - 600)) SHADOW7LOG=$d/shadow7.log '
              f'RUNNER={runner} {HERE}/sweeprepro.sh $c $f > $d/stderr.txt 2>&1; '
              f'rm -rf $SDROOT/repro_$c $OUTDIR/repro_$c.img*; echo "$c@$f: $(cat $d/shadow.log.sum 2>/dev/null)"')
    env = dict(os.environ)
    env.setdefault("SDROOT", os.path.expanduser("~/emu/rtssd"))
    env.setdefault("OUTDIR", os.path.expanduser("~/emu/out"))
    print(f"{len(games)} games x {len(frames)} points, {mode} mode -> {out}", flush=True)
    nproc = os.cpu_count() or 4
    with open(jobs) as j:
        subprocess.run(["xargs", "-P", str(nproc), "-n", "2", "bash", "-c", script], stdin=j, env=env)
    profile_report(out)


def profile_report(out):
    titles = {}
    lib = os.path.join(HERE, "lists", "library.txt")
    if os.path.exists(lib):
        for code, rom, unit, title in read_lib(lib):
            titles[code] = title
    rows = []
    for name in os.listdir(out):
        if "@" not in name:
            continue
        p = os.path.join(out, name, "shadow.log.sum")
        if not os.path.exists(p):
            continue
        t = open(p).read()
        m = re.search(r"cpu stores/frame ([\d.]+).*max (\d+).*dma writes/frame ([\d.]+)", t)
        if not m:
            continue
        code, fr = name.split("@")
        rows.append((float(m.group(1)), int(m.group(2)), float(m.group(3)), code, int(fr)))
    # by game: its busiest point
    best = {}
    for r in rows:
        if r[3] not in best or r[1] > best[r[3]][1]:
            best[r[3]] = r
    ranked = sorted(best.values(), key=lambda r: (-r[1], -r[0]))
    with open(os.path.join(out, "ranking.tsv"), "w") as w:
        w.write("code\tframe\tstores/frame avg\tmax\tdma/frame\ttitle\n")
        for avg, mx, dma, code, fr in ranked:
            w.write(f"{code}\t{fr}\t{avg:.1f}\t{mx}\t{dma:.1f}\t{titles.get(code, '')}\n")
    print(f"{len(rows)} profiles, {len(ranked)} games: {out}/ranking.tsv")


def cmd_report(argv):
    out = argv[0]
    titles = {}
    lib = os.path.join(HERE, "lists", "library.txt")
    if os.path.exists(lib):
        for code, rom, unit, title in read_lib(lib):
            titles[code] = title
    runs = {}
    for name in os.listdir(out):
        if "@" in name and os.path.isdir(os.path.join(out, name)):
            code, fr = name.split("@")
            runs.setdefault(code, []).append(int(fr))
    sheets = os.path.join(out, "sheets")
    os.makedirs(sheets, exist_ok=True)
    rows_tsv, triage, index_rows = [], [], []
    for code in sorted(runs):
        frs = sorted(runs[code])
        tiles = []
        for fr in frs:
            d = os.path.join(out, f"{code}@{fr}")
            r = parse_report(os.path.join(d, "report.txt"))
            rows_tsv.append(f"{code}\t{fr}\t{r['result']}\t{'' if r['screens'] is None else r['screens']}\t{titles.get(code, '')}")
            if r["result"].startswith("FAILED") or r["result"] == "no report" or (r["screens"] or 0) > 3:
                triage.append(f"{code}@{fr}\t{r['result'][:60]}\tscreens {r['screens']}\t{titles.get(code, '')}")
            shot = os.path.join(d, "at_save.ppm")
            if os.path.exists(shot):
                w, h, px = read_ppm(shot)
                tw, th, rows = shrink(w, h, px, 2)
            else:
                tw, th, rows = 128, 192, [bytes(128 * 3)] * 192
            bar = bytes(color(r)) * tw
            tiles.append((tw, rows + [bar] * 8))
        # one sheet: tiles side by side, 4 px apart
        th = max(len(t[1]) for t in tiles)
        gap = bytes((255, 255, 255)) * 4
        sheet_rows = [gap.join(t[1][y] if y < len(t[1]) else bytes(t[0] * 3) for t in tiles) for y in range(th)]
        sw = len(sheet_rows[0]) // 3
        write_png(os.path.join(sheets, f"{code}.png"), sw, th, b"".join(sheet_rows))
        # index: the same row at half that size again
        nw, nh, small = shrink(sw, th, b"".join(sheet_rows), 2)
        index_rows.append((nw, small))
    with open(os.path.join(out, "summary.tsv"), "w") as w:
        w.write("code\tframe\tresult\tscreens\ttitle\n" + "\n".join(rows_tsv) + "\n")
    with open(os.path.join(out, "triage.txt"), "w") as w:
        w.write("\n".join(triage) + "\n")
    if index_rows:
        iw = max(r[0] for r in index_rows)
        lines = []
        for nw, small in index_rows:
            for row in small:
                lines.append(row + bytes((255, 255, 255)) * (iw - nw))
            lines += [bytes((255, 255, 255)) * iw] * 3
        write_png(os.path.join(sheets, "index.png"), iw, len(lines), b"".join(lines))
    n = len(rows_tsv)
    print(f"{n} runs, {len(triage)} to look at: {out}/triage.txt, sheets in {sheets}")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("run", "report", "autosd", "profile"):
        sys.exit(__doc__)
    {"run": cmd_run, "report": cmd_report, "autosd": cmd_autosd, "profile": cmd_profile}[sys.argv[1]](sys.argv[2:])
