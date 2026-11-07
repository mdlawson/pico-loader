#!/usr/bin/env python3
# rtssum.py OUTDIR: the registers that differ, grouped, per game
import sys, os, re, collections
out = sys.argv[1]
def group(n):
    for pre, g in [("2d", None), ("3d.TOON", "3d settings"), ("3d.EDGE", "3d settings"), ("3d.FOG", "3d settings"),
                   ("3d.CLEAR", "3d settings"), ("3d.ALPHA", "3d settings"), ("3d.DISP3DCNT", "3d settings"),
                   ("3d.", "3d geometry"), ("tm9", "arm9 timers"), ("tm7", "arm7 timers"), ("snd", "sound"),
                   ("dma", "dma"), ("IE", "irq"), ("IME", "irq"), ("VRAMCNT", "vram"), ("MASTER", "2d"), ("DISP", "2d")]:
        if n.startswith(pre):
            if g: return g
            m = re.match(r"2d[AB]\.([A-Z]+)", n)
            r = m.group(1) if m else n
            return {"BGHOFS": "2d scroll", "BGVOFS": "2d scroll", "BGX": "2d affine", "BGY": "2d affine", "BGPA": "2d affine",
                    "BGPB": "2d affine", "BGPC": "2d affine", "BGPD": "2d affine", "WIN": "2d windows", "WINCNT": "2d windows",
                    "MOSAIC": "2d mosaic", "BLDALPHA": "2d blend", "BLDY": "2d blend", "BLDCNT": "2d blend"}.get(r, "2d other")
    return "other"
games = sorted(d for d in os.listdir(out) if os.path.isfile(os.path.join(out, d, "report.txt")))
for g in games:
    t = open(os.path.join(out, g, "report.txt")).read()
    scr = re.search(r"screens differing: (\d+)", t)
    res = re.search(r"result: (.*)", t)
    groups = collections.defaultdict(list)
    for m in re.finditer(r"^  (\S+(?: \S+)?)\s+(\d+) frames, first at\s+(\d+)", t, re.M):
        name, n, first = m.group(1), int(m.group(2)), int(m.group(3))
        groups[group(name)].append((name, n, first))
    print(f"{g}: screens {scr.group(1) if scr else '?'}/120, {res.group(1) if res else '?'}")
    for k in sorted(groups):
        v = groups[k]
        persist = max(n for _, n, _ in v)
        ex = ", ".join(f"{nm}({n})" for nm, n, _ in sorted(v, key=lambda x: -x[1])[:4])
        print(f"   {k:13s} {len(v):3d} fields, up to {persist:3d} frames: {ex}")
