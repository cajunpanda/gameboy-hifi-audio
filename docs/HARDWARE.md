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

### Antenna

The `-02U` module has an **external antenna connector**, not an on-board PCB
antenna. Per the Espressif datasheet it is a **third-generation I-PEX MHF III**
connector, also sold as **Hirose W.FL** or **Amphenol AMMC** — the three names
are the same interface. Order an antenna terminated in one of those.

Do **not** buy a U.FL / IPEX MHF1 antenna (the common default): it is physically
larger and will not seat on the MHF III connector. MHF4 is too small.

Native MHF III antennas are uncommon and mostly special-order, and the one FPC
type that is stocked (Kyocera `9000352F0`) is a *"for metal surfaces"* antenna
that detunes on a plastic shell. So the build uses a common **free-space
2.4 GHz FPC antenna** (designed for plastic enclosures) plus a small adapter to
bridge the connector generations:

- **`ANT1` — Taoglas `FXP70.07.0053A`**: adhesive flexible FPC, single-band
  2.4 GHz, ~53 mm lead, 1.1 dBi, terminated in **U.FL / IPEX MHF1**. Mount it
  against the plastic, with keep-out from metal and the PCB ground. Stocked,
  MOQ 1. For more range use `FXP73.07.0100A` (100 mm, 2.5 dBi).
- **`ADPT1` — MHF III (plug) → U.FL (jack) adapter**: a short (~50 mm) coax that
  presents an MHF III / W.FL **plug** to the module and a U.FL **jack** for the
  antenna. This is not a distributor part — order it from an RF pigtail vendor
  (Data Alliance, superbatrf, eteily) or AliExpress.

The adapter adds ~10 cm of total cable and two extra micro-connections, so keep
it as short as possible and coil the slack inside the shell. If you would rather
avoid the adapter, order the antenna itself terminated in MHF III (a special
order with MOQ/lead time) and drop `ADPT1`.

Do **not** substitute a U.FL / MHF1 antenna directly onto the module: the module
is MHF III and the two do not mate — that mismatch is what the adapter resolves.
The `FXP70`'s 1.1 dBi sits within the datasheet's ≤ 2.33 dBi certified envelope;
if you swap to a higher-gain antenna you may exceed the module's FCC/CE modular
grant. Both `ANT1` and `ADPT1` are cabled accessories (marked do-not-place) — not
assembled onto the board. Press each connector straight down to mate it; MHF III
is fragile and rated for only a handful of mating cycles.

#### Bench bring-up

For integration testing you do not need the final in-shell antenna. The simplest
rig that mates the module directly is an external whip on a bulkhead pigtail:

- **W.FL → RP-SMA-female bulkhead cable** (0.81 mm coax), e.g. Data Alliance's
  W.FL-to-RP-SMA-female cable. The W.FL end is an MHF III **plug** that mates the
  module; the RP-SMA-female end takes any RP-SMA-male whip.
- **RP-SMA-male 2.4 GHz whip**, e.g. a 5 dBi articulating antenna.

Full chain: `module MHF III jack → W.FL plug —[coax]— RP-SMA-F bulkhead ←
RP-SMA-M whip`. The 5 dBi whip exceeds the module's ≤ 2.33 dBi certified
envelope, which is fine on the bench but must not ship in a certified product.
Do **not** try to re-terminate spare U.FL/MHF1 FPC antennas onto MHF III by hand:
that connector is a crimp-plus-center-pin-solder onto thin coax, not a solder
swap, and hand rework gives unreliable VSWR — use the pigtail instead.

#### Production sourcing

MHF III antennas are uncommon off-the-shelf, but this needs a catalog antenna
with the MHF III connector option, not a bespoke antenna design. In order of
preference:

1. **2J Antennas** — flexible connector/cable-length configurator, friendlier
   MOQs; ships an FXP-class flex antenna with an MHF III plug and a chosen lead
   length. Removes `ADPT1`.
2. **Taoglas `FXP70`/`FXP73` with the MHF III connector code** — the same
   free-space antenna as `ANT1`, factory-terminated in MHF III (custom-line SKU,
   MOQ + lead time). Removes `ADPT1`.
3. **Kyocera `9000352F0-AE20L0050`** (50 mm MHF III) — MOQ 200, ~9-week lead; a
   normal buy at run sizes near 200, but it is the metal-surface type so back it
   with the PCB ground.

Budget an MOQ of a few hundred and an 8–12 week lead, and validate VSWR/range
with the antenna in its final position inside the actual GBA shell — the plastic,
battery, and PCB all detune it.

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
