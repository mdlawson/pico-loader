#!/usr/bin/env python3
"""ioscan.py GAME - a static scan for the arm9 stores that write the registers the shadow keeps,
checked against the emulator's ground truth (out/shl/GAME.txt from SHADOWLOG).

Finds IO bases the code puts in a register (a literal pool word loaded pc-relative, or
mov/movs+lsl), follows that register forward through simple arithmetic until a branch or
its redefinition, and reports each store whose address it can work out (or bound) to be a
shadowed register."""
import sys, struct, collections
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone.arm import ARM_OP_REG, ARM_OP_IMM, ARM_OP_MEM

G = sys.argv[1]
ram = open(f'out/dumps/{G}.ram', 'rb').read()
itcm = open(f'out/dumps/{G}.itcm', 'rb').read()

def shadowed(a):
    if 0x04001000 <= a < 0x04002000: a -= 0x1000; b = True
    else: b = False
    o = a - 0x04000000
    return (0x10 <= o < 0x58) or (not b and 0x330 <= o < 0x3C0)

def rd32(a):
    if 0x02000000 <= a < 0x02400000 - 3: return struct.unpack_from('<I', ram, a - 0x02000000)[0]
    if 0x01FF8000 <= a < 0x02000000 - 3: return struct.unpack_from('<I', itcm, a - 0x01FF8000)[0]
    return None

regions = [(0x01FF8000, itcm), (0x02000000, ram)]
arm = Cs(CS_ARCH_ARM, CS_MODE_ARM); arm.detail = True
thb = Cs(CS_ARCH_ARM, CS_MODE_THUMB); thb.detail = True

def isIoBase(v):
    return v is not None and 0x04000000 <= v < 0x04002000

found = {}  # site(|1 thumb) -> set of addrs (None: unknown in range)

from capstone.arm import ARM_REG_R0, ARM_REG_R12, ARM_REG_PC, ARM_REG_LR
from capstone import CS_GRP_JUMP, CS_GRP_CALL, CS_GRP_RET
ARG = set(range(ARM_REG_R0, ARM_REG_R0 + 4)) | {ARM_REG_R12}
seen_calls = set()
def region_of(a):
    for base, buf in regions:
        if base <= a < base + len(buf): return base, buf
    return None, None
def cval(v): return v[1] if isinstance(v, tuple) else v
def trace(md, base, buf, start, reg, val, thumb, regs=None, depth=0):
    """from start, follow reg = val (or the regs dict)"""
    regs = dict(regs) if regs else {reg: val}
    pc = start
    n = 0
    end = base + len(buf)
    while n < 256 and pc < end - 4:
        ins = next(md.disasm(buf[pc - base: pc - base + 4], pc, 1), None)
        if ins is None: return
        n += 1
        m = ins.mnemonic
        ops = ins.operands
        cond = ins.cc not in (0, 15)  # conditional (arm cc: 15 = al in capstone)
        if m.startswith('stm') or m.startswith('str'):
            if m.startswith('stm'):
                rn = ops[0].reg
                if isinstance(regs.get(rn), int):
                    cnt = len(ops) - 1
                    a0 = regs[rn]
                    if 'db' in m: a0 -= 4 * cnt
                    elif 'da' in m: a0 -= 4 * (cnt - 1)
                    elif 'ib' in m: a0 += 4
                    for i in range(cnt):
                        if shadowed(a0 + 4 * i): found.setdefault(ins.address | thumb, set()).add(a0 + 4 * i)
                    if ins.writeback: regs[rn] = a0 + 4 * cnt if 'ia' in m or m == 'stm' else None
            else:
                mem = [o for o in ops if o.type == ARM_OP_MEM]
                if mem:
                    mm = mem[0].mem
                    if isinstance(regs.get(mm.base), int):
                        b = regs[mm.base]
                        sz = 1 if m.startswith('strb') else 2 if m.startswith('strh') else 8 if m.startswith('strd') else 4
                        if mm.index == 0:
                            a = b + mm.disp if not ins.post_index else b
                            for k in range(0, sz, 2 if sz > 1 else 1):
                                if shadowed(a + k): found.setdefault(ins.address | thumb, set()).add(a + k)
                        elif any(shadowed(b + k) for k in range(0, 0x100, 2)):
                            found.setdefault(ins.address | thumb, set()).add(('idx', b))
                        if ins.writeback:
                            regs[mm.base] = None
            pc += ins.size; continue
        if ins.group(CS_GRP_CALL):
            if ops and ops[0].type == ARM_OP_IMM and depth < 2:
                args = {r: v for r, v in regs.items() if r in ARG and isinstance(v, int)}
                t = ops[0].imm
                key = (t, tuple(sorted(args.items())))
                if args and key not in seen_calls:
                    seen_calls.add(key)
                    tt = thumb ^ (1 if m == 'blx' else 0)
                    cb, cbuf = region_of(t)
                    if cb is not None: trace(thb if tt else arm, cb, cbuf, t, None, None, tt, args, depth + 1)
            for r in list(regs):
                if r in ARG: del regs[r]
            if not any(isinstance(v, int) for v in regs.values()): return
            pc += ins.size; continue
        if ins.group(CS_GRP_JUMP) or ins.group(CS_GRP_RET) or (ops and ops[0].type == ARM_OP_REG and ops[0].reg == ARM_REG_PC and not m.startswith(('cmp','tst'))) or (m in ('pop', 'ldm', 'ldmia') and any(o.type == ARM_OP_REG and o.reg == ARM_REG_PC for o in ops)):
            if cond and m.startswith('b') and not m.startswith('bx'):
                pc += ins.size; continue  # follow the fall-through
            return
        if m.startswith('ldr') and not m.startswith(('ldrb','ldrh','ldrsb','ldrsh','ldrd')) and len(ops) == 2 and ops[1].type == ARM_OP_MEM and ops[1].mem.base == ARM_REG_PC and ops[1].mem.index == 0 and not cond:
            lit = ((ins.address + 4) & ~3 if thumb else ins.address + 8) + ops[1].mem.disp
            v = struct.unpack_from('<I', buf, lit - base)[0] if base <= lit < end - 3 else None
            if v is not None: regs[ops[0].reg] = ('k', v)
            elif ops[0].reg in regs: del regs[ops[0].reg]
            pc += ins.size; continue
        if m.startswith(('mov', 'movs')) and len(ops) == 2 and ops[1].type == ARM_OP_IMM and not cond:
            regs[ops[0].reg] = ('k', ops[1].imm & 0xFFFFFFFF)
            pc += ins.size; continue
        if ops and ops[0].type == ARM_OP_REG and not m.startswith(('cmp', 'tst', 'teq', 'cmn')):
            dst = ops[0].reg
            srcs = ops[1:]
            vals = None
            if m.startswith(('add', 'sub', 'orr', 'mov')) and not m.startswith('movt'):
                vals = []
                for s_ in srcs:
                    if s_.type == ARM_OP_IMM: vals.append(s_.imm)
                    elif s_.type == ARM_OP_REG and s_.reg in regs and regs[s_.reg] is not None and not s_.shift.type: vals.append(cval(regs[s_.reg]))
                    else: vals.append(None)
            newv = None
            if vals is not None and None not in vals:
                if m.startswith('mov') and len(vals) == 1 and srcs[0].type == ARM_OP_REG: newv = vals[0]
                elif len(vals) == 2: newv = vals[0] + vals[1] if m.startswith('add') else vals[0] - vals[1] if m.startswith('sub') else vals[0] | vals[1]
                elif len(vals) == 1 and m.startswith(('add', 'sub', 'orr')) and dst in regs:
                    v0 = cval(regs[dst]); newv = v0 + vals[0] if m.startswith('add') else v0 - vals[0] if m.startswith('sub') else v0 | vals[0]
            if newv is not None and not cond: regs[dst] = newv if isIoBase(newv) else ('k', newv & 0xFFFFFFFF)
            elif dst in regs:
                if cond: regs[dst] = None  # maybe changed
                else: del regs[dst]
                if not any(isinstance(v, int) for v in regs.values()): return
        pc += ins.size


rom = dict(l.split('|')[:2] for l in open('games_all.txt') if '|' in l)[G]
r = open(rom, 'rb')
hdr = r.read(0x200)
a9off, a9entry, a9ram, a9size = struct.unpack_from('<IIII', hdr, 0x20)
ovtoff, ovtsize = struct.unpack_from('<II', hdr, 0x50)
fatoff, fatsize = struct.unpack_from('<II', hdr, 0x48)
r.seek(ovtoff); ovt = r.read(ovtsize)
ovs = [struct.unpack_from('<8I', ovt, i) for i in range(0, ovtsize, 32)]
def blz(d):
    # nitro backward lz: footer = end offset (24 bits) + header len (8), then extra size
    d = bytearray(d)
    enc = struct.unpack_from('<I', d, len(d) - 8)[0]; extra = struct.unpack_from('<I', d, len(d) - 4)[0]
    hlen = enc >> 24; clen = enc & 0xFFFFFF
    out = bytearray(len(d) + extra)
    out[:len(d)] = d
    src = len(d) - hlen; dst = len(out); stop = len(d) - clen
    while src > stop:
        src -= 1; flags = d[src]
        for _ in range(8):
            if src <= stop: break
            if flags & 0x80:
                src -= 2; v = d[src] | (d[src + 1] << 8)
                ln = (v >> 12) + 3; disp = (v & 0xFFF) + 3
                for _ in range(ln):
                    dst -= 1; out[dst] = out[dst + disp]
            else:
                src -= 1; dst -= 1; out[dst] = d[src]
            flags <<= 1
    return bytes(out)
def ovlData(o):
    fid = o[6]
    r.seek(fatoff + fid * 8); st, en = struct.unpack('<II', r.read(8))
    r.seek(st); d = r.read(en - st)
    if o[7] & 0x01000000:
        try: d = blz(d[:o[7] & 0xFFFFFF])
        except Exception as e: return None
    return d[:o[2]]

def scanRegions():
    # literal pool loads: ARM ldr rd,[pc,#imm] and Thumb ldr rd,[pc,#imm8*4]
    for base, buf in regions:
        L = len(buf)
        for off in range(0, L - 4, 4):
            w = struct.unpack_from('<I', buf, off)[0]
            # ARM: cond 0101 U0 0 1 1111 Rd imm12
            if (w & 0x0F7F0000) == 0x051F0000 and (w >> 28) != 0xF:
                imm = w & 0xFFF; up = (w >> 23) & 1
                lit = base + off + 8 + (imm if up else -imm)
                v = struct.unpack_from('<I', buf, lit - base)[0] if base <= lit <= base + len(buf) - 4 else None
                if isIoBase(v):
                    rdn = (w >> 12) & 0xF
                                    # capstone reg ids: r0 = ARM_REG_R0
                    from capstone.arm import ARM_REG_R0
                    if rdn < 13: trace(arm, base, buf, base + off + 4, ARM_REG_R0 + rdn, v, 0)
            # ARM mov rd,#0x04000000
            if (w & 0x0FEF0FFF) == 0x03A00301:
                from capstone.arm import ARM_REG_R0
                rdn = (w >> 12) & 0xF
                if rdn < 13: trace(arm, base, buf, base + off + 4, ARM_REG_R0 + rdn, 0x04000000, 0)
        for off in range(0, L - 2, 2):
            h = struct.unpack_from('<H', buf, off)[0]
            from capstone.arm import ARM_REG_R0
            if (h & 0xF800) == 0x4800:
                lit = ((base + off + 4) & ~3) + (h & 0xFF) * 4
                v = struct.unpack_from('<I', buf, lit - base)[0] if base <= lit <= base + len(buf) - 4 else None
                if isIoBase(v):
                    trace(thb, base, buf, base + off + 2, ARM_REG_R0 + ((h >> 8) & 7), v, 1)
            # movs rd,#0x40 ; lsls rd,rd,#20  (or #1 / #26)
            if (h & 0xF8FF) == 0x2040 and off + 4 <= L:
                h2 = struct.unpack_from('<H', buf, off + 2)[0]
                r = (h >> 8) & 7
                if h2 == (0x0000 | (20 << 6) | (r << 3) | r):
                    trace(thb, base, buf, base + off + 4, ARM_REG_R0 + r, 0x04000000, 1)
    
    
# the static arm9 and itcm from the dump, minus the overlay area; then each overlay alone
regions = [(0x01FF8000, itcm), (0x02000000, ram)]
scanRegions()
inOvl = lambda p: any(o[1] <= (p & ~1) < o[1] + o[2] for o in ovs) and not (a9ram <= (p & ~1) < a9ram + a9size)
for p in [p for p in found if inOvl(p)]: del found[p]
seen_calls.clear()
for o in ovs:
    d = ovlData(o)
    if d:
        regions = [(o[1], d)]
        seen_calls.clear()
        scanRegions()
regions = [(0x01FF8000, itcm), (0x02000000, ram)]
# the ground truth: game sites (not the bios/loader at frame 0, not the menu, no dma)
truth = collections.defaultdict(set)
for line in open(f'out/shl/{G}.txt'):
    pc, addr, op, n, first, last = line.split()
    pc = int(pc, 16); addr = int(addr, 16); first = int(first)
    if pc in (0x0200828B, 0x020082A7, 0x0200937B, 0x02009381) or first == 0 or first >= 1500 or pc == 0xD0000000 or pc >= 0x02400000 or pc < 0x01FF8000: continue
    truth[pc].add(addr)
dma = [l for l in open(f'out/shl/{G}.txt') if l.startswith('D0000000') and 0 < int(l.split()[4]) < 1500]

hit = [p for p in truth if p in found]
miss = [p for p in truth if p not in found]
extra = [p for p in found if p not in truth]
print(f'{G}: truth sites {len(truth)}, found {len(hit)}, missed {len(miss)}, scanner-only {len(extra)}, dma regs {len(dma)}')
for p in sorted(miss)[:int(sys.argv[2]) if len(sys.argv) > 2 else 12]:
    print(f'  missed {p:08X} {"t" if p & 1 else " "} regs {" ".join("%X" % a for a in sorted(truth[p]))[:60]}')

# where the truth sites live: the static arm9 binary, its autoloads (itcm), or an overlay
def where(p):
    p &= ~1
    if p < 0x02000000: return 'itcm'
    if a9ram <= p < a9ram + a9size: return 'arm9'
    for o in ovs:
        if o[1] <= p < o[1] + o[2]: return 'ovl'
    return '?'
stat = collections.Counter()
for p, addrs in truth.items():
    kind = 'thumb' if p & 1 else 'arm'
    f = found.get(p)
    how = 'missed' if f is None else 'idx' if any(isinstance(x, tuple) for x in f) else 'static'
    stat[(where(p), kind, how)] += 1
print(f'  arm9 {a9ram:08X}+{a9size:X}, {len(ovs)} overlays; truth sites by place/mode/scan:', dict(sorted(stat.items())))
ex = collections.Counter((where(p), 'thumb' if p & 1 else 'arm') for p in extra)
print('  scanner-only by place/mode:', dict(sorted(ex.items())))
allc = collections.Counter((where(p) if where(p) != '?' else 'arm9', 'thumb' if p & 1 else 'arm') for p in found)
print('  all found by place/mode:', dict(sorted(allc.items())))
if len(sys.argv) > 3:
    for p in sorted(extra)[:int(sys.argv[3])]:
        print(f'  extra {p:08X} {"t" if p & 1 else " "} {" ".join(("%X" % a) if isinstance(a, int) else str(a) for a in sorted(found[p], key=str))[:70]}')
