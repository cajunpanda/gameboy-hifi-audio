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

The full bill of materials is in [`hardware/agb/bom.csv`](../hardware/agb/bom.csv),
with manufacturer and distributor part numbers. The main active parts are:

| Ref | Part | Function |
| --- | --- | --- |
| U1 | ESP32-PICO-MINI-02U-N8R2 | MCU, Bluetooth radio, 8 MB flash |
| U2 | ES8388 | Stereo audio codec: ADC, DAC, headphone amp, analog bypass |
| U3 | TPS61021A | Boost converter, battery to 3.3 V |
| U4 | PAM8302A | Mono Class-D amplifier for the internal speaker |
| FPC1 | FH12A-12S-0.5SH | 12-pin flat-flex connector for the flex |

The ES8388 is a QFN-28 and is not hand-solderable in the usual sense. Plan on a
stencil and reflow, or hot air. The ESP32 module is castellated and easier.

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

The mod runs on a single 3.3 V rail. The GBA's switched battery rail (about
2.4 V to 3.2 V) feeds the TPS61021A boost converter, which produces 3.3 V for
the ESP32, the codec, and the speaker amp. There is no separate regulator.

The codec's analog supply is filtered with a ferrite (FB1). The FB1 footprint
also accepts a small series resistor (4.7 to 10 ohm) in place of the ferrite, in
case you measure audio-band noise on the analog supply and want to clean it up
without a board change.

Do not tap the GBA's 5 V rail. Loading it overloads the console's own boost
converter. Power comes from the battery rail only.

## The audio source

The GBA CPU does not output a line-level analog signal. Its sound pins (S01 and
S02) are a PWM bitstream. The reconstruction that turned that into analog used
to live inside the amp chip you are removing, so the mod includes its own
reconstruction low-pass filter at the tap. The filtered signal feeds the codec's
line inputs. The codec's input range suits this signal directly, so there is no
preamp.

## Headphone-detect polarity

The GBA's headphone jack grounds the detect line when nothing is plugged in,
which is the opposite of most jacks. With the pull-up on the board, the ESP32
reads the line HIGH when a plug is present and LOW when it is not. The firmware
already accounts for this.

## The grounding rule

This is the one thing that is easy to get wrong and hard to debug, so read it
before you wire anything.

The mod has one main ground, GND (the GBA main ground and battery negative). It
carries the power, the digital signals, and the switching return current from
the boost converter, the Class-D amp, and the Bluetooth radio. It also carries
the codec and audio reference. A second net, AGND, is the headphone sleeve
return, brought in from the freed CP4 pad.

- GND and AGND meet at exactly one star point on the main board. Do not join
  them anywhere else.
- Keep AGND as its own conductor all the way back to that star point.
- On GND, route the codec's analog-reference branch locally, and let the
  switching return currents join near the star point, so switching current never
  flows through the quiet reference.

GND and the battery rail reach the main board through the flex. The control
button signal (the R shoulder net) rides the flex too. The two speaker leads are
the only separate wires.

## Installing the mod

Once the boards are assembled, see [AGB-INSTALL.md](AGB-INSTALL.md) to fit the mod into
a Game Boy Advance: removing the stock parts, attaching the flex, wiring the
speaker, and the first firmware flash.
