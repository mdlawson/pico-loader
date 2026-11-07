import os
import sys, struct, subprocess, collections
G = sys.argv[1]
out = subprocess.run(['tools/scantest/scantest', f'out/dumps/{G}.ram', f'out/dumps/{G}.itcm'], capture_output=True, text=True)
found = set(int(l, 16) for l in out.stdout.split())
rom = dict(l.split('|')[:2] for l in open('lists/games_all.txt') if '|' in l)[G]
rom = rom if rom.startswith('/') else os.path.join(os.environ.get('ROMS', os.path.expanduser('~/emu/roms')), rom)
r = open(rom, 'rb'); hdr = r.read(0x200)
a9ram, a9size = struct.unpack_from('<II', hdr, 0x28)
ovtoff, ovtsize = struct.unpack_from('<II', hdr, 0x50)
r.seek(ovtoff); ovt = r.read(ovtsize)
ovs = [struct.unpack_from('<8I', ovt, i) for i in range(0, ovtsize, 32)]
inOvl = lambda p: any(o[1] <= p < o[1] + o[2] for o in ovs) and not (a9ram <= p < a9ram + a9size)
truth = set()
for line in open(f'out/shl/{G}.txt'):
    pc, addr, op, n, first, last = line.split()
    pc = int(pc, 16); first = int(first)
    if pc in (0x0200828B, 0x020082A7, 0x0200937B, 0x02009381) or first == 0 or first >= 1500 or pc >= 0x02400000 or pc < 0x01FF8000: continue
    if inOvl(pc): continue
    truth.add(pc)
miss = sorted(truth - found)
print(f'{G}: arm static truth {len(truth)}, found {len(truth & found)}, missed {len(miss)}: {" ".join("%08X" % p for p in miss[:8])}; scanner total {len(found)} ({out.stderr.strip()})')
