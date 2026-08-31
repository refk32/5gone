# 5Gone PoC — Uplink Overshadowing Lab

Paper: [5Gone: Uplink Overshadowing Attacks in 5G-SA](https://arxiv.org/html/2602.10272v1) (arXiv:2602.10272)

Reimplementation lab for **RAR DoS** (Section 4.1) on srsRAN + Open5GS + USRP B210.

**Docs:** [office runbook](docs/office-install-runbook.md) · [lab setup](docs/lab-setup.md) · [equipment BOM](docs/equipment-list.md) · [Phase 2 attack](docs/phase2-attack.md) · [readiness](docs/readiness.md)  
**Risks & fixes:** [README § Risks & troubleshooting](#risks--troubleshooting)  
**Cursor skill:** `kaito-5gone`

---

## Setup Guide

### Prerequisites

| Item | Required for |
|------|--------------|
| Ubuntu 22.04 x86_64 (AVX2) | Phase 1+ install & gNB |
| USRP B210 #1 | srsRAN gNB |
| USRP B210 #2 | Attacker RF TX (optional for offline demo) |
| USB 3.0 port (direct, no hub) | B210 stability |
| RF shield tent + PA + attenuators | RF attack only |
| Test phone + lab SIM | UE attach test |

Mac: Phase 0–2 **offline demo** only. Full RF stack → Linux.

---

### Path A — Mac / no hardware (now)

```bash
cd poc/5gone

bash scripts/validate-repo.sh           # static check — must PASS
bash scripts/setup-lab.sh --phase 0     # clone dataset + reports
bash scripts/setup-lab.sh --phase 2     # verify sniffer + attacker + full demo

# Or individually:
bash scripts/verify-phase1.sh           # sniffer: 53 RAR parsed
bash scripts/verify-phase2.sh           # attacker + IQ templates
bash scripts/run-full-demo.sh           # sniffer → bus → attacker pipeline
bash scripts/run-attack-rar-dos.sh demo # RAR DoS replay (10 events)
```

---

### Path B — Linux kantor (full install)

```bash
cd poc/5gone

# 1. Validasi
bash scripts/preflight.sh               # OS, CPU, RAM, disk

# 2. Install everything (recommended — one command)
bash scripts/install-all.sh
# Includes: deps → USB tune → benchmark → Open5GS → srsRAN → verify-lab
#           → Phase 2 attacker build → Phase 1 sniffer verify

# 3. Start 5G lab
bash scripts/patch-open5gs-plmn.sh
bash scripts/start-5g-native.sh core    # MongoDB + Open5GS
bash scripts/add-test-subscriber.sh
bash scripts/start-5g-native.sh gnb     # srsRAN + B210 #1
bash scripts/start-5g-native.sh status

# 4. Attack demo (offline first)
bash scripts/run-full-demo.sh
bash scripts/run-attack-rar-dos.sh demo

# 5. Live attack lab (needs B210 #2 + RF tent)
bash scripts/start-attack-lab.sh up       # gNB + sniffer + bus attacker
bash scripts/start-attack-lab.sh status
bash scripts/start-attack-lab.sh down
```

Manual step-by-step: [docs/office-install-runbook.md](docs/office-install-runbook.md)

---

### Pass gates (wajib sebelum RF)

| Gate | Command | Pass |
|------|---------|------|
| Repo | `bash scripts/validate-repo.sh` | exit 0 |
| Preflight | `bash scripts/preflight.sh` | exit 0 |
| USB | `bash scripts/benchmark-b210.sh` | overruns = 0 |
| 5G stack | `bash scripts/verify-lab.sh` | exit 0 |
| Sniffer | `bash scripts/verify-phase1.sh` | ≥10 RAR grants |
| Attacker | `bash scripts/verify-phase2.sh` | exit 0 |
| Pipeline | `bash scripts/run-full-demo.sh` | 5 grants processed |

---

### Attack modes

| Mode | Command | Hardware |
|------|---------|----------|
| Full pipeline demo | `bash scripts/run-full-demo.sh` | None |
| Sim | `bash scripts/run-attack-rar-dos.sh demo` | None |
| Bus (live sniffer) | `bash scripts/run-attack-rar-dos.sh bus` | gNB + B210 #2 |
| Inject (known grant) | `bash scripts/run-attack-rar-dos.sh inject` | B210 #2 + PA + tent |
| Sniffer only | `bash scripts/start-sniffer.sh tail` | gNB running |
| Sniffer replay | `bash scripts/start-sniffer.sh replay` | None (dataset) |

Grant bus: `/tmp/5gone_grants.jsonl` · Attacker log: `/tmp/5gone_attacker.log` · gNB MAC log: `/tmp/gnb_5gone.log`

---

### Key config (jangan ubah sembarangan)

| File | Purpose |
|------|---------|
| `config/srsran/gnb_20mhz.yml` | gNB — n78 20 MHz, srate **23.04** MSPS |
| `config/b210.uhd.args` | UHD args — must match gNB srate |
| `phase2/config/rar_dos.yaml` | Attacker radio + timing |
| `config/open5gs/amf.yaml.patch` | PLMN 001/01 TAC 1 |

---

### Project layout

```
poc/5gone/
├── config/           # gNB, UHD, Open5GS
├── phase0/           # Dataset analysis
├── phase1/sniffer/ # RAR log parser → grant bus
├── phase2/           # RAR DoS attacker (C++ + Python)
├── scripts/          # install, start, verify, attack
├── docs/             # Runbooks & BOM
├── data/             # 5gone dataset (gitignored)
├── build/            # srsRAN + Open5GS + attacker (gitignored)
└── run/              # Runtime logs & PIDs (gitignored)
```

---

## Risks & troubleshooting

Semua risiko known + antisipasi jika error di kantor. **Stop dan fix sebelum lanjut RF** jika gate gagal.

### Diagnostic cepat

```bash
# Status semua komponen
bash scripts/start-5g-native.sh status
bash scripts/start-attack-lab.sh status

# Log paths
tail -f /tmp/gnb_5gone.log          # srsRAN MAC/RAR (sniffer baca ini)
tail -f run/gnb.log                 # gNB startup / UHD errors
tail -f /tmp/5gone_attacker.log     # attacker actions
cat /tmp/5gone_grants.jsonl         # grant bus (sniffer → attacker)

# Re-verify dari awal
bash scripts/validate-repo.sh && bash scripts/verify-phase1.sh && bash scripts/verify-phase2.sh
```

---

### A. Install & environment

| Risk | Gejala / error | Penyebab | Solusi |
|------|----------------|----------|--------|
| Mac untuk Phase 1+ RF | `Requires Linux` | srsRAN/UHD gNB tidak jalan di macOS | Pakai Ubuntu 22.04 bare metal atau VM + USB passthrough |
| CPU tanpa AVX2 | srsRAN build fail / illegal instruction | CPU terlalu lama | `grep avx2 /proc/cpuinfo` — ganti mesin atau build flags (advanced) |
| Preflight FAIL | `preflight.sh` exit 1 | RAM/disk/OS salah | Fix sesuai output; min 8 GB RAM, 20 GB disk |
| MongoDB gagal start | `mongod` not running | Package salah / service off | `sudo systemctl start mongod`; reinstall via `install-open5gs.sh` |
| Open5GS tidak connect | `open5gs-amfd` down | Install incomplete | `bash scripts/install-open5gs.sh`; cek `build/open5gs/` |
| srsRAN build lama / fail | cmake error, OOM | First build 30–60 min | `JOBS=2 bash scripts/install-srsran.sh`; pastikan RAM cukup |
| Phase 2 C++ build fail | `5gone-rar-dos` missing | UHD/yaml-cpp not installed | `bash scripts/install-deps.sh` lalu `bash scripts/install-phase2.sh` |
| Dataset missing | `verify-phase1` / demo fail | `data/5gone-dataset` belum clone | `bash scripts/setup-lab.sh --phase 0` |

---

### B. USRP B210 & USB

| Risk | Gejala / error | Penyebab | Solusi |
|------|----------------|----------|--------|
| B210 not found | `uhd_find_devices` kosong | USB2 / hub / kabel | Colok USB3 langsung ke motherboard; ganti kabel/port |
| UHD overruns | `Num overruns > 0` di benchmark | USB bandwidth / latency | `bash scripts/tune-usb-b210.sh`; port USB3 lain; **jangan lanjut ke gNB** |
| Overruns persist | Masih overrun setelah tune | Rate terlalu tinggi | `RATE=15.36e6 bash scripts/benchmark-b210.sh` — update `gnb_20mhz.yml` + `b210.uhd.args` **bersamaan** |
| gNB UHD error | `run/gnb.log` O / async | Buffer kecil / USB | Pastikan `num_recv_frames=512` di gNB config; re-run tune |
| Satu B210 untuk gNB + attacker | Device busy / conflict | Satu SDR tidak bisa dua role simultan | **Wajib B210 #2** untuk attacker; atau mode `sim`/`replay` saja |
| FPGA images missing | UHD init fail | Images belum download | `sudo uhd_images_downloader` |
| Docker + B210 | USB drop / overruns | Passthrough tidak stabil | **Jangan pakai Docker** — native Linux only (sudah dihapus dari stack) |

---

### C. 5G stack (Open5GS + srsRAN)

| Risk | Gejala / error | Penyebab | Solusi |
|------|----------------|----------|--------|
| gNB tidak connect AMF | NGAP timeout di log | PLMN/TAC/AMF addr salah | `bash scripts/patch-open5gs-plmn.sh`; cek `amf.addr: 127.0.0.5` di gnb yaml |
| Phone tidak attach | No 5G icon / no service | IMSI tidak di DB / band mismatch | `bash scripts/add-test-subscriber.sh`; phone harus support n78 |
| PLMN mismatch | UE reject | gNB vs Open5GS vs SIM beda | Semua harus `001/01`, TAC `1` — lihat `amf.yaml.patch` |
| Sample rate mismatch | gNB crash / UHD error | Config tidak aligned | **23.04 MSPS** di gNB, `b210.uhd.args`, attacker yaml — jangan campur 30.72 |
| gNB log kosong | `/tmp/gnb_5gone.log` tidak ada | gNB belum start / path salah | `bash scripts/start-5g-native.sh gnb`; cek `run/gnb.log` untuk error startup |
| Wrong log untuk sniffer | Sniffer tidak detect RAR | Tail log yang salah | Sniffer harus tail **`/tmp/gnb_5gone.log`** (bukan hanya `run/gnb.log`) |

---

### D. Sniffer & grant bus

| Risk | Gejala / error | Penyebab | Solusi |
|------|----------------|----------|--------|
| Sniffer 0 grants live | Bus kosong saat phone PRACH | srsRAN log format ≠ dataset Amarisoft | Lihat `/tmp/gnb_5gone.log` manual; bandingkan dengan enb-export; update `phase1/sniffer/log_parser.py` |
| Parser works offline only | `verify-phase1` OK, live FAIL | Live log format berbeda | Sementara: `bash scripts/start-sniffer.sh replay` + `inject` mode dengan grant manual |
| Grant bus stale | Attacker TX grant lama | File tidak di-clear | `> /tmp/5gone_grants.jsonl` sebelum test baru |
| Manual grant inject | Perlu debug tanpa sniffer | PDCCH belum live | `python3 phase2/python/grant_bus.py publish --crnti 17922 --freq 2` |
| Sniffer tidak jalan | File not found | gNB belum up | Start gNB dulu; sniffer auto-fallback ke `run/gnb.log` jika MAC log belum ada |

**Workaround jika live sniffer gagal:**

```bash
# 1. Replay dari dataset (bukti pipeline OK)
bash scripts/start-sniffer.sh replay
bash scripts/run-attack-rar-dos.sh bus --dry-run   # Python fallback

# 2. Inject grant known
bash scripts/run-attack-rar-dos.sh inject phase2/config/sample_grant.json
```

---

### E. Attacker & RF overshadow

| Risk | Gejala / error | Penyebab | Solusi |
|------|----------------|----------|--------|
| Overshadow tidak terjadi | UE attach normal | Power attacker < UE | Tambah PA; kurangi attenuator; B210 solo ~10 dBm **tidak cukup** |
| gNB tidak lihat Msg3 fail | RA success | Timing salah / grant salah | Tune `symbol_advance_us` di `phase2/config/rar_dos.yaml`; cek freq template match |
| IQ template missing | `empty IQ` di attacker log | freq_res tidak ada di templates | `python3 phase2/tools/generate_msg3_template.py`; tambah grant di `DEFAULT_GRANTS` |
| USB latency too high | Msg3 slot missed | B210 USB > paper X310 PCIe | Naikkan `symbol_advance_us`; ukur di log; expect >500 µs — document gap |
| Live PDCCH mode empty | `live` mode no attacks | PDCCH decode belum implemented | Pakai **`bus`** atau **`inject`** mode, bukan `live` |
| Attack di luar tent | Legal / safety | RF leakage | **Wajib RF shield tent** sebelum TX; max attenuation dulu |
| 2 SDR same frequency | Interference | gNB + attacker dekat | Attenuator; directional antenna; power budget di [lab-setup.md](docs/lab-setup.md) |

**RF test urutan aman:**

```
1. Max attenuation (60 dB)
2. bash scripts/run-attack-rar-dos.sh inject --dry-run   # no TX
3. Inject mode TX, monitor /tmp/5gone_attacker.log
4. Kurangi attenuator gradual until gNB logs RA fail
```

---

### F. Hardware & scope gaps (cannot fix with software)

| Risk | Impact | Antisipasi |
|------|--------|------------|
| Hanya 1× B210 | Tidak bisa gNB + attacker RF bersamaan | Offline demo + inject later; sewa/beli B210 #2 |
| No PA | TX power insufficient | Beli PA n78 (+20 dB min) sebelum expect overshadow |
| No RF tent | Cannot TX legally/safely | Jangan TX attack tanpa shield box |
| No n78 phone/SIM | UE tidak attach | Cek band phone; lab SIM via Open5GS subscriber |
| Paper 100 MHz / sub-500 µs | Cannot replicate exact numbers | Target: **concept demo** on 20 MHz; document latency gap |
| 5Gone source not released | No reference binary | Pakai dataset traces + paper Section 5 as spec |

---

### G. Error → action map (decision tree)

```
install-all FAIL
  ├─ preflight FAIL     → fix OS/RAM/disk
  ├─ benchmark overruns → tune-usb-b210.sh, jangan start gNB
  ├─ open5gs FAIL       → install-open5gs.sh, cek mongod
  ├─ srsran FAIL        → install-deps.sh, AVX2, JOBS=2
  └─ phase2 FAIL        → install-deps.sh (libuhd, yaml-cpp)

gNB up tapi phone no attach
  ├─ cek open5gs-amfd  → patch-open5gs-plmn.sh
  ├─ cek subscriber    → add-test-subscriber.sh
  └─ cek band n78       → phone + gNB same band

sniffer no grants
  ├─ cek /tmp/gnb_5gone.log  → ada RAR lines?
  ├─ format beda             → inject mode / replay / update parser
  └─ gNB MAC log level       → mac_level: info di gnb_20mhz.yml

attacker no effect
  ├─ dry-run only?           → remove --dry-run, B210 #2 connected
  ├─ IQ template?            → generate_msg3_template.py
  ├─ power?                  → PA + reduce attenuation
  └─ timing?                 → symbol_advance_us in rar_dos.yaml
```

---

### H. Log files reference

| File | Isi | Dipakai untuk |
|------|-----|---------------|
| `/tmp/gnb_5gone.log` | srsRAN MAC/PHY internal | **Sniffer tail** — cari RAR |
| `run/gnb.log` | gNB process stdout/stderr | UHD errors, startup crash |
| `run/open5gs.log` | Open5GS stdout | AMF/SMF issues |
| `/tmp/5gone_grants.jsonl` | RAR grants JSON | Sniffer → attacker bus |
| `/tmp/5gone_attacker.log` | Attack events | Verify overshadow attempts |
| `logs/phase0-*.txt` | Dataset analysis | Offline baseline timing |

---

### I. Escalation checklist (jika stuck >30 menit)

1. `bash scripts/validate-repo.sh` — masih PASS?
2. `bash scripts/run-full-demo.sh` — pipeline offline masih OK? (isolates software vs RF)
3. Capture: `run/gnb.log`, `/tmp/gnb_5gone.log`, `uhd_find_devices`, benchmark output
4. Jika offline OK tapi live fail → masalah di hardware/config/log format, bukan core code
5. Jika inject mode gagal dengan sample grant → RF/power/timing, bukan parser

Detail RF wiring: [lab-setup.md](docs/lab-setup.md) · Install step-by-step: [office-install-runbook.md](docs/office-install-runbook.md)

---

## What the authors released

| Resource | URL | Use for PoC |
|----------|-----|-------------|
| Attack traces (PHY→NAS) | https://github.com/5gone/dataset | Parse logs, build detector first |
| Full 5Gone source code | **Not released** (dual-use / export control) | Must reimplement from paper |

## Authorization boundary

PoC **only** in:
- Shielded lab (Faraday cage / RF tent)
- Private gNodeB (Amarisoft Callbox, srsRAN gNB)
- Own test UEs / UE simulator
- Operator-written permission for any live cell

**Never** test on public gNodeB without operator IRB approval (paper Section 6.2).

---

## Four attacks (Section 4) — difficulty order

### 1. Cell-Wide DoS (start here) — Section 4.1

**Mechanism:** Sniff RAR on downlink → overshadow UE Msg3 with empty MAC PDU (padding only) on same UL grant, slightly higher TX power.

**Why start here:**
- MAC/PHY only — no NAS encoding
- No UE-specific PDCCH decode needed
- 1 CPU core enough (Table 2)
- E2E latency target: ~272 µs (RAR DoS)

**PoC success criteria:**
- [ ] Decode RAR + extract UL grant from live cell
- [ ] Encode empty MAC PDU PUSCH
- [ ] Transmit within k2 window (<500 µs for k2=1)
- [ ] gNB drops connection; UE retries PRACH then gives up

### 2. Registration Reject Downgrade — Section 4.2

**Mechanism:** Overshadow NAS Service/Registration Request → inject invalid TMSI → AMF sends Registration Reject (#15 or #27) → UE downgrades to LTE.

**Extra requirements:**
- NAS + RRC message encoding (Section 5.5.1)
- UE-specific PDCCH decode (~79 µs avg)
- Parallel PDCCH workers for scale (25 cores for 64 UEs)

### 3. SUCI Extraction — Section 4.3

**Mechanism:** Overshadow Registration Request with invalid TMSI → AMF sends Identity Request → **sniff** UE's Identity Response on uplink.

**Extra requirements:** Uplink decoder (Section 5.4) — decode victim PUSCH, parse NAS Identity Response.

### 4. SUCI Replay — Section 4.4

**Mechanism:** Replay captured SUCI in Registration Request → observe Authentication Response vs Failure on downlink.

**Requires:** Prior SUCI from attack #3 or lab SIM.

---

## Hardware (Section 5.1)

Minimum lab stack mirroring paper:

| Component | Paper uses | Alternatives for lab |
|-----------|-----------|---------------------|
| SDR | USRP X310 + PCIe | USRP B210 (see **B210 constraints** below) |
| CPU | AMD Ryzen 7950X | High-clock x86, symbol-based pipeline |
| PA/LNA | Boostel frontend | Required for overshadowing real UE |
| Sync | Octoclock-G 10 MHz ref | GPSDO or Octoclock |
| Antenna | Panorama WMM8G-7-38 | Directional 5G n78 |
| Band | TDD n78 (3.4–3.8 GHz) | Match your gNB band |
| Sample rate | 184.32 MSPS | For 100 MHz cell |

**Lab gNodeB:** Amarisoft Callbox (paper) or srsRAN Project gNB on n78.

---

## USRP B210 constraints (your hardware)

| Spec | B210 | 5Gone paper | Impact |
|------|------|-------------|--------|
| Max sample rate | 61.44 MSPS | 184.32 MSPS | **Cannot capture full 100 MHz cell** |
| Max RF bandwidth | ~56 MHz | 100 MHz | Must use **≤20 MHz** lab cell |
| Host interface | USB 3.0 | PCIe | **Sub-500 µs latency much harder** — expect overruns |
| TX power | ~10 dBm | X310 + PA | Need **external PA** (+20–30 dB) to overshadow phone |
| Frequency | 70 MHz–6 GHz | n78 (3.4–3.8 GHz) | OK — band supported |
| TDD n78 | Software TDD switching | GPIO PA/LNA switch | Doable in software, tighter timing |

### What you CAN do with B210

| Phase | Feasible? | Notes |
|-------|-----------|-------|
| 0 — Parse 5gone dataset | Yes | No SDR needed |
| 1 — Passive sync + decode | Yes | On **20 MHz** srsRAN/Amarisoft lab cell |
| 2 — RAR DoS overshadow | **Maybe** | 20 MHz cell, shielded lab, PA, aggressive latency tuning |
| 2 — Full 100 MHz 5Gone replica | **No** | Need X310 or similar |
| LTE overshadowing (prior art) | **Yes** | AdaptOver/GLaDoS used 20 MHz LTE — B210 sweet spot |

### Recommended B210 lab config

```
gNB: srsRAN Project, 20 MHz channel BW, FR1 n78
SCS: 30 kHz
Sample rate: 23.04 MSPS (official srsRAN B200 template)
Band: n78 (dl_arfcn 632628 in gnb_20mhz.yml)
Shielding: RF tent mandatory
PA: +20 dB min for overshadow tests
USB: dedicated USB 3.0 port, num_recv_frames=512
Attacker: 2nd B210 on separate USB3 port
```

### B210 UHD stable settings

```bash
# Benchmark first — must pass with zero overruns
bash scripts/benchmark-b210.sh
# Default rate: 23.04e6 (matches config/srsran/gnb_20mhz.yml)

# Device args (config/b210.uhd.args)
type=b200,master_clock_rate=23.04e6,num_recv_frames=512,num_send_frames=512
```

### Realistic B210 PoC goal

Reproduce **attack concept** on reduced bandwidth, not paper-identical numbers:

1. Decode RAR on 20 MHz lab gNB
2. Measure DL→UL reaction time (expect >500 µs on USB — document gap vs paper)
3. Attempt Msg3 overshadow with PA in shielded box
4. Parallel track: build **detector** from 5gone dataset traces

Upgrade path when ready: USRP X310 + PA for 100 MHz / sub-500 µs target.

---

## Software architecture to reimplement (Section 5.2)

Symbol-based pipeline (NOT slot-based like stock OpenAirInterface):

```
IQ samples → Cell Sync (PSS/SSS/MIB) → PDCCH decode → grant extract
                                              ↓
                                    Attack logic (which Msg3/NAS to inject)
                                              ↓
                                    PUSCH encode → IFFT → Radio TX
```

Key modules:
1. **Radio adapter** — bridge fixed chunk size ↔ symbol timestamps
2. **Cell sync** — find/track, FFT per symbol (~36 µs)
3. **PDCCH decoder** — DCI → UL/DL grants (srsRAN polar decode)
4. **PUSCH encoder** — adversarial payload on grant resources
5. **Timing advance** — independent TA from victim UE (Section 5.5.3)

Building blocks: **srsRAN** (PDCCH polar, PUSCH), custom symbol scheduler.

Stock OpenAirInterface UE: k2 ≥ 3 slots → **too slow** for real 5G-SA (Section 7).

---

## PoC status

| Phase | Status | Command |
|-------|--------|---------|
| 0 — Dataset analysis | Done | `setup-lab.sh --phase 0` |
| 1 — RAR sniffer | Done | `verify-phase1.sh` |
| 2 — RAR DoS attacker | Done (inject/bus) | `verify-phase2.sh` |
| 2b — Live PDCCH decode | WIP | use sniffer + bus mode |
| 3 — NAS attacks | Not started | — |

**PoC success criteria (RAR DoS):**
- [x] Parse RAR + UL grant from logs / sniffer
- [x] Encode empty MAC PDU PUSCH (templates)
- [ ] Transmit within k2 window on live cell (<500 µs — measure on B210)
- [ ] gNB aborts RA; UE cannot attach

---

## Metrics (match paper Section 6)

| Metric | RAR DoS target | Reg Reject target |
|--------|---------------|-------------------|
| E2E DL→UL | ~272 µs | ~368 µs |
| Symbol 0 advance | ~672 µs ahead | ~1524 µs ahead |
| k2=1 support | Yes | Yes (362 µs E2E) |

Use **Tracy** or similar for profiling (paper Section 5.2).

---

## Countermeasures to study (Section 8)

- KPI anomaly: failed RA rate, Identity Request spikes
- UE↔gNB attach transcript comparison at critical stages
- Auth procedure hardening against SUCI replay ([38])

PoC can include a **detector module** alongside attack — stronger research contribution.

---

## Cursor workflow

```
Read poc/5gone/README.md
Load kaito-research-pipeline for trace analysis
For RF code: implement phase-by-phase, lab-only
```

Prompt example:
```
Analyze 5gone dataset trace for RAR DoS — extract timing between RAR and Msg3 slot
```

---

## References in paper

- Prior LTE overshadowing: AdaptOver [15], GLaDoS [14]
- 5G SUCI-catchers (FBS): [11]
- srsRAN: https://github.com/srsran/srsran_project
- OpenAirInterface: https://gitlab.eurecom.fr/oai/openairinterface5g
