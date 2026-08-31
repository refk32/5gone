#!/usr/bin/env bash
# tune-usb-b210.sh — Reduce USB instability for USRP B210 (native host only)
set -euo pipefail

echo "==> B210 USB stability tuning (requires sudo)"

if [ "$(uname -s)" != "Linux" ]; then
  echo "Run on Linux host that owns the B210 USB port."
  exit 0
fi

# Disable USB autosuspend for Ettus devices
RULE=/etc/udev/rules.d/99-usrp-b210.rules
sudo tee "$RULE" >/dev/null <<'EOF'
# USRP B210 — disable autosuspend, reduce latency
SUBSYSTEM=="usb", ATTR{idVendor}=="2500", ATTR{idProduct}=="0020", ATTR{power/control}="on", ATTR{power/autosuspend}="-1"
SUBSYSTEM=="usb", ATTR{idVendor}=="2500", ATTR{idProduct}=="0021", ATTR{power/control}="on", ATTR{power/autosuspend}="-1"
SUBSYSTEM=="usb", ATTR{idVendor}=="2500", ATTR{idProduct}=="0022", ATTR{power/control}="on", ATTR{power/autosuspend}="-1"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "==> udev rule: $RULE"

# USB memory for streaming
SYS_USB=/sys/module/usbcore/parameters/usbfs_memory_mb
if [ -w "$SYS_USB" ] 2>/dev/null || [ -f "$SYS_USB" ]; then
  echo 1000 | sudo tee "$SYS_USB" >/dev/null 2>&1 || true
  echo "    usbfs_memory_mb=1000"
fi

# CPU governor performance (optional)
if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
  for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance | sudo tee "$gov" >/dev/null 2>&1 || true
  done
  echo "    CPU governor: performance"
fi

cat <<'TIPS'

==> Manual tips (no Docker):
  1. B210 langsung ke port USB3 motherboard (bukan hub)
  2. Cable USB3 pendek (<1m), kualitas baik
  3. Satu proses UHD saja — jangan srsRAN + GNU Radio bersamaan
  4. VM: pakai USB passthrough whole controller, bukan single device filter
  5. Benchmark: bash scripts/benchmark-b210.sh (zero overruns wajib)
  6. Turunkan rate jika perlu: RATE=15.36e6 bash scripts/benchmark-b210.sh

TIPS

if command -v uhd_find_devices >/dev/null 2>&1; then
  echo "==> Detected devices:"
  uhd_find_devices || true
fi
