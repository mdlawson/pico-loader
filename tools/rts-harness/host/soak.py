#!/usr/bin/env python3
# soak.py NAME ROM VARIANT_DIR FRAMES SEED OUT: random single-key/touch play, no exit combo
import random, subprocess, sys, os, shutil
_HERE = os.path.dirname(os.path.abspath(__file__))
_EMU = os.environ.get("EMU", os.path.expanduser("~/emu"))
def _env(k, d): return os.environ.get(k, d)
HOSTDIR = _env("HOSTDIR", _HERE)
SDROOT = _env("SDROOT", _EMU + "/rtssd")
OUTDIR = _env("OUTDIR", _EMU + "/out")
ROMS = _env("ROMS", _EMU + "/roms")
LAUNCHER = _env("LAUNCHER", os.path.join(os.path.dirname(_HERE), "launcher", "launcher.nds"))
DLDI = _env("DLDI", os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(_HERE))), "arm9", "data", "dspicoDldi.bin"))
PICO_DIR = _env("PICO_DIR", _EMU + "/rts_pico")
name, rom, pico, frames, seed, out = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
r = random.Random(seed)
keys = ["A"]*6 + ["B"]*3 + ["X","Y","L","R"] + ["UP","DOWN","LEFT","RIGHT"]*3 + ["START"]*2
args = []; f = 300
# past the title screens first
while f < 1500:
    args += ["--key", "%d:%s:4" % (f, "START" if (f // 45) % 2 else "A")]
    f += 45
while f < frames - 60:
    if r.random() < 0.35:
        args += ["--touch", "%d:%d:%d:%d" % (f, r.randrange(8, 248), r.randrange(8, 184), r.randrange(2, 20))]
    else:
        args += ["--key", "%d:%s:%d" % (f, r.choice(keys), r.randrange(2, 30))]
    f += r.randrange(35, 90)
for s in range(1800, frames + 1, 1800):
    args += ["--shot", "%d:%s/s%06d.ppm" % (s, out, s)]
sd = SDROOT + "/soak_%s_%s" % (name, os.path.basename(pico))
shutil.rmtree(sd, ignore_errors=True); os.makedirs(sd + "/_pico"); os.makedirs(out, exist_ok=True); os.makedirs(OUTDIR, exist_ok=True)
rom = rom if rom.startswith("/") else os.path.join(ROMS, rom)
os.link(rom, sd + "/game.nds")
shutil.copy(LAUNCHER, sd)
for fn in os.listdir(pico): shutil.copy(os.path.join(pico, fn), sd + "/_pico/")
open(sd + "/launch.txt", "w").write("/game.nds")
save = len(sys.argv) > 7 and os.path.expandvars(sys.argv[7])
if save and os.path.exists(save): shutil.copy(save, sd + "/game.sav")
for p in [sd + ".img", sd + ".img.idx"]:
    pass
img = OUTDIR + "/" + os.path.basename(sd) + ".img"
for p in (img, img + ".idx"):
    if os.path.exists(p): os.remove(p)
env = dict(os.environ, ABORT_RING="1")
with open(out + "/stderr.txt", "w") as e:
    subprocess.run([HOSTDIR + "/dsrun.sh", sd, str(frames), "--status", "600"] + args, stdout=subprocess.DEVNULL, stderr=e, env=env)
