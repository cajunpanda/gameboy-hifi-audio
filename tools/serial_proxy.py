#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins
"""Serial proxy and flasher for the GameBoy HiFi bench rig: one port owner, many readers, no manual juggling.

Only one process can open /dev/ttyUSB0, so a human and an automated tool (IDE
task, CI step, AI agent) can't both watch it, and you normally have to stop the
monitor before flashing. This proxy fixes both:

  * It owns the port and tees everything to a shared logfile, so any number of
    readers follow it live (`tail -f /tmp/gba_serial.log`, file reads, etc.).
  * `flash` is a first-class subcommand: it tells the running proxy to briefly
    release the port, runs `pio ... upload`, and the proxy auto-reattaches and
    resumes logging. You never kill/restart anything by hand; `tail -f` survives it
    (it just pauses during the flash, then the fresh boot log streams in).
  * It survives power-cycles and replug: `poll()` detects the USB-serial chip
    re-enumerating and re-execs a clean process to reopen the port (flushing the
    power-on glitch), so a replug no longer leaves the line stuck spewing garbage.

Usage:
  tools/serial_proxy.py monitor [--port P] [--baud B] [--log PATH] [--no-reset]
        Own the port and tee to the log (run in a terminal or as a background
        process). Then, anywhere:
            tail -f /tmp/gba_serial.log
  tools/serial_proxy.py flash [--env prod] [--port P] [--manual] [--no-truncate] [-- EXTRA pio args...]
        Pause the proxy, build+upload (to the proxy's port, or --port), resume + reset. On
        success the log is truncated for a fresh start (keeps context small; --no-truncate
        keeps it). Direct flash if no proxy.
        `--manual` is for a no-auto-reset adapter (3-wire): it prompts you to enter download
        mode (hold BOOT, tap EN, release BOOT) and waits, then reminds you to tap EN to boot.
        Must be run from a terminal (it needs a TTY for the prompt), e.g. with the `!` prefix.

External USB-UART adapter (e.g. to the board's UART pins for first flash or current measurement):
  `--port` takes a device path OR a /dev/serial/by-id substring, so you can pin an
  outboard FTDI/CP2102 by name even when several adapters are present:
      tools/serial_proxy.py monitor --port FTDI      # or CP2102, or /dev/ttyUSB1
  ($SERIAL_PROXY_PORT still works as an exact-path override.) flash/send/reset then
  inherit that port from the running proxy automatically.
  NOTE: a bare 3-wire (TX/RX/GND) adapter has no DTR/RTS auto-reset, so esptool can't
  enter the bootloader on its own. When `flash` reaches "Connecting....", hold BOOT(IO0)
  and tap EN/RESET on the board. And only connect the adapter's TX/RX/GND, NOT its
  VCC/5V pin (the bench supply is the only power source for a current measurement).
  tools/serial_proxy.py send sleep # inject a console line to the device (proxy keeps the port)
  tools/serial_proxy.py reset      # pulse DTR/RTS to reboot the chip
  tools/serial_proxy.py status     # show proxy pid / port / log
  tools/serial_proxy.py stop       # stop the proxy
  tools/serial_proxy.py tail       # follow the log in this terminal
"""
import argparse
import json
import os
import select
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW_DIR = os.path.join(os.path.dirname(HERE), "firmware")
STATE = os.environ.get("SERIAL_PROXY_STATE", "/tmp/btgba_serial_proxy.json")
DEFAULT_LOG = os.environ.get("SERIAL_PROXY_LOG", "/tmp/gba_serial.log")
DEFAULT_BAUD = int(os.environ.get("SERIAL_PROXY_BAUD", "115200"))
# Control-file queue for the `send` subcommand: the CLI appends lines here, the
# running proxy drains+injects them to the device (it owns the port, so a reader
# can't write directly). Signals can't carry the payload, so a file is used.
SEND = os.environ.get("SERIAL_PROXY_SEND", "/tmp/btgba_serial_proxy.send")


def _byid_links():
    byid = "/dev/serial/by-id"
    try:
        return sorted(os.path.join(byid, e) for e in os.listdir(byid))
    except OSError:
        return []


def discover_port(match=None, warn=False):
    """Resolve the serial port. Prefer a stable /dev/serial/by-id/* symlink: it follows the
    physical adapter across ttyUSBn renumbering on replug, unlike a bare /dev/ttyUSB0.

    `match` (from --port) pins a specific adapter: a full device path is used as-is, otherwise
    it's a case-insensitive substring matched against the by-id link names (e.g. 'FTDI',
    'CP2102', 'ttyUSB1'). This is how you select a specific adapter when more than one USB-UART
    bridge is enumerated. Without `match`: $SERIAL_PROXY_PORT (exact) wins, else the
    sole by-id link, else /dev/ttyUSB0. With two+ adapters and no match, the first is used and
    (if warn) a hint to disambiguate is printed."""
    if match:
        if os.path.exists(match) or match.startswith("/dev/"):
            return match                               # explicit device path
        for link in _byid_links():
            if match.lower() in os.path.basename(link).lower():
                return link
        sys.stderr.write("[serial_proxy] WARNING: no by-id adapter matches %r; using it as a "
                         "literal path\n" % match)
        return match
    env = os.environ.get("SERIAL_PROXY_PORT")
    if env:
        return env
    links = _byid_links()
    if links:
        if len(links) > 1 and warn:
            sys.stderr.write(
                "[serial_proxy] WARNING: %d serial adapters present; using %s.\n"
                "  Pass --port <substring> (e.g. FTDI / CP2102) to pick another:\n%s\n"
                % (len(links), links[0], "\n".join("    " + l for l in links)))
        return links[0]
    for cand in ("/dev/ttyUSB0", "/dev/ttyACM0"):
        if os.path.exists(cand):
            return cand
    return "/dev/ttyUSB0"


DEFAULT_PORT = discover_port()

try:
    import serial  # pyserial
except ImportError:
    serial = None


# ---------- shared state ----------
def load_state():
    try:
        s = json.load(open(STATE))
        return s if _alive(s["pid"]) else None
    except Exception:
        return None


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# ---------- the proxy daemon ----------
class Proxy:
    def __init__(self, port, baud, log, reset):
        self.port, self.baud, self.log, self.reset_on_attach = port, baud, log, reset
        self.ser = None
        self.pause = False
        self.pause_since = None
        self.reset_req = False
        self.stop = False
        self.poller = select.poll()
        self.reg_fd = None
        self.empty = 0

    def _open(self, do_reset):
        # Construct the port unopened so DTR/RTS can be forced low BEFORE open().
        # Opening a Serial() that already has a port asserts DTR, which the board's
        # DTR/RTS -> EN/IO0 auto-reset circuit turns into an EN pulse (a spurious
        # chip reset) on every (re)attach. Setting dtr/rts on the closed port makes
        # open() apply them, avoiding that glitch -- important when a flaky cable
        # re-enumerates repeatedly and we must not reboot the chip each time.
        self.ser = serial.Serial(timeout=0.2)
        self.ser.port = self.port
        self.ser.baudrate = self.baud
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass
        self.ser.open()
        if do_reset:
            self._reset_pulse()
        # Idle DTR and RTS to the same level so an external two-transistor auto-reset circuit
        # (DTR/RTS -> EN/IO0) leaves the chip running during monitoring instead of holding it
        # in reset/bootloader; opening a port otherwise asserts these lines. The
        # _reset_pulse() above already ends both low; this covers the no-reset attach path.
        # Harmless when DTR/RTS aren't wired (onboard USB or a plain 3-wire adapter).
        try:
            self.ser.setDTR(False)
            self.ser.setRTS(False)
        except Exception:
            pass
        try:
            self.ser.reset_input_buffer()   # drop power-on glitch bytes on (re)attach
        except Exception:
            pass
        self.poller.register(self.ser.fileno(),
                             select.POLLIN | select.POLLERR | select.POLLHUP | select.POLLNVAL)
        self.reg_fd = self.ser.fileno()
        self.empty = 0

    def _close(self):
        if self.reg_fd is not None:
            try:
                self.poller.unregister(self.reg_fd)
            except (KeyError, OSError):
                pass
            self.reg_fd = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def _drain_send(self):
        # Inject queued console input from the `send` subcommand. The CLI appends
        # lines to SEND; we atomically grab the file (rename, so a concurrent
        # append isn't lost; it lands in a fresh SEND for next pass) and write
        # each line to the device. One '\r' terminator only: a trailing '\n' would
        # look like a second (empty) Enter and exit the firmware's REPL
        # (linenoiseAllowEmpty(false)). Best-effort.
        if not self.ser:
            return
        tmp = SEND + ".inflight"
        try:
            os.rename(SEND, tmp)          # atomic; raises if SEND absent
        except OSError:
            return
        try:
            with open(tmp) as f:
                data = f.read()
            os.remove(tmp)
        except OSError:
            data = ""
        for line in data.splitlines():
            try:
                self.ser.write((line + "\r").encode())
                self.ser.flush()
                self._w("\n----- proxy: sent %r -----\n" % line)
            except Exception:
                pass

    def _reset_pulse(self):
        try:
            self.ser.setDTR(False)
            self.ser.setRTS(True)
            time.sleep(0.1)
            self.ser.reset_input_buffer()
            self.ser.setRTS(False)
            self._w("\n----- proxy: reset pulse %s -----\n" % time.strftime("%T"))
        except Exception:
            pass

    def _w(self, txt):
        with open(self.log, "a", buffering=1) as f:
            f.write(txt)
        sys.stdout.write(txt)
        sys.stdout.flush()

    def run(self):
        signal.signal(signal.SIGUSR1, lambda *_: self._set_pause(True))   # flash: release
        signal.signal(signal.SIGUSR2, lambda *_: self._set_pause(False))  # flash: resume
        signal.signal(signal.SIGHUP, lambda *_: setattr(self, "reset_req", True))
        signal.signal(signal.SIGTERM, lambda *_: setattr(self, "stop", True))
        signal.signal(signal.SIGINT, lambda *_: setattr(self, "stop", True))
        json.dump({"pid": os.getpid(), "port": self.port, "baud": self.baud, "log": self.log},
                  open(STATE, "w"))
        for stale in (SEND, SEND + ".inflight"):   # drop any leftover send queue
            try:
                os.remove(stale)
            except OSError:
                pass
        self._open(self.reset_on_attach)
        self._w("\n===== proxy attached%s %s pid=%d  port=%s =====\n" % (
            " + reset" if self.reset_on_attach else "", time.strftime("%F %T"),
            os.getpid(), self.port))
        missing = False
        try:
            while not self.stop:
                if self.pause:
                    if self.ser:
                        self._w("\n----- proxy: released port for flash %s -----\n" % time.strftime("%T"))
                        self._close()
                    if self.pause_since and time.time() - self.pause_since > 180:
                        self._w("\n----- proxy: auto-resume (pause timeout) -----\n")
                        self._set_pause(False)
                    else:
                        time.sleep(0.05)
                        continue
                if not self.ser:
                    # The device went away (power-cycle / replug). Reopening in-process is not
                    # reliable: on re-enumeration the USB-UART bridge comes back at a default
                    # baud and an in-process reopen doesn't always re-apply 115200, giving
                    # garbage. The one reliable recovery is a fresh process, so once the
                    # device reappears we re-exec ourselves.
                    if os.path.exists(self.port):
                        self._w("\n----- proxy: device back; restarting clean (re-exec) %s -----\n"
                                % time.strftime("%T"))
                        self._close()
                        try:
                            os.remove(STATE)        # the fresh process rewrites it (same pid)
                        except OSError:
                            pass
                        sys.stdout.flush()
                        time.sleep(0.4)             # let udev settle the new device node
                        os.execv(sys.executable, [sys.executable] + sys.argv)
                    if not missing:
                        self._w("\n----- proxy: %s gone, waiting for it to reappear... -----\n" % self.port)
                        missing = True
                    time.sleep(0.3)
                    continue
                if self.reset_req:
                    self.reset_req = False
                    self._reset_pulse()
                self._drain_send()
                try:
                    events = self.poller.poll(200)
                except Exception:
                    self._close()
                    continue
                if not events:
                    continue
                ev = events[0][1]
                if ev & (select.POLLHUP | select.POLLERR | select.POLLNVAL):
                    self._w("\n----- proxy: device disconnected; reopening %s -----\n" % time.strftime("%T"))
                    self._close()
                    continue
                if ev & select.POLLIN:
                    try:
                        data = self.ser.read(4096)
                    except (OSError, serial.SerialException):
                        self._w("\n----- proxy: read error; reopening %s -----\n" % time.strftime("%T"))
                        self._close()
                        continue
                    if data:
                        self.empty = 0
                        self._w(data.decode("utf-8", "replace"))
                    else:
                        # POLLIN with no bytes == EOF: the device went away
                        self.empty += 1
                        if self.empty > 3:
                            self._w("\n----- proxy: device EOF; reopening %s -----\n" % time.strftime("%T"))
                            self._close()
        finally:
            self._close()
            try:
                os.remove(STATE)
            except OSError:
                pass

    def _set_pause(self, v):
        self.pause = v
        self.pause_since = time.time() if v else None


# ---------- helpers ----------
def _wait_port_free(port, timeout):
    """Poll until the port can be opened (the proxy has released it)."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
            os.close(fd)
            return True
        except OSError:
            time.sleep(0.1)
    return False


# ---------- subcommands ----------
def cmd_monitor(a):
    if serial is None:
        sys.exit("pyserial missing: pip install --user pyserial")
    if load_state():
        sys.exit("proxy already running; `serial_proxy.py status`")
    port = discover_port(a.port, warn=True)
    print("[serial_proxy] owning %s -> %s   (tail -f %s)" % (port, a.log, a.log), flush=True)
    Proxy(port, a.baud, a.log, not a.no_reset).run()


def cmd_flash(a):
    extra = a.extra[1:] if a.extra and a.extra[0] == "--" else a.extra
    s = load_state()
    # Target the same port the proxy owns (so an external adapter selected at `monitor`
    # time is honored), else resolve --port / autodetect for a direct no-proxy flash.
    port = s["port"] if s else discover_port(a.port)
    cmd = ["pio", "run", "-d", FW_DIR, "-e", a.env, "-t", "upload",
           "--upload-port", port] + extra
    if s:
        os.kill(s["pid"], signal.SIGUSR1)               # ask proxy to release the port
        _wait_port_free(s["port"], 6)
        print("[serial_proxy] proxy paused; flashing %s" % port, flush=True)
    # Manual mode: for an adapter with no DTR/RTS auto-reset (e.g. a 3-wire FTDI), esptool
    # can't drop the chip into the bootloader itself. Walk the user through it and wait:
    # once the chip is already in download mode, esptool's (no-op) reset is harmless and it
    # syncs immediately. Needs a TTY for the prompt; if there isn't one (e.g. an agent's
    # non-interactive shell), bail with how to run it.
    if a.manual:
        print("\n[serial_proxy] MANUAL flash - adapter has no auto-reset. Put the ESP32 into\n"
              "  download mode:  1) hold BOOT(IO0)   2) tap EN/RESET   3) release BOOT\n"
              "  (serial log should show ...DOWNLOAD_BOOT...)", flush=True)
        try:
            input("  Press Enter when the board is in download mode (Ctrl-C to abort)... ")
        except EOFError:
            # No interactive stdin (e.g. driven by an agent / non-TTY shell): can't wait,
            # so proceed and trust the chip is ALREADY in download mode. esptool fails
            # cleanly if it isn't.
            print("  [no TTY - proceeding; chip must already be in download mode]", flush=True)
        except KeyboardInterrupt:
            if s and _alive(s["pid"]):
                os.kill(s["pid"], signal.SIGUSR2)       # resume the proxy we paused
            sys.exit("\n[serial_proxy] manual flash aborted.")
    rc = 1
    try:
        rc = subprocess.call(cmd)
    finally:
        if s and _alive(s["pid"]):
            if rc == 0 and not a.no_truncate:           # fresh, small log for the new firmware (default)
                try:
                    with open(s["log"], "w") as f:
                        f.write("===== flashed %s @ %s =====\n" % (a.env, time.strftime("%F %T")))
                    print("[serial_proxy] log truncated for new firmware", flush=True)
                except OSError:
                    pass
            os.kill(s["pid"], signal.SIGUSR2)           # resume
            time.sleep(0.4)
            if a.manual:
                # No auto-reset to boot the new image either; the chip is still parked in
                # the bootloader. The proxy is back on the port to catch the boot log.
                print("[serial_proxy] flashed (rc=%d). TAP EN/RESET to boot the new firmware -> %s"
                      % (rc, s["log"]), flush=True)
            else:
                os.kill(s["pid"], signal.SIGHUP)        # reset -> deterministically capture a clean boot
                print("[serial_proxy] proxy resumed + reset -> %s" % s["log"], flush=True)
    sys.exit(rc)


def cmd_send(a):
    s = load_state()
    if not s:
        sys.exit("no proxy running (start `serial_proxy.py monitor` first)")
    text = " ".join(a.text)
    with open(SEND, "a") as f:                      # proxy drains within ~200 ms
        f.write(text + "\n")
    print("[serial_proxy] queued console input -> %r" % text)


def cmd_reset(a):
    s = load_state()
    if not s:
        sys.exit("no proxy running")
    os.kill(s["pid"], signal.SIGHUP)
    print("[serial_proxy] reset pulse sent")


def cmd_stop(a):
    s = load_state()
    if not s:
        print("no proxy running")
        return
    os.kill(s["pid"], signal.SIGTERM)
    for _ in range(20):
        if not _alive(s["pid"]):
            break
        time.sleep(0.1)
    print("[serial_proxy] stopped pid %d" % s["pid"])


def cmd_status(a):
    s = load_state()
    print("proxy: pid %d  port %s  baud %s  log %s" % (s["pid"], s["port"], s["baud"], s["log"])
          if s else "proxy: not running")


def cmd_tail(a):
    log = (load_state() or {}).get("log", a.log)
    open(log, "a").close()
    os.execvp("tail", ["tail", "-n", "+1", "-F", log])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("monitor", help="own the port + tee to the shared log")
    m.add_argument("--port", default=None,
                   help="device path or /dev/serial/by-id substring (e.g. FTDI, CP2102); "
                        "default: autodetect (currently %s)" % DEFAULT_PORT)
    m.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    m.add_argument("--log", default=DEFAULT_LOG)
    m.add_argument("--no-reset", action="store_true")
    m.set_defaults(fn=cmd_monitor)

    f = sub.add_parser("flash", help="pause proxy, build+upload, resume (truncates log by default)")
    f.add_argument("--env", default="prod")
    f.add_argument("--port", default=None,
                   help="upload port for a direct (no-proxy) flash; device path or by-id "
                        "substring. When a proxy is running its port is used instead.")
    f.add_argument("--manual", action="store_true",
                   help="adapter has no auto-reset (3-wire): prompt + wait for you to enter "
                        "download mode (hold BOOT, tap EN), then remind you to tap EN to boot")
    f.add_argument("--no-truncate", action="store_true",
                   help="keep the existing log instead of truncating it for the new firmware")
    f.add_argument("extra", nargs=argparse.REMAINDER)
    f.set_defaults(fn=cmd_flash)

    sd = sub.add_parser("send", help="inject a console line to the device (proxy keeps the port + log)")
    sd.add_argument("text", nargs="+", help="the command line, e.g. `send sleep` or `send mode a`")
    sd.set_defaults(fn=cmd_send)

    for name, fn in (("reset", cmd_reset), ("stop", cmd_stop), ("status", cmd_status)):
        sp = sub.add_parser(name)
        sp.set_defaults(fn=fn)
    t = sub.add_parser("tail")
    t.add_argument("--log", default=DEFAULT_LOG)
    t.set_defaults(fn=cmd_tail)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
