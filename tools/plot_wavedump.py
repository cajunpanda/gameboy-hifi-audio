#!/usr/bin/env python3
"""Analyze a WAVEDUMP block captured from the GameBoy HiFi firmware.

Capture with the `wavedump` console command (pair with `radio off` for a clean
GBA-only noise floor), then run this against the serial log:

    tools/plot_wavedump.py                     # reads /tmp/gba_serial.log
    tools/plot_wavedump.py somelog.txt
    tail -f /tmp/gba_serial.log | tools/plot_wavedump.py -
    tools/plot_wavedump.py --plot noise.png    # also save waveform+spectrum (needs matplotlib)

Prints RMS/peak level, the spectral noise floor, and any dominant tones, so you
can decide between a notch (tonal hum), a high-pass (low-frequency hum), or a
gate/expander (broadband hiss). Samples are the raw 24-in-32 ADC values; they are
normalized so full scale = 0 dBFS.
"""
import sys
import argparse
import math


def parse_blocks(lines):
    """Return (fs, [(L,R), ...]) for the LAST complete WAVEDUMP block, or (None, None)."""
    blocks = []
    cur = None
    fs = 44100
    for ln in lines:
        s = ln.strip()
        if "WAVEDUMP_BEGIN" in s:  # may be prefixed by the "hifi> " console prompt
            cur = []
            for tok in s.split():
                if tok.startswith("fs="):
                    try:
                        fs = int(tok[3:])
                    except ValueError:
                        pass
        elif "WAVEDUMP_END" in s:
            if cur is not None:
                blocks.append((fs, cur))
                cur = None
        elif cur is not None:
            parts = s.split(",")
            if len(parts) == 2:
                try:
                    cur.append((int(parts[0]), int(parts[1])))
                except ValueError:
                    pass
    if not blocks:
        return None, None
    return blocks[-1]


def db(v):
    return 20 * math.log10(v) if v > 1e-12 else -999.0


def analyze(fs, samples):
    import numpy as np

    a = np.array(samples, dtype=np.float64) / 2147483648.0  # 24-in-32 -> full scale
    n = len(a)
    print(f"# wavedump: N={n} samples, fs={fs} Hz, {n / fs * 1000:.1f} ms window")
    for ch, name in ((0, "L"), (1, "R")):
        x = a[:, ch] - np.mean(a[:, ch])  # strip DC offset
        rms = float(np.sqrt(np.mean(x ** 2)))
        peak = float(np.max(np.abs(x))) if n else 0.0
        print(f"\n== {name} channel ==")
        print(f"  RMS {db(rms):7.1f} dBFS     peak {db(peak):7.1f} dBFS")

        w = np.hanning(n)
        mag = np.abs(np.fft.rfft(x * w)) / (np.sum(w) / 2)
        freqs = np.fft.rfftfreq(n, 1.0 / fs)
        magdb = 20 * np.log10(np.maximum(mag, 1e-12))
        floor = float(np.median(magdb[1:]))
        print(f"  spectral noise floor (median) {floor:6.1f} dBFS")

        peaks = []
        for i in range(2, len(magdb) - 2):
            if magdb[i] > floor + 12 and magdb[i] >= magdb[i - 1] and magdb[i] > magdb[i + 1]:
                peaks.append((magdb[i], float(freqs[i])))
        peaks.sort(reverse=True)
        if peaks:
            print("  dominant tones (>12 dB over floor):")
            for m, f in peaks[:8]:
                print(f"     {f:8.1f} Hz   {m:6.1f} dBFS   (+{m - floor:.0f} dB)")
            print("  -> tonal: consider a notch (per tone) or high-pass if it is low-frequency")
        else:
            print("  no dominant tones -> broadband: a gate/downward expander is the tool")
    return a


def make_plot(a, fs, path):
    try:
        import numpy as np
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:  # noqa: BLE001
        print(f"# plot skipped (matplotlib unavailable: {e})")
        return
    n = len(a)
    t = np.arange(n) / fs * 1000.0
    fig, ax = plt.subplots(2, 1, figsize=(10, 7))
    for ch, nm in ((0, "L"), (1, "R")):
        x = a[:, ch] - np.mean(a[:, ch])
        ax[0].plot(t, x, label=nm, lw=0.6)
        w = np.hanning(n)
        mag = np.abs(np.fft.rfft(x * w)) / (np.sum(w) / 2)
        f = np.fft.rfftfreq(n, 1.0 / fs)
        ax[1].semilogx(f[1:], 20 * np.log10(np.maximum(mag[1:], 1e-12)), label=nm, lw=0.7)
    ax[0].set(title="waveform", xlabel="ms", ylabel="amplitude")
    ax[0].legend(); ax[0].grid(alpha=0.3)
    ax[1].set(title="spectrum", xlabel="Hz", ylabel="dBFS", xlim=(10, fs / 2))
    ax[1].legend(); ax[1].grid(alpha=0.3, which="both")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    print(f"# plot saved: {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile", nargs="?", default="/tmp/gba_serial.log",
                    help="serial log to scan (default /tmp/gba_serial.log; '-' = stdin)")
    ap.add_argument("--plot", metavar="PNG", help="save a waveform+spectrum plot")
    args = ap.parse_args()

    if args.logfile == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.logfile) as f:
            lines = f.readlines()

    fs, samples = parse_blocks(lines)
    if not samples:
        sys.exit("no WAVEDUMP block found (run `wavedump` on the device first)")
    try:
        import numpy  # noqa: F401
    except ImportError:
        sys.exit("numpy required (pip install numpy)")

    a = analyze(fs, samples)
    if args.plot:
        make_plot(a, fs, args.plot)


if __name__ == "__main__":
    main()
