#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins
"""Bench bring-up correlation: cold-boot the board on the programmable supply and line up
the current draw with the serial boot log on one clock.

This is the integration layer over two *generic* tools that know nothing about each other:
  * serial_proxy.py --timestamp  -> owns the serial port, tees an epoch-stamped log
  * dps150.py log --cycle        -> owns the supply, power-cycles the rail and streams
                                    epoch-stamped current samples on stdout
Both emit wall-clock (epoch) timestamps, so this script just reads the two streams and
merges them; it contains no serial-port or instrument code itself. Swap in any other
timestamped serial monitor or any other bench-supply logger and this still works.

Prereqs:
  1. A serial monitor writing an epoch-stamped log, e.g. the serial-proxy skill:
        ~/.claude/skills/serial-proxy/serial_proxy.py monitor --port FTDI --timestamp &
  2. dps150.py (the PSU tool) reachable: --dps PATH, or $DPS150_BIN, or the default below.

Usage:
  tools/bringup.py [--volts 3.2] [--ilimit 1.5] [--baseline 1.5] [--duration 11]
                   [--log /tmp/serial_proxy.log] [--dps PATH]
"""
import argparse
import os
import re
import subprocess
import sys
import threading
import time

# Both building blocks are generic global skills; this repo just orchestrates them.
PROXY = os.environ.get("SERIAL_PROXY_BIN",
                       os.path.expanduser("~/.claude/skills/serial-proxy/serial_proxy.py"))
DEFAULT_DPS = os.environ.get("DPS150_BIN", os.path.expanduser("~/.claude/skills/dps150/dps150.py"))
DEFAULT_LOG = os.environ.get("SERIAL_PROXY_LOG", "/tmp/serial_proxy.log")
STAMP = re.compile(r"^\[(\d+(?:\.\d+)?)\]\s?(.*)$")


def tail_log(path, stop, out, lock):
    """Follow `path` from its current end, appending (epoch, 'S', text) to `out`."""
    try:
        f = open(path, "r", errors="replace")
    except OSError:
        sys.stderr.write("bringup: cannot open serial log %s\n" % path)
        return
    f.seek(0, os.SEEK_END)
    while not stop.is_set():
        line = f.readline()
        if not line:
            time.sleep(0.003)
            continue
        line = line.rstrip("\n")
        if not line:
            continue
        m = STAMP.match(line)
        at, text = (float(m.group(1)), m.group(2)) if m else (time.time(), line)
        with lock:
            out.append((at, "S", text))
    f.close()


def read_psu(proc, state, out, lock, on_t0=None):
    """Read `dps150 log --cycle` stdout: '# t0 <epoch>' sets state['t0']; TSV rows become
    (epoch, 'I', (v, i, p, mode)). on_t0() fires the instant power-on is reported."""
    for raw in proc.stdout:
        line = raw.rstrip("\n")
        if line.startswith("# t0 "):
            state["t0"] = float(line[5:])
            if on_t0:
                on_t0()
            with lock:
                out.append((state["t0"], "EVT", "*** OUTPUT ON ***"))
            continue
        if line.startswith("#") or not line:
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        try:
            at, i, v, p = (float(parts[0]), float(parts[1]), float(parts[2]), float(parts[3]))
        except ValueError:
            continue
        with lock:
            out.append((at, "I", (v, i, p, parts[4])))


def render(events, t0, baseline, volts):
    rows = sorted(events, key=lambda e: e[0])
    print("   t(s)   | event")
    print("-" * 74)
    last_t, last_i = -9, None
    for at, kind, payload in rows:
        rel = at - t0
        if rel < -baseline - 0.1:
            continue
        if kind == "S":
            print("%+8.3f | SERIAL  %s" % (rel, payload))
        elif kind == "EVT":
            print("%+8.3f | %s" % (rel, payload))
        elif kind == "I":
            v, i, p, mode = payload
            if last_i is None or abs(i - last_i) >= 0.008 or (rel - last_t) >= 0.30:
                print("%+8.3f | I=%.3f A  V=%.3f  P=%.2f W  %s" % (rel, i, v, p, mode))
                last_t, last_i = rel, i
    isamps = [(e[0] - t0, e[2][1]) for e in rows if e[1] == "I" and (e[0] - t0) >= 0]
    if isamps:
        pk = max(isamps, key=lambda x: x[1])
        print("-" * 74)
        print("peak current : %.3f A at t=%+.3f s   (%.2f W)" % (pk[1], pk[0], pk[1] * volts))
        print("settled draw : %.3f A (last sample)" % isamps[-1][1])
    nser = sum(1 for e in rows if e[1] == "S")
    if nser == 0:
        print("WARNING: no serial lines captured -- is the monitor running with --timestamp "
              "and writing %s ?" % LOG_HINT)


LOG_HINT = ""


def main():
    global LOG_HINT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--volts", type=float, default=3.2)
    ap.add_argument("--ilimit", type=float, default=1.5)
    ap.add_argument("--baseline", type=float, default=1.5, help="s of rail-off baseline before power-on")
    ap.add_argument("--duration", type=float, default=11.0, help="s to capture after power-on")
    ap.add_argument("--log", default=DEFAULT_LOG, help="epoch-stamped serial log to read")
    ap.add_argument("--dps", default=DEFAULT_DPS, help="path to the dps150.py PSU tool")
    ap.add_argument("--hz", type=float, default=20.0)
    ap.add_argument("--kill-phantom", action="store_true",
                    help="release the serial monitor's port during the rail-off window so its "
                         "signal lines can't phantom-power the board, giving a true cold reset "
                         "(needs a running serial_proxy; resumes it at power-on)")
    a = ap.parse_args()
    LOG_HINT = a.log

    if not os.path.exists(a.dps):
        sys.exit("bringup: PSU tool not found at %s (set --dps or $DPS150_BIN)" % a.dps)

    on_t0 = None
    if a.kill_phantom:
        # Close the monitor's port NOW so the adapter stops phantom-powering the board; it
        # discharges during dps150's --settle window. Resume the monitor the instant the rail
        # comes back (t0) to catch the boot log.
        if subprocess.call([sys.executable, PROXY, "release"]) != 0:
            sys.exit("bringup: --kill-phantom needs a running serial monitor (serial_proxy)")
        on_t0 = lambda: subprocess.call([sys.executable, PROXY, "resume"])

    events, lock, state = [], threading.Lock(), {"t0": None}
    stop = threading.Event()

    tL = threading.Thread(target=tail_log, args=(a.log, stop, events, lock), daemon=True)
    tL.start()

    # Kick off the generic PSU logger: it power-cycles the rail and streams current + '# t0'.
    proc = subprocess.Popen(
        [sys.executable, a.dps, "log", "--cycle",
         "--volts", str(a.volts), "--ilimit", str(a.ilimit),
         "--settle", str(a.baseline), "--duration", str(a.duration), "--hz", str(a.hz)],
        stdout=subprocess.PIPE, text=True)
    read_psu(proc, state, events, lock, on_t0=on_t0)   # returns when dps150 exits
    proc.wait()
    if a.kill_phantom:
        subprocess.call([sys.executable, PROXY, "resume"])   # ensure monitor is back
    time.sleep(0.3)                       # let the last serial lines land in the log
    stop.set()
    tL.join(timeout=1)

    if state["t0"] is None:
        sys.exit("bringup: PSU logger never reported power-on (# t0). Check the supply.")
    render(events, state["t0"], a.baseline, a.volts)


if __name__ == "__main__":
    main()
