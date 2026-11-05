"""Prints what a Pico Loader RTS save state file holds (layout: rtsmenu/state.h).
Usage: python3 rts_inspect.py /path/to/_pico/states/XXXX-1.rts"""
import struct, sys, zlib

SECTIONS = [("arm9 io", 0x200, 0x1100), ("arm7 context", 0x1400, 0x200), ("itcm", 0x4000, 0x8000), ("dtcm", 0xC000, 0x4000),
            ("palettes", 0x10000, 0x800), ("oam", 0x10800, 0x800), ("shared wram", 0x11000, 0x8000),
            ("arm7 wram", 0x19000, 0x10000)]
off = 0x29000
for name, size in zip("ABCDEFGHI", (0x20000,) * 4 + (0x10000, 0x4000, 0x4000, 0x8000, 0x4000)):
    SECTIONS.append(("vram " + name, off, size)); off += size
SECTIONS.append(("main ram", 0x100000, 0x400000))

data = open(sys.argv[1], "rb").read()
print("file size", hex(len(data)))
magic, version, hsize, code = struct.unpack_from("<4I", data, 0)
if magic != 0x53545250:
    sys.exit("no valid state in this file (magic %08x): never saved, or the save did not finish" % magic)
title = data[16:28].rstrip(b"\0").decode("ascii", "replace")
words = struct.unpack_from("<%dI" % ((hsize - 28) // 4), data, 28)
print("version", version, "| game", title, struct.pack("<I", code).decode("ascii", "replace"), "| block %08x" % words[0])
w = words[1:]
print("arm9 at the freeze: wait entry %08x  irq lr %08x  irq sp %08x  spsr %08x" % (w[0], w[9], w[10], w[11]))
print("  r4-r11", " ".join("%08x" % x for x in w[1:9]))
print("  svc sp/lr/spsr %08x %08x %08x  sys sp/lr %08x %08x" % (w[12], w[13], w[14], w[21], w[22]))
cp = w[31:47]
print("cp15: control %08x  dtcm %08x  itcm %08x  dcache %02x icache %02x wbuf %02x" % cp[:6])
print("  regions", " ".join("%08x" % x for x in cp[8:16]))
vram = data[28 + 47 * 4 + 4:28 + 47 * 4 + 14]
print("VRAMCNT A-I (WRAMCNT 8th):", vram.hex(" "))
if version >= 2:
    c = struct.unpack_from("<64I", data, 0x1400)
    if c[0] == 0x58433741:
        print("arm7 at the freeze: irq sp %08x spsr %08x  svc sp %08x  sys sp %08x  IE %08x  IME %d  stub at %08x"
              % (c[2], c[1], c[4], c[12], c[22], c[23], c[57]))
    else:
        print("arm7 context missing (magic %08x)" % c[0])
if version >= 5 and struct.unpack_from("<I", data, hsize - 4)[0] == 1:
    # the zero map: 2 KB blocks left unwritten because they were all zero
    zmap = data[0x1A00:0x1C00]
    blocks = [b for b in range(len(zmap) * 8) if zmap[b >> 3] >> (b & 7) & 1]
    buf = bytearray(data)
    for b in blocks:
        buf[b * 0x800:(b + 1) * 0x800] = bytes(0x800)
    data = bytes(buf)
    print("zero map: %d blocks of 2 KB were all zero and not written" % len(blocks))
print()
for name, start, size in SECTIONS:
    part = data[start:start + size]
    nonzero = sum(1 for i in range(0, size, 512) if part[i:i + 512].strip(b"\0"))
    print("%-12s %7d bytes  %5.1f%% of sectors non-zero  crc %08x" % (name, size, 100 * nonzero / (size // 512), zlib.crc32(part)))
