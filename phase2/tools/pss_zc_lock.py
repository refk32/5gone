#!/usr/bin/env python3
"""PSS ZC matched-filter lock — find the TRUE PSS bin offset in a capture.

RX is tuned to centre_freq.  SSB block (240 subcarriers) centered at bin `c`
from DC; PSS occupies subcarriers 56..182 -> absolute bins c-64..c+62.

Vectorized: we compute the per-window spectrum once, then for each candidate
`c` we take the 127-bin slice via a circular roll + slice (views, no copies
in the hot loop), dot with the conjugate reference.  nwin ~ 43k, so we keep
the loop tight and print progress.

Usage: python3 pss_zc_lock.py <dump.cf32> [c_lo:c_hi] [step]
"""
import sys
import time
import numpy as np

SRATE = 23.04e6
FFT = 768
kPssLen = 127


def main():
    path = sys.argv[1]
    if len(sys.argv) > 2:
        parts = sys.argv[2].split(":")
        lo, hi = int(parts[0]), int(parts[1])
    else:
        lo, hi = -184, 84
    if len(sys.argv) > 3:
        step = int(sys.argv[3])
    else:
        step = 1

    t0 = time.time()
    d = np.fromfile(path, dtype=np.complex64)
    n = (len(d) // FFT) * FFT
    x = d[:n].reshape(-1, FFT)
    x = x - x.mean(axis=1, keepdims=True)
    win = np.hanning(FFT).astype(np.float32)
    win = win / win.sum()
    X = np.fft.fft(x * win, axis=1)
    rms = np.sqrt(np.mean(np.abs(X) ** 2, axis=1)) + 1e-12
    del x
    print(f"capture {path}: {len(d)} samples ({len(d)/SRATE*1e3:.0f} ms), "
          f"windows={X.shape[0]}  FFT done in {time.time()-t0:.1f}s", flush=True)

    # reference on absolute-bin grid (length FFT, zero elsewhere), so the dot
    # product becomes a correlation with a sparse ref -> use real FFT trick:
    # build ref on the FFT grid for the nominal center c=0, then for each c we
    # roll the SPECTRUM by -c and dot with the fixed ref (rolling brings the
    # PSS slice to the same bins).  Rolling the spectrum by -c == shifting the
    # signal by +c.  That is exactly what "SSB center at bin c" means.
    ref = np.zeros(FFT, dtype=np.complex64)
    n2 = np.arange(8, 8 + kPssLen)
    root = 29
    ref[56:56 + kPssLen] = np.exp(-1j * np.pi * root * n2 * (n2 + 1) / kPssLen)
    ref_c = ref.conj()

    best = []
    nwin = X.shape[0]
    for c in range(lo, hi + 1, step):
        Xr = np.roll(X, 119 - c, axis=1)     # PSS center c -> nominal bin 119
        # PSS slice is bins 56..182 in the rolled spectrum
        corr = Xr[:, 56:183] @ ref_c[56:183]
        corr /= rms
        amp = np.abs(corr)
        peak_idx = int(np.argmax(amp))
        thr = np.quantile(amp, 0.9)
        score = float(amp[peak_idx]) / (thr + 1e-12)
        best.append((score, c, peak_idx, float(amp[peak_idx]), float(amp.mean())))

    best.sort(reverse=True)
    print("\ntop candidates (score, bin_offset c, peak_window, peak_corr, mean_corr):")
    for r in best[:8]:
        print(f"  score={r[0]:7.2f}  c={r[1]:+4d}  win={r[2]:5d}  peak={r[3]:.4f}  mean={r[4]:.4f}")
    b = best[0]
    print(f"\nBEST: SSB center bin c={b[1]:+d}  score={b[0]:.2f}  at window {b[2]} "
          f"({b[2]*FFT/SRATE*1e3:.3f} ms)")
    print(f"      => pss_bin_shift (SSB-center offset from carrier) = {b[1]:+d} "
          f"= {b[1]*30e3/1e6:+.2f} MHz   [took {time.time()-t0:.1f}s]")

if __name__ == "__main__":
    main()