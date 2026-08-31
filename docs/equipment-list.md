# 5Gone PoC — Daftar Peralatan (BOM)

Target: uplink overshadowing lab dengan **USRP B210**, cell **20 MHz**, shielded environment.

---

## Yang sudah kamu punya

| Item | Qty | Catatan |
|------|-----|---------|
| USRP B210 | 1 | gNB radio — **attacker needs 2nd B210 for RF TX** |
| USB 3.0 cable (included) | 1 | Wajib port USB3 dedicated, bukan hub murah |

---

## Tier 1 — Minimal lab (Phase 0–1, passive only)

Software + RF dasar. **Estimasi: $200–600**

| Item | Qty | Fungsi | Contoh produk |
|------|-----|--------|---------------|
| Linux host (x86) | 1 | srsRAN + UHD — **Mac tidak ideal** | Ubuntu 22.04 PC/mini PC, atau VM di Mac |
| Omni antenna 2.4–6 GHz | 1 | RX awal / dekat gNB | Ettus VERT2450, atau 3–3.8 GHz omni |
| SMA cables (low loss) | 2–3 | B210 ↔ antenna/attenuator | LMR-200, ≤1 m untuk lab dekat |
| Fixed attenuator 30–40 dB | 2 | Safety: kurangi power sebelum RX | Mini-Circuits, SMA |
| Variable attenuator 0–60 dB | 1 | Fine-tune power overshadow | Optional Phase 2 |
| RF shield box / tent kecil | 1 | **Wajib sebelum TX attack** | Ramsey ST-3000, atau DIY aluminium box + SMA feedthrough |
| Test smartphone | 1 | Victim UE (Phase 2+) | Android unlocked preferred (ADB flight mode toggle) |
| Programmable / test SIM | 1–2 | Attach ke lab core | sysmocom, atau SIM operator untuk lab |

---

## Tier 2 — Overshadow lab (Phase 2, TX attack)

Tambahan untuk overshadow Msg3. **Estimasi: +$400–1200**

| Item | Qty | Fungsi | Contoh produk |
|------|-----|--------|---------------|
| **USRP B210 (2nd unit)** | 1 | Attacker TX/RX — gNB pakai B210 #1 | Ettus B210 |
| Power amplifier 3.3–3.8 GHz | 1 | B210 ~10 dBm → cukup > HP (~23 dBm ERP dekat) | Mini-Circuits ZHL-33-S+, atau PA 5G n78 kit |
| Band-pass filter n78 | 1 | Spurious suppression | 3300–3800 MHz BPF |
| Directional antenna n78 | 1 | Fokus ke gNB/UE dalam tent | Panel 3.5 GHz 8–12 dBi |
| RF power meter / SDR RSSI | 1 | Verifikasi TX level | Mini-Circuits PWR-6RMS, atau known-path loss |
| Second SDR RX (optional) | 1 | Sniff uplink victim (SUCI attack) | B210 channel 2, atau RTL-SDR backup |
| GPSDO (optional) | 1 | Sync frequency stabil | Leo Bodnar, atau B210 internal TCXO dulu |

---

## Tier 3 — Full 5G SA core (gNodeB + 5GC)

Software open-source stack. **Estimasi: $0 software + Tier 1–2 hardware**

| Komponen | Software | Fungsi |
|----------|----------|--------|
| gNodeB 20 MHz | **srsRAN Project** | Base station lab |
| 5G Core | **Open5GS** | AMF/SMF/UPF — attach UE |
| UE simulator (optional) | srsRAN UE / Amarisoft | Scale test tanpa banyak HP |
| Orchestration | Native scripts | `start-5g-native.sh` — no Docker |

**Alternatif komersial (paper):**

| Item | Estimasi harga | Kapan dipakai |
|------|----------------|---------------|
| Amarisoft Callbox + UE sim | $$$$ (license) | Production-like, paper pakai ini |
| srsRAN + Open5GS | Gratis | PoC B210 — **recommended** |

---

## Tier 4 — Upgrade path (100 MHz, paper-identical)

**Estimasi: +$3000–8000** — bukan untuk B210 phase awal.

| Item | Fungsi |
|------|--------|
| USRP X310 + PCIe | 184 MSPS, 100 MHz |
| Octoclock-G | 10 MHz ref |
| Boostel PA/LNA frontend | Paper setup |
| AMD Ryzen 7950X+ | Sub-500 µs processing |

---

## Shopping checklist (print)

```
Phase 0 (software only)
[ ] Linux Ubuntu 22.04 (bare metal atau VM)
[ ] Git, Python 3.10+

Phase 1 (passive RF)
[ ] Omni antenna + SMA cables
[ ] 30 dB attenuators (×2)
[ ] RF shield tent/box
[ ] Test phone + lab SIM

Phase 2 (overshadow)
[ ] PA 3.5 GHz (+20 dB min)
[ ] BPF n78
[ ] Variable attenuator
[ ] Power measurement

5G stack (native Linux)
[ ] bash scripts/install-open5gs.sh
[ ] bash scripts/install-srsran.sh
[ ] bash scripts/tune-usb-b210.sh
```

---

## Yang TIDAK perlu di awal

- Amarisoft (mahal — srsRAN cukup)
- X310 (upgrade nanti)
- Spectrum analyzer $10k (nice-to-have)
- Faraday room full-size (tent kecil cukup untuk PoC)

---

## Legal / safety

- RF tent **wajib** sebelum PA aktif
- Cek regulasi setempat (Kominfo / ISM / experimental license)
- Jangan TX di band operator tanpa izin
- Max power dalam tent: hitung path loss + legal EIRP limit
