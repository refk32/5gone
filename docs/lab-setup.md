# 5Gone Lab Setup — Step by Step

Hardware target: **USRP B210** + **20 MHz** srsRAN cell + **shielded box**.

---

## Overview

```
┌─────────────────────────────────────────────────────────┐
│  RF Shield Box / Tent                                    │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐          │
│  │ srsRAN   │    │ Test     │    │ B210     │          │
│  │ gNB SDR  │    │ Phone UE │    │ Attacker │          │
│  │ (or SDR) │    │          │    │ + PA     │          │
│  └────┬─────┘    └────┬─────┘    └────┬─────┘          │
│       └───────────────┴─────────────────┘               │
│              Attenuators + cables                        │
└─────────────────────────────────────────────────────────┘
         │
    Linux Host (Ubuntu 22.04)
    ├── Open5GS (5G core)
    ├── srsRAN gNB (20 MHz)
    └── 5Gone PoC software (Phase 1+)
```

---

## Step 1 — Host OS

**Recommended:** Ubuntu 22.04 LTS bare metal (Intel/AMD, 8+ cores, 16+ GB RAM).

Mac (darwin): Phase 0 OK. Phase 1+ srsRAN — gunakan **Ubuntu VM** (UTM/Parallels) dengan USB passthrough untuk B210.

```bash
# Verify after Ubuntu install
uname -m   # x86_64
lsusb      # B210 should appear as Ettus
```

---

## Step 2 — Install software stack (native, no Docker)

```bash
cd poc/5gone
bash scripts/install-deps.sh
bash scripts/install-open5gs.sh   # Linux
bash scripts/install-srsran.sh    # Linux — builds gNB binary
bash scripts/tune-usb-b210.sh     # USB stability — run when B210 connected
```

**No Docker** — B210 needs direct USB to host; container USB passthrough is unstable.

Verify B210:

```bash
bash scripts/benchmark-b210.sh
# Must show: zero overruns at 23.04 MSPS
```

---

## Step 3 — Phase 0 (no hardware RF)

```bash
bash scripts/setup-lab.sh --phase 0
python3 phase0/analyze_traces.py --dataset data/5gone-dataset
```

---

## Step 4 — RF bench wiring (Phase 1)

**Before powering PA — passive RX only first.**

```
gNB antenna ──[30dB att]── splitter ── B210 RX
                              └── (optional) spectrum monitor

Phone UE ── inside shield box, minimal signal via attenuated path
B210 RX ── same box, antenna 10–20 cm from gNB antenna
```

1. Connect B210 #1, run `uhd_find_devices`
2. Start 5G stack: `bash scripts/start-5g-native.sh up`
3. Place phone in tent, enable flight mode → disable → watch attach
4. Start sniffer: `bash scripts/start-sniffer.sh tail` (parses `/tmp/gnb_5gone.log` → grant bus)

> **Log paths:** srsRAN MAC/RAR → `/tmp/gnb_5gone.log` (sniffer). Startup/UHD errors → `run/gnb.log`.

---

## Step 5 — 5G stack (Open5GS + srsRAN native)

```bash
bash scripts/start-5g-native.sh up    # MongoDB + Open5GS + gNB
bash scripts/start-5g-native.sh logs
bash scripts/start-5g-native.sh down
```

Config: `config/srsran/gnb_20mhz.yml` — B210 via UHD direct on host.

**First boot checklist:**
- [ ] Open5GS AMF running (port 38412)
- [ ] srsRAN gNB connected to AMF
- [ ] Phone shows 5G icon in tent (weak signal via attenuator)
- [ ] SIM provisioned in Open5GS subscriber DB

Add test subscriber:

```bash
bash scripts/add-test-subscriber.sh --imsi 001010000000001 --k 465B5CE8B199B49FAA5F0A2EE238A6BC --opc E8ED289DEBA952E4283B54E88E6183CA
```

---

## Step 6 — Phase 2 attack (RAR DoS)

**Requires 2nd USRP B210 for RF TX** (gNB uses B210 #1).

```bash
bash scripts/install-phase2.sh
bash scripts/verify-phase2.sh
bash scripts/run-attack-rar-dos.sh demo     # offline first

# Live: sniffer + bus attacker (or one command)
bash scripts/start-sniffer.sh tail &
bash scripts/run-attack-rar-dos.sh bus
# OR: bash scripts/start-attack-lab.sh up
```

**Only inside shield box with PA:**

```
B210 #2 TX ── BPF ── PA ── [variable att] ── antenna in tent
```

See [phase2-attack.md](phase2-attack.md) for full procedure.

---

## Step 7 — RF prep (overshadow power)

Power budget:
- Phone UL typical: 0–23 dBm
- B210 alone: ~10 dBm — **insufficient**
- Need PA: +20–30 dB → target 20–30 dBm at antenna (after att)

Calibrate with maximum attenuation first, reduce until overshadow observed.

---

## Step 8 — Directory layout

```
poc/5gone/
├── config/          # UHD args, gNB YAML, Open5GS notes
├── data/            # 5gone dataset (gitignored)
├── docs/            # Equipment + lab setup
├── phase0/          # Trace analysis
├── phase1/sniffer/  # RAR log parser → grant bus
├── phase2/          # RAR DoS attacker (sim/inject/live/bus)
├── run/             # PID files, runtime logs (gitignored)
├── build/           # srsRAN + Open5GS build output (gitignored)
├── scripts/         # Native install — no Docker
└── logs/            # Analysis reports (gitignored)
```

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| B210 not found | USB3 passthrough (VM), `sudo uhd_images_downloader` |
| UHD overruns | Lower rate, `num_recv_frames=512`, dedicated USB port |
| Phone won't attach | Check IMSI in Open5GS, PLMN match, band n78 on phone |
| srsRAN won't start | Check `$RUN/gnb.log`, CPU AVX2, B210 USB (run tune-usb-b210.sh) |
| No RAR decoded | Enable gNB MAC log; run `start-sniffer.sh tail`; check `run/gnb.log` |

---

## Implementation status

1. `phase0/analyze_traces.py` — dataset parser ✓
2. `phase1/sniffer/` — RAR log parser + grant bus ✓
3. `phase2/5gone-rar-dos` — RAR DoS attacker (sim/inject/bus) ✓
4. Live PDCCH decode (RF, no gNB log) — next
5. NAS attacks (Registration Reject, SUCI) — Phase 3
