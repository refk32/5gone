# Office Install Runbook — Ubuntu 22.04 + B210 (native, no Docker)

Jalankan **di Linux kantor**, B210 colok USB3 langsung ke motherboard.

## Sebelum ke kantor (Mac — sudah bisa)

```bash
cd poc/5gone
bash scripts/validate-repo.sh    # harus PASS
```

Copy folder `research/` ke Linux (git clone / USB drive).

## Di Linux kantor — urutan install

```bash
cd poc/5gone

# 1. Validasi environment
bash scripts/preflight.sh        # harus PASS

# 2. Install terurut (atau satu per satu)
bash scripts/install-all.sh

# ATAU manual step-by-step:
bash scripts/install-deps.sh
bash scripts/tune-usb-b210.sh     # B210 harus sudah colok
bash scripts/benchmark-b210.sh    # Num overruns = 0 WAJIB
bash scripts/install-open5gs.sh
bash scripts/install-srsran.sh
bash scripts/verify-lab.sh        # harus PASS
bash scripts/install-phase2.sh    # attacker binary
bash scripts/verify-phase1.sh     # sniffer
bash scripts/verify-phase2.sh     # attacker
```

## Start lab

```bash
bash scripts/patch-open5gs-plmn.sh   # PLMN 001/01 TAC 1
bash scripts/start-5g-native.sh core   # MongoDB + Open5GS dulu
bash scripts/add-test-subscriber.sh    # tambah SIM lab
bash scripts/start-5g-native.sh gnb    # srsRAN + B210
bash scripts/start-5g-native.sh status
```

## Stop

```bash
bash scripts/start-5g-native.sh down
```

## Pass criteria (wajib sebelum RF tent)

| Check | Command | Pass |
|-------|---------|------|
| Repo OK | `validate-repo.sh` | exit 0 |
| Preflight | `preflight.sh` | exit 0 |
| USB benchmark | `benchmark-b210.sh` | overruns = 0 |
| Verify | `verify-lab.sh` | exit 0 |
| gNB MAC log | `tail /tmp/gnb_5gone.log` | RAR lines when UE PRACH |
| gNB startup | `tail run/gnb.log` | no UHD error |
| AMF | `pgrep open5gs-amfd` | running |
| Verify sniffer | `verify-phase1.sh` | exit 0 |
| Verify attacker | `verify-phase2.sh` | exit 0 |
| Pipeline demo | `run-full-demo.sh` | 5 grants processed |

## Troubleshooting

Lihat **full risk matrix**: [README § Risks & troubleshooting](../README.md#risks--troubleshooting)

| Problem | Fix |
|---------|-----|
| UHD overruns | `tune-usb-b210.sh`, port USB3 lain, `RATE=15.36e6` |
| gNB can't connect AMF | cek PLMN/TAC match, `patch-open5gs-plmn.sh` |
| B210 not found | `lsusb`, udev rule, re-plug |
| srsRAN build fail | AVX2 CPU, `install-deps.sh` ulang |

## Config files (jangan ubah sembarangan)

| File | Purpose |
|------|---------|
| `config/srsran/gnb_20mhz.yml` | gNB — official B200 n78 20MHz template |
| `config/b210.uhd.args` | UHD device args — must match gnb srate 23.04 |
| `config/open5gs/amf.yaml.patch` | PLMN reference |

## Phase 2 — attack demo

```bash
bash scripts/install-phase2.sh
bash scripts/verify-phase2.sh
bash scripts/run-full-demo.sh            # end-to-end pipeline (no RF)
bash scripts/run-attack-rar-dos.sh demo  # offline sim

# Live lab (Linux + 2x B210):
bash scripts/start-attack-lab.sh up        # gNB + sniffer + attacker
bash scripts/start-attack-lab.sh down
```

See [phase2-attack.md](phase2-attack.md).

## Phase 0 (optional, bisa di Mac)

```bash
bash scripts/setup-lab.sh --phase 0
```
