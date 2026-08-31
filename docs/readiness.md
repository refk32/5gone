# Readiness Assessment — 5Gone Lab Code

**Target:** Ubuntu 22.04 + USRP B210 native (no Docker)  
**Goal:** Minimize install failure at office  
**Updated:** static audit — install belum dijalankan di Linux kantor

---

## Score summary

| Area | Ready | Notes |
|------|-------|-------|
| Phase 0 — dataset analysis | **95%** | Scripts tested, dataset cloned |
| Phase 1 — sniffer + 5G lab | **85%** | RAR log parser + grant bus; gNB untested Linux |
| Phase 2 — overshadow attack | **85%** | sim/inject/bus/live + full pipeline demo |
| Hardware docs | **90%** | BOM + RF tent checklist |
| Install automation | **80%** | preflight + verify + install-all |

**Overall code readiness for office install: ~90%**  
**Overall PoC readiness (end-to-end attack): ~70%** — full pipeline demo ready; RF needs 2nd B210 + tent

---

## Siap pakai (bisa commit & bawa ke kantor)

| Item | Status |
|------|--------|
| `scripts/validate-repo.sh` | Static check — jalan di Mac/Linux |
| `scripts/preflight.sh` | Cek OS, CPU AVX2, RAM, disk |
| `scripts/install-all.sh` | Urutan install terstruktur |
| `scripts/tune-usb-b210.sh` | udev + USB tips |
| `scripts/benchmark-b210.sh` | UHD stability test |
| `config/srsran/gnb_20mhz.yml` | **Fixed** — srsRAN official B200 n78 20MHz |
| `config/b210.uhd.args` | **Fixed** — srate 23.04 MSPS aligned |
| `phase0/parse_attacker_logs.py` | Tested — 52 RAR attacks parsed |
| `phase1/sniffer/` | RAR parser + grant bus — 53 grants from dataset |
| `scripts/start-sniffer.sh` | Live tail + replay modes |
| `scripts/run-full-demo.sh` | End-to-end pipeline demo |
| `scripts/start-attack-lab.sh` | One-command attack lab |
| `scripts/run-attack-rar-dos.sh` | One-command attack demo |
| `scripts/install-phase2.sh` | Build attacker + IQ templates |
| `docs/phase2-attack.md` | Attack procedure + lab topology |
| `docs/office-install-runbook.md` | Step-by-step kantor |
| `docs/equipment-list.md` | BOM hardware |

---

## Blocker yang sudah diperbaiki (vs versi sebelumnya)

| Was broken | Fixed |
|------------|-------|
| `amf.addr: open5gs` (Docker hostname) | → `127.0.0.5` Open5GS default |
| `srate: 30.72` mismatch srsRAN B200 | → `23.04` per official config |
| Missing `num_recv_frames` in gNB | → 512 in device_args |
| Open5GS start manual wrong `-c` paths | → systemd + PREFIX fallback |
| No preflight/verify | → added |
| MongoDB package wrong name | → mongodb-org 8.0 official |
| `config/open5gs/native/*` missing | → amf.yaml.patch + patch script |

---

## Masih belum ada (expected gap)

| Gap | Impact | When |
|-----|--------|------|
| **Live PDCCH RAR decode (RF)** | sniffer via gNB log; parser tuned for dataset format | Phase 2b |
| **2nd USRP B210** | gNB + attacker simultaneous | Buy/rent 2nd unit |
| RF tent + PA + phone | Cannot TX test | Buy hardware |
| Open5GS tested on your Linux | Unknown until office | Day 1 kantor |
| srsRAN build on your CPU | ~30-60 min first build | Day 1 kantor |
| srsRAN live MAC log format | May differ from 5gone enb-export — parser tweak after gNB up | Day 1 kantor |

---

## Checklist sebelum ke kantor

### Di Mac sekarang
- [ ] `bash scripts/validate-repo.sh` → PASS
- [ ] Git commit / push `poc/5gone/`
- [ ] Baca `docs/office-install-runbook.md`

### Hardware bawa ke kantor
- [ ] USRP B210 + USB3 cable
- [ ] RF shield tent (before TX)
- [ ] Attenuators 30 dB
- [ ] Test phone + lab SIM (later)

### Linux kantor minimum
- [ ] Ubuntu 22.04 x86_64
- [ ] 8+ GB RAM, 20 GB disk free
- [ ] CPU AVX2
- [ ] USB3 port motherboard (bukan hub)
- [ ] Internet (git clone)

---

## Day-1 office procedure

```
validate-repo → preflight → install-all → verify-lab
→ verify-phase1 → verify-phase2 → run-full-demo
→ start-5g-native core → gnb → start-sniffer tail → run-attack-rar-dos bus
```

Stop jika benchmark overruns > 0 — jangan lanjut ke gNB.

---

## Honest note on "0% failure"

True 0% impossible tanpa test di hardware yang sama. Yang achievable:

1. **Static validation** — 0 failures (`validate-repo.sh`)
2. **Config alignment** — gNB/Open5GS PLMN/TAC/srate matched to upstream docs
3. **Ordered install** — satu script, satu urutan
4. **Pass gates** — benchmark + verify before RF

Remaining risk: USB stability (B210), srsRAN first-build time, band n78 regulatory in tent.

**Full risk matrix + error fixes:** [README.md § Risks & troubleshooting](../README.md#risks--troubleshooting)
