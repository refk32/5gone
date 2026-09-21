#!/usr/bin/env python3
"""rx_probe_psd.py — absolute level time/frequency map of a cf32 capture.

Windows the complex64 capture into 768-sample (one 30 kHz-SCS symbol) chunks,
removes the per-window DC offset (kills the B210 DC-leakage tone that used to
mask everything), and prints, per 5 ms block, the absolute noise floor and the
energy in two candidate PSS bands:
  A) around the DL carrier        (~3489.4 MHz)  -> SSB at cell center
  B) at the gNB's real SSB offset (default -5.58 MHz => band -4.6..-6.5 MHz)
     (srsRAN ssb_arfcn 632256 = 3483.84 MHz = carrier - 5.58 MHz, NOT -7.5)
Values are dBFS (full-scale sine = 0 dBFS); B210 noise floor ~ -80..-100 dBFS.
A CW/PSS window ON shows A (or B) jumping 20-40 dB above floor for its 0.4 s.

usage: python3 rx_probe_psd.py <file.cf32> [srate=23.04e6] [fc_mhz=3489.42] [stride=5] [ssb_off_mhz=-5.58]
"""
import sys
import numpy as np

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/real_gnb.cf32"
    srate = float(sys.argv[2]) if len(sys.argv) > 2 else 23.04e6
    fc_mhz = float(sys.argv[3]) if len(sys.argv) > 3 else 3489.42
    stride = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    ssb_off_mhz = float(sys.argv[5]) if len(sys.argv) > 5 else -5.58

    W = 768
    iq = np.fromfile(path, dtype=np.complex64)
    n = (len(iq) // W) * W
    iq = iq[:n]
    print(f"{path}: {n} samples = {n/srate*1e3:.1f} ms")

    x = iq.reshape(-1, W)
    x = x - x.mean(axis=1, keepdims=True)
    win = np.hanning(W).astype(np.float32)
    k = np.fft.fftfreq(W, 1.0 / srate)
    fbin = fc_mhz + k / 1e6
    neg = k < 0
    A = np.abs(k/1e6) <= 1.0
    B = (k/1e6 >= ssb_off_mhz - 0.9) & (k/1e6 <= ssb_off_mhz + 0.9)

    blocks = x.shape[0] // 150
    print(f"  t_ms  floor(dBFS)  f_peak(MHz)  p_dBFS   A@+0   B@{ssb_off_mhz:.2f}  (dBFS, max in band)")
    for b in range(0, blocks, stride):
        blk = x[b*150:(b+1)*150]
        X = np.fft.fft(blk * win, axis=1)
        P = (np.abs(X) / win.sum()) ** 2
        dB = 10 * np.log10(2 * P + 1e-12)          # dBFS per bin
        seq = dB.mean(axis=0)
        floor = np.median(seq[~A & ~B & ~neg])
        bp = int(np.argmax(seq))
        pA = float(np.max(seq[A]))
        pB = float(np.max(seq[B]))
        print(f"{b*5:6d}  {floor:8.1f}  {fbin[bp]:10.3f}  {seq[bp]:7.1f}  {pA:6.1f} {pB:6.1f}")

if __name__ == "__main__":
    main()