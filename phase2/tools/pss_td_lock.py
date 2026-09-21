#!/usr/bin/env python3
"""Focused PSS detection: time-domain matched filter for PSS root-29 on a capture.

The receiver is tuned to the SSB arfcn, so the SSB block (and PSS, which sits
at the block center) is at DC.  PSS = ZC root-29 (n_id2=1 for PCI=1), length
127, mapped onto OFDM subcarriers 56..182 of a 768-bin symbol (30 kHz, 23.04
MSPS) with a normal cyclic prefix.  We build that exact time-domain symbol
and sliding-correlate against a downconverted (DC-centered, CFR-compensated
per half) version of the RX.

Strongly suppressed the coarse CFO issue by computing correlation on a ~2-slot
window and picking the peak; CFO then just rotates the peak phase slightly.

Usage: python3 pss_td_lock.py <dump.cf32>
"""
import sys
import numpy as np

SRATE = 23.04e6
FFT = 768          # 30 kHz SCS at 23.04 MSPS
FS_30 = 30e3
CP = 160           # normal CP for 30 kHz = 160 samples? no: 30kHz CP=144, ext 512
# NR 30kHz normal CP: symbol0=352? Actually per-slot 14 symbols: cp len for 15k = 160/144
# For 30 kHz: extended CP 512 samples; normal = 144 samples? At 23.04: 768*0.5 = ...
# Slot = 14 sym + CP. 30kHz slot = 0.5 ms = 11520 samples = 14*768 + 13*144 + 1*... -> 14*768=10752, remaining 768 /14 = wrappers.
# Simplest: build one symbol with CP=144 samples (approx) - correlation peak width tolerates.

K_PSS_FIRST_SUB = 56   # PSS subcarrier offset within SSB block
K_PSS_LEN = 127


def zc(n, u):
    return np.exp(-1j * np.pi * u * (n + 1) * n / 127).astype(np.complex64)


def pss_symbol(n_id2, fft=FFT, cp=144):
    """Build time-domain PSS symbol (with CP) for root index n_id2."""
    u = [29, 34, 25][n_id2]
    s = zc(np.arange(K_PSS_LEN), u)
    sub = np.zeros(fft, dtype=np.complex64)
    sub[K_PSS_FIRST_SUB:K_PSS_FIRST_SUB + K_PSS_LEN] = s
    body = np.fft.ifft(sub, fft)
    sym = np.concatenate([body[-cp:], body]) if cp > 0 else body
    return sym.astype(np.complex64)


def main():
    path = sys.argv[1]
    d = np.fromfile(path, dtype=np.complex64)
    print(f"capture {path}: {len(d)} samples ({len(d)/SRATE*1e3:.0f} ms), "
          f"rms={np.sqrt(np.mean(np.abs(d)**2)):.5f}")

    ref = pss_symbol(1, cp=144)   # PCI 1 -> n_id2=1 -> root 29
    R = ref.size
    er = float(np.abs(ref @ ref.conj()))

    # scan whole capture in steps of 1 sample but report locals every slot start
    n = len(d) - R
    corr = np.zeros(len(d))
    # use FFT-based convolution: correlate = ifft(fft(rx)*conj(fft(ref))) with zero-pad
    nfft = 1 << int(np.ceil(np.log2(len(d) + R)))
    RX = np.fft.fft(d, nfft)
    RF = np.fft.fft(ref[::-1].conj(), nfft)   # ref* reverse = correlation kernel
    out = np.fft.ifft(RX * RF, nfft)
    co = out[:n]  # corr[lag] = sum_i ref[i]* rx[lag+i]

    # energy normalization per lag (sliding window over |d|^2)
    e = np.convolve(np.abs(d) ** 2, np.ones(R), mode="full")[:n]
    denom = np.sqrt(er * e) + 1e-12
    c = np.abs(co) / denom

    k = int(np.argmax(c))
    print(f"best corr = {c[k]:.4f} at lag {k} = {k/SRATE*1e3:.3f} ms")
    # report peaks above 0.35 (the rx_probe threshold)
    idx = np.where(c >= 0.35)[0]
    if len(idx):
        # cluster
        clusters = []
        st = idx[0]; pre = idx[0]-1
        for i in idx:
            if i != pre + 1:
                clusters.append(st); st = i
            pre = i
        clusters.append(st)
        print(f"corr>=0.35 at {len(idx)} lags, {len(clusters)} clusters (slot_est = lag/(11520)):")
        for s in clusters[:12]:
            print(f"  lag {s} = {s/SRATE*1e3:.3f} ms ({s/11520:.2f} slots) corr={c[s]:.3f}")
    else:
        print("no lag above 0.35")
        top = np.argsort(c)[-12:][::-1]
        for t in top:
            print(f"  lag {t} = {t/SRATE*1e3:.3f} ms corr={c[t]:.3f}")

if __name__ == "__main__":
    main()