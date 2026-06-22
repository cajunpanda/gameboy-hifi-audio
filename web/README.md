# Web config page

A single-file Web Bluetooth control panel for the mod's BLE config server
(`firmware/src/ble_config.c`). It tunes the DSP settings (speaker and Bluetooth
volume, EQ, sound effects), sets the R-button hold timings, plays chimes and
clips, saves to the device, and updates the firmware over the air. No app, no
WiFi, just Chrome or Edge talking to the ESP32 over Bluetooth.

## Hosting (GitHub Pages)

`.github/workflows/pages.yml` deploys this folder to GitHub Pages on every push
to `main` that touches `web/`. One-time setup: repo Settings, Pages, Source =
GitHub Actions. The page is then served at `https://<user>.github.io/<repo>/`.

Web Bluetooth needs a secure context, which the Pages HTTPS provides. For local
work, serve over localhost (for example `python3 -m http.server` in this folder).
Opening the file directly does not expose `navigator.bluetooth`.

## Use

1. Power the mod. The BLE server advertises as "GameBoy HiFi Setup" with slow
   advertising to keep current low, so discovery can take a couple of seconds.
2. Open the page, click Connect, and pick "GameBoy HiFi Setup".
3. The controls fill in from the device and push live edits back. Save to device
   persists them. Changes made from the UART console show up here through
   notifications.

On Linux, Chrome needs the experimental web platform features flag.

## Firmware update

The Firmware update card pushes a new `firmware.bin` to the device over
Bluetooth. Drop the file, click Flash firmware, and keep the page open until the
device reboots into the new image (a minute or two; audio pauses). The transfer
uses windowed flow control with a host-side crc32 and on-device SHA-256
validation before the boot slot swaps. Older firmware without the OTA
characteristics is detected, and the card disables itself.

The card shows the installed firmware version and slot from the device. The
version comes from `CONFIG_APP_PROJECT_VER` in `firmware/sdkconfig.defaults`.

## Wire format

The settings characteristic is a packed little-endian blob mirroring
`gbhifi_settings_t`. The exact layout is documented in both `web/index.html` and
`firmware/src/ble_config.c` (`SETTINGS_WIRE_LEN`); keep the two in lockstep. The
mode field is read-only status: entering Battery saver duty-cycles the radio to
sleep, so the page cannot reverse it. The R-button hold gesture owns the mode.
