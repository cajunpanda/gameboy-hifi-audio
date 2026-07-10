# Cable-free dev over BLE

Flash firmware and use the debug console over Bluetooth LE, without the Tag-Connect
cable. The generic tool lives in the **serial-proxy skill** (`ble_proxy.py`); this repo
only carries a device profile, `ble-proxy.json` (in the repo root), which supplies the
device name and GATT UUIDs so the tool "just works" when run from anywhere in the tree.

`ble_proxy.py` needs `bleak`; it's installed in `tools/.venv` (gitignored). Run it via
that interpreter:

```
PY=tools/.venv/bin/python
SP=~/.claude/skills/serial-proxy/ble_proxy.py

$PY $SP info                 # what's installed? -> version|slot|builddate
$PY $SP flash                # OTA the nearest .pio/build/*/firmware.bin
$PY $SP monitor &            # own the BLE link, tee log -> /tmp/ble_proxy.log
$PY $SP send get             # inject a console command (same set as the UART REPL)
tail -f /tmp/ble_proxy.log   # follow it
```

`flash`/`info` auto-coordinate with a running `monitor` (release → flash → resume), since
the peripheral accepts one central at a time.

## How it works here

- **OTA** — already implemented in `firmware/src/ble_config.c` (OTACTL/OTADATA chars,
  windowed flow control, crc32 + SHA-256, rollback). ~35 KiB/s, ~33 s for the 1.18 MB image.
- **Console** — `ble_config.c` adds a LOG notify char (fed by an `esp_log_set_vprintf` hook
  that chains to the UART logger) and a CMD write char (dispatched via `esp_console_run()`,
  reusing `console.c`'s command table, stdout captured and streamed back).

Both are additive: the UART REPL and serial-proxy build/flash/monitor loop are unchanged.

## Cable is still required for

- The **first flash** onto a partition layout (can't OTA into a layout the running image
  predates).
- **Brick recovery** — BLE OTA/console both need a running image whose BLE stack comes up.

Use the cable for the fast inner loop and recovery; BLE for cable-free iteration on
known-good images. See the serial-proxy skill's SKILL.md for the full `ble_proxy.py` docs
and the reference OTA protocol spec.

## Setup

```
python3 -m venv tools/.venv && tools/.venv/bin/pip install bleak
```

Needs a working BlueZ adapter (`bluetoothctl show` → Powered: yes).
