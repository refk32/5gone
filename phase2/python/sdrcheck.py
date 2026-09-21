#!/usr/bin/env python3
import sys, os, math, subprocess, time, tempfile, shutil, struct

RATE  = 23.04e6
FREQ  = 3489420000.0
TONE  = 1.0e6
RX_SEC = 1.0
XLDIRS = ["/usr/local/lib/uhd/examples", "/usr/local/bin",
          "/usr/lib/uhd/examples", "/usr/share/uhd/examples"]

def find_tool(cands):
    for c in cands:
        for d in XLDIRS:
            p = os.path.join(d, c)
            if os.path.isfile(p): return p
        w = shutil.which(c)
        if w: return w
    return None

def help_text(tool):
    r = subprocess.run([tool, "--help"], capture_output=True, text=True)
    return (r.stdout or "") + (r.stderr or "")

def opts(h, names):
    return {n: ("--" + n) in h for n in names}

def tx_cmd(tool, o, serial):
    cmd = [tool, "--args", f"type=b200,serial={serial},master_clock_rate={int(RATE)}",
           "--freq", str(int(FREQ)), "--rate", str(int(RATE)), "--gain", "80"]
    if o["antenna"]:  cmd += ["--antenna", "TX/RX"]
    if o["subdev"]:   cmd += ["--subdev", "A:A"]
    if o["amp"]:      cmd += ["--amp", "0.5"]
    if o["duration"]: cmd += ["--duration", "2.5"]
    if o["wave-type"]:   w = ["--wave-type", "SINE"]
    elif o["type"]:      w = ["--type", "SINE"]
    else:                return None, "[tx] no waveform flag (--wave-type/--type)"
    cmd += w
    if o["wave-freq"]: cmd += ["--wave-freq", str(int(TONE))]
    return cmd, None

def rx_cmd(tool, o, serial, ns, path):
    cmd = [tool, "--args", f"type=b200,serial={serial},master_clock_rate={int(RATE)}",
           "--freq", str(int(FREQ)), "--rate", str(int(RATE)), "--gain", "40",
           "--nsamps", str(ns)]
    if o["file"]: cmd += ["--file", path]
    elif o["output"]: cmd += ["--output", path]
    else: return None, "[rx] no output flag (--file/--output)"
    if o["subdev"]:   cmd += ["--subdev", "A:B"]
    if o["antenna"]:  cmd += ["--antenna", "TX/RX"]
    return cmd, None

def analyze(path, ns):
    with open(path, "rb") as f:
        data = f.read()
    nbytes = len(data)
    if nbytes == ns * 8:
        iq = struct.iter_unpack("<ff", data); scale = 1.0
    elif nbytes == ns * 4:
        iq = struct.iter_unpack("<hh", data); scale = 1.0 / 32768.0
    else:
        print(f"[rx] unexpected file size {nbytes} bytes for {ns} samples")
        sys.exit(4)
    tot = 0.0; sr = 0j; prev = 0j; first = True
    for re, im in iq:
        x = complex(re * scale, im * scale)
        if not first:
            sr += x * prev.conjugate()      # line detector: locks any offset
        prev = x; first = False
        tot += x.real * x.real + x.imag * x.imag
    tot_pow = tot / ns
    line_pow = abs(sr) / (ns - 1)
    noise = tot_pow - line_pow
    snr = 10 * math.log10(line_pow / (noise if noise > 1e-15 else 1e-15))
    f = math.atan2(sr.imag, sr.real) * RATE / (2 * math.pi)
    return tot_pow, snr, f

def main():
    if len(sys.argv) != 3:
        print("usage: python3 /tmp/sdrcheck.py <TX_SERIAL> <RX_SERIAL>")
        sys.exit(2)
    tx_serial, rx_serial = sys.argv[1], sys.argv[2]
    tx_tool = find_tool(["uhd_tx_waveforms", "tx_waveforms"])
    rx_tool = find_tool(["uhd_rx_samples_to_file", "rx_samples_to_file"])
    if not tx_tool or not rx_tool:
        print("[find] TX:", tx_tool, " RX:", rx_tool)
        print("[find] not found — set env TX_TOOL / RX_TOOL to full path.")
        sys.exit(2)

    o_tx = opts(help_text(tx_tool), ["antenna", "subdev", "amp", "duration",
                                     "wave-type", "type", "wave-freq"])
    o_rx = opts(help_text(rx_tool), ["file", "output", "subdev", "antenna", "type"])
    ns = int(RATE * RX_SEC)
    path = tempfile.mktemp(suffix=".cf32")
    txp = None
    try:
        cmd, err = tx_cmd(tx_tool, o_tx, tx_serial)
        if err: print(err); sys.exit(2)
        txp = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        time.sleep(1.0)
        cmd, err = rx_cmd(rx_tool, o_rx, rx_serial, ns, path)
        if err: print(err); sys.exit(2)
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            print("[rx] tool failed:\n", r.stderr)
            if txp.poll() is not None:
                print("[tx] stderr:\n", txp.stderr.read())
            sys.exit(3)
        mean, snr, f = analyze(path, ns)
        level_db = 10 * math.log10(mean)
        heard = snr > 6.0 and abs(abs(f) - TONE) < 40e3 and abs(f) > 20e3
        verdict = "RADIO OK: tone heard" if heard else "FLAT: no tone heard"
        print(f"[rx] {rx_serial}: line at {f:+.1f} kHz, +{snr:.1f} dB over noise "
              f"| level {level_db:.1f} dBFS")
        print(f"[verdict] {tx_serial} -> {rx_serial} : {verdict}")
    finally:
        if txp is not None and txp.poll() is None:
            txp.terminate()
            try: txp.wait(timeout=5)
            except subprocess.TimeoutExpired: txp.kill()
        if os.path.exists(path): os.unlink(path)

if __name__ == "__main__":
    main()