#!/usr/bin/env python3
# sndcmp.py DIR...: how close the sound after the load is to the reference: the
# best cross-correlation over a +-0.1 s lag (mono), and the levels
import sys, wave, numpy as np
def load(p):
    w = wave.open(p); a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
    return a.reshape(-1, 2).mean(axis=1)
for d in sys.argv[1:]:
    try:
        r = load(d + "/sound_ref.wav"); c = load(d + "/sound_load.wav")
    except Exception as e:
        print(f"{d}: {e}"); continue
    n = min(len(r), len(c)); r, c = r[:n], c[:n]
    lr, lc = np.sqrt((r ** 2).mean()), np.sqrt((c ** 2).mean())
    best, lag = 0.0, 0
    if lr > 1 and lc > 1:
        R = np.fft.rfft(r, 2 * n); C = np.fft.rfft(c, 2 * n)
        x = np.fft.irfft(R * np.conj(C))
        lags = np.r_[0:3300, len(x) - 3300:len(x)]
        k = lags[np.argmax(x[lags])]
        best = x[k] / (np.sqrt((r ** 2).sum() * (c ** 2).sum()))
        lag = k if k < n else k - len(x)
    print(f"{d.rstrip('/').split('/')[-1]:8s} level ref {lr:6.0f} load {lc:6.0f}  correlation {best:5.2f} at lag {lag:+d}")
