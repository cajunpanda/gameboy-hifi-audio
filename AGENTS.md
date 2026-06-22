# AGENTS.md

Guidance for AI coding agents working in this repository.

## What this is

GameBoy HiFi Audio: Bluetooth A2DP source firmware and custom hardware for a
Game Boy Advance audio mod. The mod removes the GBA's stock audio amp and makes
an ESP32 plus an ES8388 codec the whole audio back end. It captures the GBA
CPU's pre-volume audio, runs DSP (EQ, effects, volume), streams it to Bluetooth,
and drives the local speaker and wired headphones.

The reader docs are the place to start:

- `docs/MANUAL.md`: end-user operation.
- `docs/AGB-INSTALL.md`: fitting the assembled mod into a Game Boy Advance.
- `docs/HARDWARE.md`: boards, BOM, fabrication, assembly, grounding.
- `docs/FIRMWARE.md`: toolchain, build/flash, architecture, config, OTA.

## Build, flash, monitor

All firmware commands run from `firmware/`:

```sh
cd firmware
pio run                          # build
pio run -t upload                # flash
pio run -t upload -t monitor     # flash + monitor
pio run -t menuconfig            # IDF menuconfig
```

`pio device monitor` needs a TTY, which a sandboxed agent shell usually lacks.
Use `tools/serial_proxy.py` instead: `monitor` starts it, `tail -f
/tmp/gba_serial.log` watches it, `flash --env prod` builds and uploads.
Never `pkill -f`/`pgrep -f` the proxy (the `-f` match kills the shell); use the
`stop` subcommand.

`sdkconfig.defaults` is the source of truth. The generated `sdkconfig.prod`
is not checked in; delete it to regenerate. Adding a new `CONFIG_*` needs that
delete plus a rebuild.

## Things that will bite you

- Pin the real-time audio tasks to core 1, off the BT controller (core 0), or
  A2DP streaming garbles. Tickless idle stays off for the same reason.
- The pipeline is locked to 44.1 kHz interleaved stereo int16. The IDF A2DP
  source rejects any other SBC format. Do not change one of sample rate,
  channels, or bit depth without the others.
- The ESP32 is the I2S master; MCLK is on GPIO0 (APLL). On this silicon the
  reported MCLK divider is unreliable, and the live MCLK couples into the codec
  I2C bus, so codec config is verify-and-fixed after the writes.
- Do not read-back-verify live codec I2C writes (mode/volume). Heavy live reads
  can hang the bus and trip the watchdog. Read-backs belong only in the boot
  verify path.
- Do not re-page a Bluetooth sink immediately on disconnect; defer the first
  reconnect to the periodic timer, or Bluedroid crashes during teardown.
- `settings.c` is the only seam control surfaces touch. The console and the BLE
  server call `settings_*`/`sfx_*` and never touch the audio path. Keep that.
- `pinmap.h` is the single source of truth for GPIOs. Do not hard-code pin
  numbers anywhere else.
- `pio run -t upload` does not flash the LittleFS clip image. Build
  `littlefs_storage_bin` and write it to the `storage` offset in
  `partitions.csv`.

## Style

Comments are terse and describe current behavior only. No change history, no
dated notes, no "Rev/Option/Stage" references. Plain ASCII (write I2S, I2C).
Keep prose and comments human: no em dashes, no emoji.
