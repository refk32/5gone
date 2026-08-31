# Phase 1 — Passive RAR sniffer

Monitors gNB logs for RAR + UL grant, publishes to grant bus for Phase 2 attacker.

## Modes

| Command | Use |
|---------|-----|
| `tail` | Live — follows `/tmp/gnb_5gone.log` (srsRAN MAC log) |
| `replay` | Demo — replays dataset log into bus |
| `once` | Parse entire log file |

## Quick start

```bash
# Demo pipeline (no RF)
bash scripts/run-full-demo.sh

# Live lab
bash scripts/start-5g-native.sh gnb
bash scripts/start-sniffer.sh tail
bash scripts/run-attack-rar-dos.sh bus
```

Grant bus: `/tmp/5gone_grants.jsonl`
