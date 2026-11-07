#!/usr/bin/env python3
# findexch.py DIR...: look for the SDK sound driver's 16 channel records (first
# byte = channel number 0..15 at a fixed stride) in main ram and arm7 wram, and
# for the fields that hold the hardware's SAD / TMR / PNT / LEN of the channels
# that are on.
import sys, struct
def regions(ram):
    return [(0x02000000, ram[:0x400000]), (0x03800000, ram[0x400000:0x410000]), (0x03000000, ram[0x410000:0x418000])]
for d in sys.argv[1:]:
    ram = open(d + "/ram_ref.bin", "rb").read()
    regs = dict(l.rsplit(None, 1) for l in open(d + "/regs_ref.txt"))
    ch = {i: {k: int(regs[f"snd{i}.{k}"], 16) for k in ("CNT", "SAD", "TMR", "PNT", "LEN")} for i in range(16)}
    found = []
    for base, mem in regions(ram):
        for stride in range(0x40, 0x200, 4):
            span = stride * 15
            for o in range(0, len(mem) - span - 4, 4):
                if mem[o] != 0 or mem[o + stride] != 1: continue
                if all(mem[o + i * stride] == i for i in range(16)):
                    found.append((base + o, stride, base, o))
    print(d.rstrip('/').split('/')[-1], "candidates:", [(hex(a), hex(s)) for a, s, _, _ in found][:4])
    for a, stride, base, o in [x for x in found if x[1] == 0x54 and x[0] >= 0x03800000][:1]:
        mem = dict((b, m) for b, m in regions(ram))[base]
        # which word offsets equal the hardware SAD (as an address) for the channels that are on
        on = [i for i in range(16) if ch[i]["CNT"] & 0x80000000 and ((ch[i]["CNT"] >> 29) & 3) != 3]
        hits = {}
        for k in ("SAD", "TMR", "PNT", "LEN"):
            for off in range(0, stride - 1, 2):
                ok = 0
                for i in on:
                    rec = o + i * stride
                    w32 = struct.unpack_from("<I", mem, rec + (off & ~3))[0] if off % 4 == 0 else None
                    w16 = struct.unpack_from("<H", mem, rec + off)[0]
                    v = ch[i][k]
                    if k == "SAD" and w32 is not None and (w32 & 0x07FFFFFC) == v: ok += 1
                    elif k == "TMR" and (w16 == v or (0x10000 - w16) & 0xFFFF == v): ok += 1
                    elif k == "PNT" and (w16 == v >> 2 or w16 == v): ok += 1
                    elif k == "LEN" and w32 is not None and (w32 in (v, v >> 2)): ok += 1
                if on and ok == len(on): hits.setdefault(k, []).append(hex(off))
        print(f"   at {a:08X} stride {stride:#x}: {len(on)} channels on; fields matching all of them: {hits}")
