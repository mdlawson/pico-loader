#!/usr/bin/env python3
"""sweep.py - the RTS restore test at many save points across a library, from a fresh boot.

  sweep.py run LIBRARY OUTDIR [--frames 1500,2500,...] [--mode ds|dsi] [--only code,...]
  sweep.py report OUTDIR          summary.tsv, contact sheets and the triage list

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
import os, sys, subprocess, struct, zlib, re

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
    if len(sys.argv) < 3 or sys.argv[1] not in ("run", "report"):
        sys.exit(__doc__)
    {"run": cmd_run, "report": cmd_report}[sys.argv[1]](sys.argv[2:])
