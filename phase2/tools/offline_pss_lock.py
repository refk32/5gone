#!/usr/bin/env python3
"""offline PSS lock — find the real SSB bin shift in a captured cf32 dump.

Fast scan: FFT each 768-sample window once, then for each candidate bin shift
just look at the power in that 127-bin band (no re-FFT per shift).  SSB slots
repeat every 40 slots (20 ms); we score the per-slot band-energy contrast
(peak vs median) exactly like the srsRAN cell's DDDUU pattern.

Usage: python3 offline_pss_lock.py <dump.cf32> [bin_shifts=auto]
  auto = every 12 bins from -372 to +372 (63 candidates)
"""
import sys
import numpy as np

def main():
    path = sys.argv[1]
    if len(sys.argv) > 2 and sys.argv[2] != "auto":
        cand = [int(v) for v in sys.argv[2].split(",")]
    else:
        cand = list(range(-372, 373, 12))
    srate = 23.04e6
    scs = 30e3
    fft = int(round(srate / scs))          # 768
    win = np.hanning(fft).astype(np.float32)
    win = win / win.sum()

    d = np.fromfile(path, dtype=np.complex64)
    n = (len(d) // fft) * fft
    x = d[:n].reshape(-1, fft)
    x = x - x.mean(axis=1, keepdims=True)
    print(f"capture {path}: {len(d)} samples ({len(d)/srate*1e3:.0f} ms), rms={np.sqrt(np.mean(np.abs(d)**2)):.4f}")

    X = np.fft.fft(x * win, axis=1)        # one FFT for everything
    P = np.abs(X) ** 2
    # normalize per-window (AFC/AGC invariance): band power is relative anyway
    k = np.fft.fftfreq(fft, 1.0 / srate)

    slot = int(srate * 0.5e-3)             # 11520 samples
    nwin_slot = slot // fft                # 15
    nslots = P.shape[0] // nwin_slot

    # Precompute, for each candidate shift, the per-window PSS-band power: sum
    # of |X|^2 over the 127 bins starting at `bs` (wrapped circularly).
    best = []
    for bs in cand:
        idx = (np.arange(127) + bs) % fft   # PSS bins for this shift
        band_pow = P[:, idx].sum(axis=1)    # per-window band power
        m = band_pow[: nslots * nwin_slot].reshape(nslots, nwin_slot)
        sym_mean = m.mean(axis=1)           # per-slot mean band power
        base = np.median(sym_mean)
        peak_slot = int(np.argmax(sym_mean))
        snr = float(sym_mean[peak_slot]) / (base + 1e-12)
        best.append((snr, bs, peak_slot, float(sym_mean[peak_slot]), float(base)))

    best.sort(reverse=True)
    print("\ntop candidates (snr, bin_shift, slot_index, ssb_energy, base):")
    for r in best[:10]:
        print(f"  snr={r[0]:7.1f}  shift={r[1]:+4d}  slot={r[2]:4d}  e={r[3]:.5e}  base={r[4]:.5e}")
    b = best[0]
    print(f"\nBEST: pss_bin_shift={b[1]:+d}  snr={b[0]:.1f}  at slot {b[2]} "
          f"({b[2]*0.5:.0f} ms -> carrier offset {b[1]*scs/1e6:+.2f} MHz)")

if __name__ == "__main__":
    main()