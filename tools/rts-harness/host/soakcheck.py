import sys, os, re, hashlib, glob
for d in sorted(glob.glob(sys.argv[1] + '/*')):
    e = open(d + '/stderr.txt', errors='replace').read()
    ab = re.search(r'aborts: (\d+)', e); ab = ab.group(1) if ab else '?'
    pcs = re.findall(r'frame +(\d+) +arm9 pc ([0-9A-F]+)', e)
    last = pcs[-1] if pcs else ('-', '-')
    shots = sorted(glob.glob(d + '/s*.ppm'))
    hs = [hashlib.md5(open(s, 'rb').read()).hexdigest()[:6] for s in shots]
    run = best = 1
    for i in range(1, len(hs)):
        run = run + 1 if hs[i] == hs[i-1] else 1; best = max(best, run)
    bad = [l for l in e.splitlines() if re.search(r'undefined|prefetch|data abort|exception|guru|crash', l, re.I)][:2]
    print('%-22s aborts %-3s last frame %s pc %s  shots %d unique %d longest-same %d %s' % (os.path.basename(d), ab, last[0], last[1], len(hs), len(set(hs)), best, bad))
