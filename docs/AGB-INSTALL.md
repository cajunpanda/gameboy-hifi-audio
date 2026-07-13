# GameBoy HiFi Audio: AGB Installation Guide

This guide covers fitting a GameBoy HiFi board into a Game Boy Advance: removing
the stock parts, soldering on the flex, wiring the speaker, and the first
firmware flash. You need an assembled main board and flex, either from a kit or
built and assembled per [HARDWARE.md](HARDWARE.md).

This is an advanced modification. It involves removing surface-mount parts from
the GBA mainboard and fine-pitch soldering. If you are not comfortable with that,
have it done by someone who is. The changes are permanent: the mod becomes the
console's audio system.

## What you need

- An assembled main board and flex.
- A hot-air station (recommended), or a low-melt desoldering alloy such as
  Chipquik, for lifting U6 and CP4 cleanly.
- A fine-tip soldering iron, flux, and fine solder.
- A Tag-Connect TC2030 serial cable for the first flash, only if you assembled
  the board yourself (see First firmware flash below). Prebuilt kits ship
  pre-flashed and need no cable.
- GBA opening tools (a tri-wing driver for the shell).

## Remove the stock parts

Open the GBA and take out the mainboard. Then remove these parts from it:

- U6, the stock audio amplifier.
- CP4, the coupling caps.
- R30 and R31.
- The speaker.

Use hot air to lift U6 and CP4. If you do not have hot air, flood their pins with
a low-melt alloy (Chipquik or similar) and lift them with the iron. Work
carefully: the flex solders back onto these same footprints, so keep the pads
clean and intact. R30, R31, and the speaker come off with a regular iron.

<!-- photo: U6, CP4, R30, R31, and the speaker removed -->

## Solder on the flex

The flex solders to the freed footprints: U6, CP4, R30, R31, and the speaker
pads. That is how the mod picks up power, ground, the audio taps, the headphone
sleeve return, and the R-button net, and it anchors the flex at the speaker
holes.

Solder the flex exactly per its silkscreen and the KiCad flex project. Follow
that pad mapping, not a generic pinout.

### Tapping the CPU audio (two ways)

The two audio signals (the CPU sound pins S0 and S1) can reach the flex by either
route. Pick whichever you find easier; the result is electrically the same.

- **At the R30 / R31 pads (default).** The flex lands solder directly onto the
  R30 and R31 footprints. These are small pads right at the flex edge, so this is
  the fiddliest joint of the whole install.
- **Through the U6 footprint (alternative).** Bridge across each of the R30 and
  R31 footprints with a short length of fine wire, so the audio passes straight
  through to the U6 footprint. The flex then picks S0 and S1 up at the U6 pads
  (where it already solders), and nothing has to land on the tiny R30 / R31 flex
  lands.

If an S0 or S1 pad lifts or is damaged while you remove the stock parts, bridge
from it to the nearby S0 / S1 via instead. It is the same net, so the tap still
works.

<!-- photo: flex soldered onto the footprints -->

## Connect the boards and the speaker

1. Connect the flex to the main board through the flat-flex connector.
2. Solder the speaker to the main board. The mod's own amplifier drives it.
3. Fold the flex as designed and secure the main board inside the shell.

<!-- photo: flex connected, speaker wired, board secured -->

### Choosing the speaker

The speaker is an off-PCB accessory (not in the schematic BOM). It solders to the
main board's `SPK+` / `SPK-` pads (J5 / J6). Any 23 mm 8 Ω driver fits the
footprint; the choice that matters is **efficiency**, because U4 (PAM8302A) runs
on the 3.3 V rail and delivers only ~0.5 W, so a high-sensitivity driver is what
buys usable, clean volume. Do not go below 4 Ω (the amp's minimum load).

Recommended parts (all 23 mm, 8 Ω, NdFeB):

| Role          | Part                     | Size            | Sensitivity       | Notes                                                                                                             |
| ------------- | ------------------------ | --------------- | ----------------- | ----------------------------------------------------------------------------------------------------------------- |
| Primary       | PUI Audio `AS02308MR-T`  | Ø22.5 × 7.15 mm | 97 dB @ 1 W/0.1 m | PEN cone, solder pads. **Fit-test the 7.15 mm height** — tight in the AGB shell (a known-good speaker is 6.5 mm). |
| 2nd source    | Same Sky `CMS-2207-18SP` | Ø22.5 × 7.0 mm  | 97 dB @ 1 W/0.1 m | Spec-identical drop-in for the PUI; use whichever is in stock. Same fit caveat.                                   |
| Slim fallback | Soberton `SP-2305L`      | Ø23 × 5.0 mm    | 89 dB             | 70 mm flying leads (suit the SPK+/SPK- wiring). Use if 7 mm fouls the shell; ~5 dB less efficient.                |

## First firmware flash

Prebuilt kits ship pre-flashed. If you bought one, skip this step and update over
Bluetooth from the web config page.

A board you assembled yourself has no firmware. Flash it once through the board's
Tag-Connect serial port, then later updates go over Bluetooth. The recommended
cable is the [Tag-Connect TC2030-NL-FTDI-C232HD](https://www.tag-connect.com/product/tc2030-nl-ftdi-c232hd-ddhsp-0-dtr-usb-to-tc2030-no-legs-serial-cable),
which carries the DTR line so the flasher can reset the board into the bootloader
on its own.

See [FIRMWARE.md](FIRMWARE.md) for the build and flash steps.

## After installation

Reassemble the console and power it on. The mod runs off the GBA's battery rail,
so it starts with the console. See [MANUAL.md](MANUAL.md) for pairing, the
control button, and the operating modes.

## Cautions

- Removing U6 and CP4 is permanent. The mod becomes the GBA's audio system.
- Desolder U6 and CP4 with hot air or a low-melt alloy so you do not lift their
  pads; the flex reuses those footprints.
