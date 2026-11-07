#!/usr/bin/env python3
"""play.py - step a game in the emulator a little at a time, for an agent (or a person) to play.

Each session is a machine snapshot plus the sd image it goes with, so a step is: load the
snapshot, press the given inputs, save a new snapshot and a screenshot. Nothing is re-run.

  play.py new SESSION GAME [--save FILE|--nosave] [--dsi] [--pico DIR]
      boot GAME (a name from GAMES, or a rom path) through the pico loader; shows the screen
  play.py step SESSION "SCRIPT"
      run SCRIPT from the current snapshot; prints the screenshot path(s)
  play.py undo SESSION [N]          go back N steps (default 1)
  play.py restore SESSION|all [--force]  rebuild a packaged session (../play/sessions) for replay
  play.py mark SESSION LABEL        keep the current snapshot as a checkpoint
  play.py log SESSION               the steps so far
  play.py exportsave SESSION FILE   the game's own save file as it is now on the sd card
  play.py shot SESSION              the current screen again

SCRIPT is space-separated tokens, run in order:
  A B X Y L R START SELECT UP DOWN LEFT RIGHT   tap (held 6 frames, then 10 frames released)
  A:30                                          hold for 30 frames (then 10 released)
  UP+B:20                                       several buttons together
  w:60  (or wait:60)                            press nothing for 60 frames
  t:X,Y  t:X,Y:N                                touch the bottom screen at X,Y (0-255, 0-191), 20 frames by default
  drag:X1,Y1,X2,Y2:N                            drag across N frames
  path:X1,Y1,X2,Y2,X3,Y3...:N                   one stroke through all the points (N frames a segment), e.g. a closed loop
  snap                                          take an extra screenshot here
  mash:A:N                                      tap A N times (to skip text)
After the script it waits 20 frames and takes the screenshot.
"""
import gzip, json, os, shutil, subprocess, sys, hashlib
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

HOST = HOSTDIR
ROOT = _env("PLAYROOT", _EMU + "/play")
B = _env("BIOS", _EMU + "/bios")
GAMES = {
    "sm64": ROMS + "/sm64/Super Mario 64 DS (USA).nds",
    "aw": ROMS + "/aw/Advance Wars - Dual Strike (USA, Australia).nds",
    "ph": ROMS + "/ph/Legend of Zelda, The - Phantom Hourglass (USA) (En,Fr,Es).nds",
    "ml": ROMS + "/ml/Mario & Luigi - Partners in Time (USA) (Rev 1).nds",
    "ct": ROMS + "/ct/Chrono Trigger (USA) (En,Fr).nds",
    "pkd": ROMS + "/pkd/Pokemon - Diamond Version (USA) (Rev 5).nds",
    "layton": ROMS + "/layton/Professor Layton and the Curious Village (USA, Australia).nds",
    "cv": ROMS + "/cv/Castlevania - Dawn of Sorrow (USA).nds",
    "rs": ROMS + "/rs/Dragon Quest Heroes - Rocket Slime (USA).nds",
    "eba": ROMS + "/eba/Elite Beat Agents (USA).nds",
    "gsdd": ROMS + "/gsdd/Golden Sun - Dark Dawn (USA, Australia) (En,Es).nds",
    "gta": ROMS + "/gta/Grand Theft Auto - Chinatown Wars (USA) (En,Fr,De,Es,It).nds",
    "st": ROMS + "/st/Legend of Zelda, The - Spirit Tracks (USA) (En,Fr,Es) (Rev 1).nds",
    "mph": ROMS + "/mph/Metroid Prime - Hunters (USA) (Rev 1).nds",
    "nsmb": ROMS + "/nsmb/New Super Mario Bros. (USA).nds",
    "diy": ROMS + "/diy/WarioWare - D.I.Y. (USA).nds",
    "dq9": ROMS + "/dq9/Dragon Quest IX - Sentinels of the Starry Skies (USA) (En,Fr,Es).nds",
}
KEYS = {"A", "B", "X", "Y", "L", "R", "START", "SELECT", "UP", "DOWN", "LEFT", "RIGHT"}


def die(msg):
    print("error: " + msg); sys.exit(1)


def sdir(sess): return os.path.join(ROOT, sess)


def load_meta(sess):
    p = os.path.join(sdir(sess), "meta.json")
    if not os.path.exists(p): die("no session %s (play.py new %s GAME)" % (sess, sess))
    return json.load(open(p))


def save_meta(sess, m): json.dump(m, open(os.path.join(sdir(sess), "meta.json"), "w"), indent=1)


def parse(script, tdef=20):
    """-> (keys [(frame, mask, len)], touches [(frame, x, y, len)], snaps [frame], total frames)"""
    keys, touches, snaps, f = [], [], [], 0
    for tok in script.split():
        u = tok.upper()
        if u == "SNAP": snaps.append(f); continue
        if u.startswith("W:") or u.startswith("WAIT:"):
            f += int(u.split(":")[1]); continue
        if u.startswith("MASH:"):
            _, k, n = u.split(":")
            if k not in KEYS: die("unknown key " + k)
            for _ in range(int(n)): keys.append((f, k, 6)); f += 16
            continue
        if u.startswith("T:") or u.startswith("TOUCH:"):
            parts = tok.split(":")
            x, y = (int(v) for v in parts[1].split(","))
            n = int(parts[2]) if len(parts) > 2 else tdef  # games often miss shorter touches
            touches.append((f, x, y, n)); f += n + 10; continue
        if u.startswith("DRAG:"):
            parts = tok.split(":")
            x1, y1, x2, y2 = (int(v) for v in parts[1].split(","))
            n = int(parts[2]) if len(parts) > 2 else 20
            for i in range(n + 1):
                touches.append((f + i, x1 + (x2 - x1) * i // n, y1 + (y2 - y1) * i // n, 1))
            f += n + 11; continue
        if u.startswith("PATH:"):
            # path:X1,Y1,X2,Y2,...:N - one stroke through every point, N frames a segment
            parts = tok.split(":")
            v = [int(x) for x in parts[1].split(",")]
            n = int(parts[2]) if len(parts) > 2 else 10
            pts = list(zip(v[0::2], v[1::2]))
            if len(pts) < 2: die("path needs two points or more")
            t = f
            for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
                for i in range(n):
                    touches.append((t, x1 + (x2 - x1) * i // n, y1 + (y2 - y1) * i // n, 1)); t += 1
            touches.append((t, pts[-1][0], pts[-1][1], 2))
            f = t + 12; continue
        name, _, n = u.partition(":")
        ks = name.split("+")
        for k in ks:
            if k not in KEYS: die("unknown token " + tok)
        n = int(n) if n else 6
        keys.append((f, "+".join(ks), n)); f += n + 10
    return keys, touches, snaps, f + 20


def run(m, frames, extra, state_in, state_out, img, env_extra={}):
    env = dict(os.environ, SDIMG_ONLY="1", SAVE_STATE=state_out, **env_extra)
    # a rom over 192 MB doesn't fit the default 256 MB sd image (Golden Sun DD, DQ9)
    try:
        if os.path.getsize(m["sd"] + "/game.nds") > (192 << 20): env.setdefault("DSHOST_SD_MB", "512")
    except OSError: pass
    if state_in: env["LOAD_STATE"] = state_in
    else: env.pop("SDIMG_ONLY")
    cmd = [HOST + "/dshost", "--dspico", DLDI,
           "--bios9", B + "/bios9.bin", "--bios7", B + "/bios7.bin", "--firmware", B + "/firmware.bin"]
    if m.get("dsi"):
        cmd += ["--bios9i", B + "/bios9i.bin", "--bios7i", B + "/bios7i.bin"]
    cmd += ["--cart", m["sd"] + "/launcher.nds", "--sd", m["sd"], "--sdimg", img,
            "--frames", str(frames), "--status", "0"] + extra
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env, text=True, errors="replace")
    if os.environ.get("PLAY_STDERR"): open(os.environ["PLAY_STDERR"], "w").write(r.stderr)
    if r.returncode or not os.path.exists(state_out):
        die("emulator failed: " + r.stderr[-800:])
    return r.stderr


def to_png(ppm, png):
    from PIL import Image
    im = Image.open(ppm)
    im = im.resize((512, 768), Image.NEAREST)
    im.save(png); os.remove(ppm)


def cur(sess, m):
    n = m["steps"][-1]["n"]
    return os.path.join(sdir(sess), "hist", "%04d" % n)


def digest(p):
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


def commit(sess, m, n, tmp_state, img, script, frames, inputs=None):
    """keep step n: the gzipped snapshot, and the image (a hard link when it didn't change)"""
    h = os.path.join(sdir(sess), "hist", "%04d" % n)
    with open(tmp_state, "rb") as i, gzip.open(h + ".mst.gz", "wb", compresslevel=1) as o: shutil.copyfileobj(i, o)
    os.remove(tmp_state)
    d = digest(img)
    prev = m["steps"][-1] if m["steps"] else None
    if prev and prev.get("img") == d: os.link(cur(sess, m) + ".img", h + ".img")
    else: shutil.copy(img, h + ".img")
    m["frame"] = m.get("frame", 0) + frames
    st = {"n": n, "script": script, "frames": frames, "frame": m["frame"], "img": d}
    if inputs: st["keys"], st["touches"] = inputs
    m["steps"].append(st)
    # only the last 40 steps can be undone; older snapshots go (checkpoints are copies)
    for s in m["steps"][1:-40]:
        for ext in (".mst.gz", ".img"):
            p = os.path.join(sdir(sess), "hist", "%04d%s" % (s["n"], ext))
            if os.path.exists(p): os.remove(p)
    save_meta(sess, m)


def step(sess, script, m=None):
    m = m or load_meta(sess)
    keys, touches, snaps, frames = parse(script)
    n = max(s["n"] for s in m["steps"]) + 1
    base = cur(sess, m); work = os.path.join(sdir(sess), "work")
    os.makedirs(work, exist_ok=True)
    state_in = work + "/in.mst"
    with gzip.open(base + ".mst.gz", "rb") as i, open(state_in, "wb") as o: shutil.copyfileobj(i, o)
    img = work + "/sd.img"; shutil.copy(base + ".img", img)
    if os.path.exists(base + ".img.idx"): shutil.copy(base + ".img.idx", img + ".idx")
    shots = os.path.join(sdir(sess), "shots"); os.makedirs(shots, exist_ok=True)
    extra = []
    for f, k, l in keys: extra += ["--key", "%d:%s:%d" % (f, k, l)]
    for f, x, y, l in touches: extra += ["--touch", "%d:%d:%d:%d" % (f, x, y, l)]
    outs = []
    for i, f in enumerate(snaps):
        p = "%s/%04d_%d" % (shots, n, i); extra += ["--shot", "%d:%s.ppm" % (max(f - 1, 0), p)]; outs.append(p)
    p = "%s/%04d" % (shots, n); extra += ["--shot", "%d:%s.ppm" % (frames - 1, p)]; outs.append(p)
    run(m, frames, extra, state_in, work + "/out.mst", img)
    commit(sess, m, n, work + "/out.mst", img, script, frames, (keys, touches))
    os.remove(state_in)
    for p in outs:
        if os.path.exists(p + ".ppm"): to_png(p + ".ppm", p + ".png")
    for p in outs: print(p + ".png")
    print("step %d, frame %d" % (n, m["frame"]))


def new(sess, game, save, dsi, pico, force=False):
    rom = GAMES.get(game, game)
    if not os.path.exists(rom): die("no rom " + rom)
    d = sdir(sess)
    if os.path.exists(d):
        if not force: die("session %s exists; use step/undo on it, or pick a new session name (--force replaces it)" % sess)
        shutil.rmtree(d)
    sd = d + "/sd"; os.makedirs(sd + "/_pico"); os.makedirs(d + "/hist")
    try: os.link(rom, sd + "/game.nds")
    except OSError: shutil.copy(rom, sd + "/game.nds")
    shutil.copy(LAUNCHER, sd)
    for fn in os.listdir(pico):
        if os.path.isfile(os.path.join(pico, fn)): shutil.copy(os.path.join(pico, fn), sd + "/_pico/")
    open(sd + "/launch.txt", "w").write("/game.nds")
    if save is None and os.path.exists(SDROOT + "/%s/game.sav" % game): save = SDROOT + "/%s/game.sav" % game
    if save: shutil.copy(save, sd + "/game.sav")
    m = {"game": game, "rom": rom, "sd": sd, "dsi": dsi, "pico": pico, "save": save, "frame": 0, "steps": []}
    work = d + "/work"; os.makedirs(work); os.makedirs(d + "/shots")
    # boot: the launcher, the loader and the game's first screens
    run(m, 300, ["--shot", "299:%s/shots/0000.ppm" % d], None, work + "/out.mst", work + "/sd.img")
    if os.path.exists(work + "/sd.img.idx"): os.remove(work + "/sd.img.idx")
    commit(sess, m, 0, work + "/out.mst", work + "/sd.img", "(boot)", 300)
    to_png(d + "/shots/0000.ppm", d + "/shots/0000.png")
    print(d + "/shots/0000.png"); print("step 0, frame 300")


def main():
    a = sys.argv[1:]
    if len(a) < 2: print(__doc__); sys.exit(1)
    cmd, sess = a[0], a[1]
    if cmd == "new":
        rest = a[3:]; save = None; dsi = False; force = False; pico = PICO_DIR
        i = 0
        while i < len(rest):
            if rest[i] == "--save": save = rest[i + 1]; i += 1
            elif rest[i] == "--nosave": save = ""
            elif rest[i] == "--dsi": dsi = True
            elif rest[i] == "--pico": pico = rest[i + 1]; i += 1
            elif rest[i] == "--force": force = True
            i += 1
        new(sess, a[2], save, dsi, pico, force)
    elif cmd == "step": step(sess, " ".join(a[2:]))
    elif cmd == "undo":
        m = load_meta(sess); k = int(a[2]) if len(a) > 2 else 1
        if k >= len(m["steps"]) or not os.path.exists(os.path.join(sdir(sess), "hist", "%04d.mst.gz" % m["steps"][-k - 1]["n"])):
            die("can't go back that far")
        for s in m["steps"][-k:]:
            for ext in (".mst.gz", ".img"):
                p = os.path.join(sdir(sess), "hist", "%04d%s" % (s["n"], ext))
                if os.path.exists(p): os.remove(p)
        m["steps"] = m["steps"][:-k]; m["frame"] = m["steps"][-1]["frame"]; save_meta(sess, m)
        print(os.path.join(sdir(sess), "shots", "%04d.png" % m["steps"][-1]["n"]))
        print("back at step %d, frame %d" % (m["steps"][-1]["n"], m["frame"]))
    elif cmd == "mark":
        m = load_meta(sess); c = os.path.join(sdir(sess), "checkpoints", a[2]); os.makedirs(c, exist_ok=True)
        b = cur(sess, m)
        shutil.copy(b + ".mst.gz", c + "/state.mst.gz")
        if os.path.exists(c + "/sd.img"): os.remove(c + "/sd.img")
        os.link(b + ".img", c + "/sd.img")  # history images are never written again
        shutil.copy(os.path.join(sdir(sess), "shots", "%04d.png" % m["steps"][-1]["n"]), c + "/screen.png")
        json.dump({"game": m["game"], "rom": m["rom"], "dsi": m["dsi"], "pico": m["pico"], "frame": m["frame"],
                   "sd": m["sd"], "steps": m["steps"]}, open(c + "/info.json", "w"), indent=1)
        print("checkpoint " + c)
    elif cmd == "replay":
        # replay SESSION FRAME OUTDIR [rts saveFrame:gap] [--pico DIR]: every step's inputs from a cold
        # boot in one run, up to FRAME (a checkpoint's), then the rts test there - no snapshots involved
        m = load_meta(sess); upto = a[2]; out = a[3]; os.makedirs(out, exist_ok=True)
        cp = os.path.join(sdir(sess), "checkpoints", upto, "info.json")
        if os.path.exists(cp):  # a checkpoint's name: its own steps, whatever was undone since
            info = json.load(open(cp)); upto = info["frame"]
            if "steps" in info: m["steps"] = info["steps"]
        upto = int(upto)
        fr = a[4] if len(a) > 4 and not a[4].startswith("--") else "120:180"
        if "--pico" in a: m["pico"] = a[a.index("--pico") + 1]
        extra = []; base = 300
        for s in m["steps"][1:]:
            if s["frame"] > upto: break
            if "keys" in s:
                keys, touches, frames = s["keys"], s["touches"], s["frames"]
            else:
                # steps from before touches defaulted to 20 frames: the default that gives their length
                for tdef in (20, 6):
                    keys, touches, snaps, frames = parse(s["script"], tdef)
                    if frames == s["frames"]: break
            for f, k, l in keys: extra += ["--key", "%d:%s:%d" % (base + f, k, l)]
            for f, x, y, l in touches: extra += ["--touch", "%d:%d:%d:%d" % (base + f, x, y, l)]
            base += frames
        if base != upto: die("no step ends at frame %d" % upto)
        sv, gap = (int(v) for v in fr.split(":"))
        sd = out + "/sd"; shutil.rmtree(sd, ignore_errors=True)
        # the roms linked (never written), the rest copied: the image's changes go back into these files
        shutil.copytree(m["sd"], sd, ignore=shutil.ignore_patterns("states"),
                        copy_function=lambda a, b: os.link(a, b) if a.endswith(".nds") else shutil.copy2(a, b))
        if m["pico"]:
            for fn in os.listdir(m["pico"]):
                if os.path.isfile(os.path.join(m["pico"], fn)): shutil.copy(os.path.join(m["pico"], fn), sd + "/_pico/")
        shutil.rmtree(sd + "/_pico/states", ignore_errors=True)
        for p in (out + "/sd.img", out + "/sd.img.idx"):
            if os.path.exists(p): os.remove(p)
        m["sd"] = sd
        extra += ["--rts-test", "%d:%d" % (upto + sv, gap), "--rts-out", out, "--shot", "%d:%s/at_save.ppm" % (upto + sv - 1, out)]
        env = dict(os.environ); env.pop("SAVE_STATE", None)
        try: run(m, 60000 + upto, extra, None, out + "/end.mst", out + "/sd.img")
        except SystemExit: pass
        for p in (out + "/end.mst", out + "/sd.img", out + "/sd.img.idx", out + "/ram_ref.bin", out + "/ram_cmp.bin",
                  out + "/sound_ref.wav", out + "/sound_load.wav"):
            if os.path.exists(p): os.remove(p)
        shutil.rmtree(sd, ignore_errors=True)
        rp = out + "/report.txt"
        print(open(rp).read() if os.path.exists(rp) else "no report")
    elif cmd == "restore":
        # restore SESSION|all [--force]: set up a session from the snapshot-free copy in ../play/sessions
        # (inputs, checkpoint frames, the starting save) so "replay" / replayall.sh can rebuild its scenes
        pk = os.path.join(os.path.dirname(_HERE), "play", "sessions")
        names = sorted(os.listdir(pk)) if sess == "all" else [sess]
        for name in names:
            src = os.path.join(pk, name); d = sdir(name)
            if not os.path.exists(src + "/meta.json"): die("no packaged session " + name)
            if os.path.exists(d):
                if "--force" not in a: print("%s exists, skipped (--force replaces it)" % name); continue
                shutil.rmtree(d)
            m = json.load(open(src + "/meta.json"))
            rom = m["rom"] if m["rom"].startswith("/") else os.path.join(ROMS, m["rom"])
            if not os.path.exists(rom): print("%s: no rom %s, skipped" % (name, rom)); continue
            sd = d + "/sd"; os.makedirs(sd + "/_pico")
            try: os.link(rom, sd + "/game.nds")
            except OSError: shutil.copy(rom, sd + "/game.nds")
            shutil.copy(LAUNCHER, sd); open(sd + "/launch.txt", "w").write("/game.nds")
            if os.path.exists(src + "/game.sav"): shutil.copy(src + "/game.sav", sd + "/game.sav")
            for fn in os.listdir(PICO_DIR):
                if os.path.isfile(os.path.join(PICO_DIR, fn)): shutil.copy(os.path.join(PICO_DIR, fn), sd + "/_pico/")
            def fix(x): x["rom"] = rom; x["sd"] = sd; x["pico"] = PICO_DIR; return x
            m = fix(m); m["save"] = sd + "/game.sav" if m.get("save") else None; save_meta(name, m)
            for c in sorted(os.listdir(src + "/checkpoints")):
                os.makedirs(d + "/checkpoints/" + c)
                json.dump(fix(json.load(open(src + "/checkpoints/%s/info.json" % c))), open(d + "/checkpoints/%s/info.json" % c, "w"), indent=1)
            print("%s: %d checkpoints (replay only: no snapshots)" % (name, len(os.listdir(src + "/checkpoints"))))
    elif cmd == "log":
        m = load_meta(sess)
        for s in m["steps"]: print("%4d  f%-7d %s" % (s["n"], s["frame"], s["script"]))
    elif cmd == "shot":
        m = load_meta(sess); print(os.path.join(sdir(sess), "shots", "%04d.png" % m["steps"][-1]["n"]))
    elif cmd == "exportsave":
        m = load_meta(sess)
        r = subprocess.run(["7z", "e", "-so", cur(sess, m) + ".img", "game.sav"], capture_output=True)
        if r.returncode or not r.stdout: die("no game.sav in the image")
        os.makedirs(os.path.dirname(os.path.abspath(a[2])), exist_ok=True)
        open(a[2], "wb").write(r.stdout); print("%s (%d bytes)" % (a[2], len(r.stdout)))
    else: print(__doc__); sys.exit(1)


if __name__ == "__main__":
    main()
