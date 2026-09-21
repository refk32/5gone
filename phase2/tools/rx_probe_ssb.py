#!/usr/bin/env python3
"""rx_probe_ssb.py — locate the SSB in a cf32 capture by burstiness.

The SSB (SSS/PSS/DMRS, 240 SC at 30 kHz) is transmitted periodically (every
5/10/20 ms) on the first symbols of its slots, while PDSCH/PDCCH fill the DL
continuously. So SSB subcarriers are the bins whose power varies strongly over
time (max/mean >> 1), clustered near the SSB center.

Prints, per slot-window (768-sample FFT every 11520 samples = 0.5 ms):
  - the bursty-bin cluster center -> SSB center frequency (MHz)
  - the PSS bin offset = SSB_center_bin - 119 (== 119 is the SSB's own center;
    our fixed pss ref centers PSS at bin ~119 of the FFT). That delta is the
    frequency shift pss_time_reference must apply around carrier for a hit.

usage: python3 rx_probe_ssb.py <file.cf32> [sample_rate=23.04e6] [carrier_mhz]
"""
import sys
import numpy as np

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/real_gnb.cf32"
    srate = float(sys.argv[2]) if len(sys.argv) > 2 else 23.04e6
    fc_mhz = float(sys.argv[3]) if len(sys.argv) > 3 else 3489.42

    W = 768
    stride = 11520          # 0.5 ms slot at 30 kHz SCS (14 syms + CPs)
    iq = np.fromfile(path, dtype=np.complex64)
    n = (len(iq) // stride) * stride
    iq = iq[:n]
    nwin = n // stride
    print(f"{path}: {n} samples = {n/srate*1e3:.0f} ms, {nwin} slot-windows")

    m = iq[:nwin * W].reshape(nwin, W)
    win = np.hanning(W).astype(np.float32)
    spec = np.abs(np.fft.fft(m * win, axis=1)) ** 2
    k = np.fft.fftfreq(W, 1.0 / srate)

    mean_p = spec.mean(axis=0)
    floor = np.percentile(mean_p, 5)
    burst = spec.max(axis=0) / (mean_p + 1e-30)

    hot = np.where((burst > 3.0) & (mean_p > 3 * floor))[0]

    # Signed bin (b->b-W above the Nyquist edge) so bins past W//2 map to the
    # negative-frequency half. The real SSB below the carrier (e.g. 5.58 MHz ->
    # bin 582 -> signed -186) must NOT be filtered out.
    def signed(b):
        return int(b) if int(b) < W // 2 else int(b) - W

    best_burst, best_bin_idx = 0.0, -1
    print("  bin   f(MHz)   burst   mean(dB rel)  span(+-3)")
    for i in range(len(hot)):
        b = signed(hot[i])
        span = sum(1 for j in hot if -3 <= signed(j) - b <= 3)
        mdb = 10*np.log10(mean_p[int(hot[i])] / floor + 1e-12)
        print(f"{b:5d}  {fc_mhz + k[int(hot[i])]/1e6:8.3f}  {burst[int(hot[i])]:5.1f}  {mdb:8.1f}  {span}")
        if span >= 4 and burst[int(hot[i])] > best_burst:
            best_burst, best_bin_idx = burst[int(hot[i])], int(hot[i])

    if best_bin_idx < 0:
        print("NO bursty SSB cluster found: no periodic SSB above the noise floor.")
        print("Either the gNB is not radiating, or the RX freq/antenna is wrong.")
        return

    # Burst-weighted centroid of the hot cluster within one PSS half-span
    # (~63 bins) of the peak-hot bin. More robust than trusting the single
    # max-burst bin, which a noise spike can hijack.
    hub = signed(best_bin_idx)
    cl = [(signed(j), burst[int(j)]) for j in hot
          if abs(signed(j) - hub) <= 63]
    wsum = sum(w for _, w in cl)
    center = int(round(sum(b * w for b, w in cl) / wsum)) if wsum > 0 else hub
    ssb_off_bins = center           # signed bin == offset from DC == offset from carrier
    print(f"\nSSB burst cluster center: bin {center} (signed) -> {fc_mhz + k[best_bin_idx]/1e6:.3f} MHz")
    print(f"SSB center offset from carrier: {ssb_off_bins:+d} bins ({ssb_off_bins*0.03:+.2f} MHz)")
    print(f"-> pass that signed bin offset as rx_probe's pss_bin_shift (e.g. -186)")

if __name__ == "__main__":
    main()
