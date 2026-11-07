#!/usr/bin/env python3
"""mklib.py ZIPDIR [LIST] - unpacks a folder of No-Intro style .zip (or .nds) DS roms into
$ROMS/<code>/<name>.nds (code = the header's game code, lower case) and writes a library list
for sweep.py: one line per rom, `code|rom path relative to $ROMS|unit|title`, unit being
ds, dsi-enhanced or dsi-only (header byte 0x12). Patched or duplicate dumps of a code already
seen are skipped. Default LIST: lists/library.txt next to this script.
"""
import os, sys, zipfile, shutil

here = os.path.dirname(os.path.abspath(__file__))
roms = os.environ.get("ROMS", os.path.expanduser("~/emu/roms"))
src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "lists", "library.txt")
UNITS = {0: "ds", 2: "dsi-enhanced", 3: "dsi-only"}

lines, seen = [], {}
for f in sorted(os.listdir(src)):
    if f.startswith(".") or "(patched" in f:
        continue
    p = os.path.join(src, f)
    if f.lower().endswith(".zip"):
        with zipfile.ZipFile(p) as z:
            inner = [n for n in z.namelist() if n.lower().endswith(".nds")]
            if not inner:
                continue
            with z.open(inner[0]) as r:
                head = r.read(0x200)
            name = os.path.basename(inner[0])
            code = head[0x0C:0x10].decode("ascii", "replace").lower()
            if code in seen:
                print(f"skip {f}: code {code} already from {seen[code]}")
                continue
            dest = os.path.join(roms, code, name)
            if not os.path.exists(dest):
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with z.open(inner[0]) as r, open(dest + ".part", "wb") as w:
                    shutil.copyfileobj(r, w, 1 << 20)
                os.rename(dest + ".part", dest)
    elif f.lower().endswith(".nds"):
        head = open(p, "rb").read(0x200)
        name = f
        code = head[0x0C:0x10].decode("ascii", "replace").lower()
        dest = os.path.join(roms, code, name)
        if not os.path.exists(dest):
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(p, dest)
    else:
        continue
    seen[code] = f
    unit = UNITS.get(head[0x12], f"unit{head[0x12]}")
    title = os.path.splitext(name)[0]
    lines.append(f"{code}|{code}/{name}|{unit}|{title}")
    print(f"{code} {unit:13} {title}")

with open(out, "w") as w:
    w.write("\n".join(lines) + "\n")
print(f"{len(lines)} roms -> {out}")
