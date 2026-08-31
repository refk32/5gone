# Phase 2 — RAR DoS Attacker

Uplink overshadow attack demo (paper Section 4.1) for the 5Gone PoC lab.

## Architecture

```
DL RX (UHD) → cell sync → RAR monitor (PDCCH/RA-RNTI)
                              ↓
                    extract UL grant (k, freq, TC-RNTI)
                              ↓
              empty MAC PDU → PUSCH encode → UL TX (overshadow)
```

## Modes

| Mode | Hardware | Purpose |
|------|----------|---------|
| `sim` | None | Replay 52 RAR attacks from 5gone dataset |
| `inject` | 2nd B210 + PA | TX with known grant (lab debug) |
| `live` | 2nd B210 + PA | Full DL monitor (PDCCH decode WIP) |

## Build (Linux office)

```bash
bash scripts/install-phase2.sh
bash scripts/verify-phase2.sh
```

## Run

```bash
# Offline demo (Mac or Linux)
bash scripts/run-attack-rar-dos.sh demo

# RF inject (shield tent!)
bash scripts/run-attack-rar-dos.sh inject phase2/config/sample_grant.json

# Live (after PDCCH decode validated)
export ATTACKER_DEVICE='serial=XXXXX'
bash scripts/run-attack-rar-dos.sh live
```

## Hardware note

**Two USRP B210 required for full RF attack:**
- B210 #1 → srsRAN gNB (`start-5g-native.sh gnb`)
- B210 #2 → attacker (`run-attack-rar-dos.sh inject|live`)

Single B210: use `sim` mode + dataset; or run gNB on ZMQ for dev.

## Files

- `config/rar_dos.yaml` — attacker config (aligned with gNB)
- `iq_templates/` — precomputed Msg3 IQ (generate via `tools/generate_msg3_template.py`)
- `python/grant_bus.py` — manual grant injection bus

See [docs/phase2-attack.md](../docs/phase2-attack.md) for full attack procedure.
