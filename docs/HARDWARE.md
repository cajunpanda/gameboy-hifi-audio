# GameBoy HiFi Audio: Hardware Guide

This guide is for building the mod yourself: ordering the boards, sourcing
parts, assembling, and installing it in a Game Boy Advance. The KiCad projects
under `hardware/agb/` are the authoritative reference for the schematic, the
board layout, and the exact net and pin assignments. This document is the
overview.

This is an advanced modification. It requires removing a surface-mount chip from
the GBA mainboard, fine-pitch soldering, and reflow or hot-air work on a QFN
part. If you are not comfortable with that, have it done by someone who is.

## What is on the boards

There are two boards for the AGB:

- **Main board** (`hardware/agb/agb-hifi-audio-pcb`): the ESP32 module, the
  audio codec, the power supply, and the speaker amplifier.
- **Flex** (`hardware/agb/agb-hifi-audio-fpc`): a flexible circuit that solders
  to the GBA's audio-amp footprint and carries power, ground, the audio taps,
  and the control-button signal back to the main board. It folds over once
  during installation.

## Key parts

The bill of materials lives in the KiCad schematic itself (the source of truth),
where each symbol carries its manufacturer, LCSC, and DigiKey part numbers. Export
it from the [main-board schematic](../hardware/agb/agb-hifi-audio-pcb/). The main
active parts are:

| Ref  | Part                    | Function                                                   |
| ---- | ----------------------- | ---------------------------------------------------------- |
| U1   | ESP32-PICO-MINI-02-N8R2 | MCU, Bluetooth radio, 8 MB flash                           |
| U2   | ES8388                  | Stereo audio codec: ADC, DAC, headphone amp, analog bypass |
| U3   | TPS61021A               | Boost converter, battery to 3.3 V                          |
| U4   | PAM8302A                | Mono Class-D amplifier for the internal speaker            |
| U5   | LP5907MFX-3.0           | Low-noise LDO for the codec's analog supply                |
| FPC1 | FH12A-12S-0.5SH         | 12-pin flat-flex connector for the flex                    |

The ES8388 is a QFN-28 and is not hand-solderable in the usual sense. Plan on a
stencil and reflow, or hot air. The ESP32 module needs the same treatment: its
pads sit under the module's edge rather than as exposed castellations, so an iron
cannot reach them all. Its pitch is coarser than the codec's, so it is the more
forgiving of the two, but it is still a reflow or hot-air part.

### Full bill of materials

Passives are listed by value; the schematic carries the exact manufacturer,
LCSC, and DigiKey part numbers for every line. 26 lines, 60 pieces.

| Ref(s)                         | Qty | Value / part                   | Package           |
| ------------------------------ | --- | ------------------------------ | ----------------- |
| C1, C2, C32–C35                | 6   | 22 µF 25 V                     | 0805              |
| C3, C6, C8, C10, C11, C20, C24 | 7   | 10 µF                          | 0402              |
| C4, C7, C9, C21, C25, C26, C30 | 7   | 100 nF                         | 0402              |
| C5, C15, C16, C19, C27, C31    | 6   | 1 µF                           | 0402              |
| C12                            | 1   | 4.7 µF                         | 0402              |
| C13, C14                       | 2   | 10 nF C0G                      | 0402              |
| C17, C18                       | 2   | 220 µF 6.3 V (tantalum)        | EIA-3528          |
| C22, C23                       | 2   | 10 nF                          | 0402              |
| C29                            | 1   | 10 pF                          | 0402              |
| R1                             | 1   | 316 kΩ                         | 0402              |
| R2, R9, R20, R21               | 4   | 100 kΩ                         | 0402              |
| R3, R4                         | 2   | 2.2 kΩ                         | 0402              |
| R7, R8                         | 2   | 15 kΩ                          | 0402              |
| R10, R13, R14                  | 3   | 10 kΩ                          | 0402              |
| R11, R12                       | 2   | 1 kΩ                           | 0402              |
| R15, R16                       | 2   | 4.7 kΩ                         | 0402              |
| R17                            | 1   | 10 Ω                           | 0402              |
| R19                            | 1   | 33 Ω                           | 0402              |
| L1                             | 1   | 0.47 µH (power inductor)       | 2.5 × 2.0 mm      |
| FB1                            | 1   | 600 Ω @ 100 MHz (ferrite bead) | 0402              |
| FPC1                           | 1   | FH12A-12S-0.5SH                | 12-pin FFC        |
| U1                             | 1   | ESP32-PICO-MINI-02-N8R2        | SMD module        |
| U2                             | 1   | ES8388                         | QFN-28 (4 × 4 mm) |
| U3                             | 1   | TPS61021A                      | WSON-8 (2 × 2 mm) |
| U4                             | 1   | PAM8302A                       | MSOP-8            |
| U5                             | 1   | LP5907MFX-3.0                  | SOT-23-5          |

## Ordering the boards

Open the KiCad project (KiCad 10 or newer) and export fabrication data:

1. Run the design rule check and the electrical rule check first.
2. Plot Gerbers and drill files for the main board, and again for the flex.
3. Export the position (centroid) file if you are using an assembly service.

The flex needs special fabrication settings: it is a two-layer polyimide flex
with a stiffener under the connector fingers, and a few intentional edge-plated
lands. Those requirements are written into the flex board as fabrication notes
on the documentation layer, so they travel with the Gerbers. Read them before
you order, and tell the fab house it is a flex, not a rigid board.

JLCPCB and PCBWay both make these. For the main board, their assembly services
can place most of the parts, which saves you the QFN reflow.

## Power

The GBA's switched battery rail (about 2.4 V to 3.2 V) feeds the TPS61021A
boost converter, which produces 3.3 V for the ESP32, the codec's digital side,
and the speaker amp.

The codec's analog supply (AVDD, and HPVDD behind R17) is its own domain: the
3.3 V rail passes through the ferrite (FB1) into U5, an LP5907MFX-3.0 low-noise
LDO (~82 dB PSRR at 1 kHz), which regulates a quiet 3.0 V for the codec's analog
stages. It isolates the codec's DAC, mixer, and headphone amp from the boost
rail, which the Bluetooth radio's wake-up bursts modulate. Bulk caps are spread
across the rail (boost output, boost input, ESP32, speaker amp) for brown-out
margin at low battery.

## The audio source

The GBA CPU does not output a line-level analog signal. Its sound pins (S01 and
S02) are a PWM bitstream. The reconstruction that turned that into analog used
to live inside the amp chip you are removing, so the mod includes its own
reconstruction low-pass filter at the tap. The filtered signal feeds the codec's
line inputs. The codec's input range suits this signal directly, so there is no
preamp.

## Installing the mod

Once the boards are assembled, see [AGB-INSTALL.md](AGB-INSTALL.md) to fit the mod into
a Game Boy Advance: removing the stock parts, attaching the flex, wiring the
speaker, and the first firmware flash.
