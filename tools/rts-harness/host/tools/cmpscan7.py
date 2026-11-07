import os
import sys, subprocess, collections
# cmpscan7.py GAME: the arm7 scanner's sites against the harness's arm7 truth (out/shl7)
G = sys.argv[1]
rom = dict(l.split('|')[:2] for l in open('lists/games_all.txt') if '|' in l)[G]
rom = rom if rom.startswith('/') else os.path.join(os.environ.get('ROMS', os.path.expanduser('~/emu/roms')), rom)
out = subprocess.run(['tools/scantest7/scantest7', rom], capture_output=True, text=True)
found = set(int(l.split()[0], 16) for l in out.stdout.split('\n') if l.strip())
by = collections.defaultdict(lambda: [0, set(), 0])
for line in open(f'out/shl7/{G}.txt'):
    pc, addr, op, n, first, last = line.split()
    pc = int(pc, 16); a = int(addr, 16)
    if pc >= 0x06000000 or pc < 0x02000000 and pc != 0xD0000000 and pc < 0x01000000: pass
    e = by[pc]; e[0] += int(n); e[1].add(a); e[2] = int(first)
truth = {pc for pc in by if 0x02000000 <= pc < 0x03900000}
miss = sorted(truth - found)
print(f'{G}: truth {len(truth)} found {len(truth & found)}; scanner {len(found)}; others: ' + ' '.join('%08X(%d)' % (p, by[p][0]) for p in by if p not in truth))
for p in miss: print('  missed %08X n %d first f%d regs %s' % (p, by[p][0], by[p][2], ' '.join('%X' % a for a in sorted(by[p][1])[:6])))
