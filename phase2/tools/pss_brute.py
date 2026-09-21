#!/usr/bin/env python3
"""Two-stage PSS lock: coarse integer-bin scan, then fractional-CFO refine.

Stage 1: for each integer bin offset b (-384..383), take the 127-bin window
that would hold the PSS and correlate against the root-29 ZC across all
windows.  No CFO rotation (integer bins only) — fast.  Report top bins.

Stage 2: around the single best integer bin, test fractional shifts
(b + f) for f in -0.5..+0.5 step 0.05 using the DFT shift theorem, again
across all windows.  Report the (bin, cfo) pair with peak correlation and its
window index (PSS arrival time).

Usage: python3 pss_brute.py <dump.cf32> [nwin] [cfo_refine=1]
"""
import sys
import time
import numpy as np

SRATE = 23.04e6
FFT = 768
ZC = 29
BASE = np.arange(127) - 63          # PSS window centered on candidate bin c


def load(path, nwin):
    d = np.fromfile(path, dtype=np.complex64)
    n = min(len(d) // FFT, nwin)
    x = d[: n * FFT].reshape(n, FFT)
    x = x - x.mean(axis=1, keepdims=True)
    win = np.hanning(FFT).astype(np.float32)
    win = win / win.sum()
    X = np.fft.fft(x * win, axis=1)
    X[:, 0] = 0.0
    rms = np.sqrt(np.mean(np.abs(X) ** 2, axis=1)) + 1e-12
    return X / rms[:, None]


def main():
    path = sys.argv[1]
    nwin = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    refine = len(sys.argv) < 4 or int(sys.argv[3]) != 0

    t0 = time.time()
    Z = load(path, nwin)
    n_idx = np.arange(127)
    zc_n = np.exp(-1j * np.pi * ZC * (n_idx + 1) * n_idx / 127)
    zc_n = (zc_n / np.linalg.norm(zc_n)).conj()
    print(f"{path}: {Z.shape[0]} windows loaded ({time.time()-t0:.0f}s)", flush=True)

    # ---- Stage 1: integer bin scan ----
    best1 = []   # (corr, b, win)
    for b in range(-FFT // 2, FFT // 2):
        idx = (BASE + b) % FFT
        sl = Z[:, idx]
        corr = np.abs(sl @ zc_n)
        w = int(np.argmax(corr))
        best1.append((float(corr[w]), b, w))
    best1.sort(reverse=True)
    print("stage 1 top bins:", " ".join(f"b={b:+d}:{c:.2f}" for c, b, _ in best1[:8]),
          f"({time.time()-t0:.0f}s)", flush=True)
    b0 = best1[0][1]
    c0 = best1[0][0]

    if not refine:
        b, w = best1[0][1], best1[0][2]
        print(f"\nBEST (integer only): bin_offset={b:+d} corr={c0:.3f} at window {w}")
        return

    # ---- Stage 2: fractional CFO refinement around b0 ----
    best2 = []   # (corr, bs, cfo, win)
    for f in np.arange(-0.5, 0.501, 0.05):
        shift = b0 + f
        k = np.arange(FFT, dtype=np.float64)
        Xs = Z * np.exp(-2j * np.pi * f * k / FFT)[None, :]
        idx = ((BASE + shift) % FFT).astype(int)
        corr = np.abs(Xs[:, idx] @ zc_n)
        w = int(np.argmax(corr))
        best2.append((float(corr[w]), shift, f, w))
    best2.sort(reverse=True)
    print("stage 2 top:", " ".join(f"shift={s:+.2f}:{c:.2f}@w{w}" for c, s, _, w in best2[:6]),
          f"({time.time()-t0:.0f}s)", flush=True)
    c, bs, f, w = best2[0]
    print(f"\nPSS LOCK: corr={c:.3f}  SSB center {bs:+.2f} bins "
          f"(={(bs*30e3)/1e6:+.3f} MHz vs SSB tune)  CFO={f:+.2f} bin ({f*30e3:.0f} Hz)")
    print(f"  window {w} = {w*FFT/SRATE*1e3:.2f} ms")
    print(f"  rx_probe pss_bin_shift (SSB-center vs carrier): bin {int(round(bs))}")

if __name__ == "__main__":
    main()