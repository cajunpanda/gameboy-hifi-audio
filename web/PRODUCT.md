# Product

## Register

product

## Users

Owners of the **GameBoy HiFi** audio mod — a hardware kit that adds hi-fi
Bluetooth audio and DSP to a Game Boy Advance — plus the maker selling the kit.
Mostly hobbyist/enthusiast buyers. They open this page to tune sound, usually on
a **phone** next to the console, occasionally on a **desktop** at the bench.

Primary job on any visit: connect over Bluetooth, watch the **live spectrum**,
and adjust the **EQ for whatever they're currently listening on** (speaker,
headphones, or Bluetooth). Firmware updates, R-button hold timings, sound cues,
and the device console are secondary.

## Product Purpose

The single Web Bluetooth control panel for the mod's BLE config server
(`firmware/src/ble_config.c`). It is the main interface a buyer touches, so it
doubles as the product's face. Success = a buyer connects on the first try,
immediately understands the spectrum + EQ, and dials in their sound without a
manual.

## Brand Personality

Precision instrument; serious-playful; honest. Three words: **instrument,
tactile, unfussy.** It should feel like boutique audio test gear (an
oscilloscope, Teenage Engineering) that happens to live in a Game Boy. The Game
Boy heritage shows through a refined phosphor-lime "signal" color and light
copy — never kitsch pixel-green.

## Anti-references

- Kitschy retro: DMG pea-green everything, pixel fonts, chiptune skeuomorphism.
- Generic dark-neon "gamer" dashboards.
- SaaS-cream, glassmorphism, gradient-and-glow decoration.
- Marketing scaffolding: tracked uppercase eyebrows, hero-metric templates,
  over-decorated glossy buttons.

## Design Principles

1. **The spectrum + the current output's EQ is the whole point.** Everything
   else is one tap away, never in the way.
2. **Read like an instrument.** Monospace readouts, hairline panels, honest
   units. No decoration that isn't signal.
3. **Follow the hardware.** The UI auto-selects the output the user is actually
   hearing (speaker / headphones / Bluetooth) from the live audio source.
4. **Robust over pretty when they conflict.** Web Bluetooth on Linux is flaky,
   so the connection self-heals (retry + auto-reconnect) rather than looking
   nice and failing.
5. **One self-contained file.** No build, no external requests — GitHub Pages
   hosts it as-is and it stays forkable.

## Accessibility & Inclusion

- Dark theme only (used screen-lit at a desk or on a couch). Contrast verified:
  body and labels ≥ 4.5:1 on graphite; lime/violet used at high-contrast
  weights; near-black text on lime fills.
- Full `:focus-visible` on every control and link; native form controls (range,
  checkbox, number, text) for assistive-tech support.
- `prefers-reduced-motion` honored — view transitions and entrance animations
  collapse to instant.
- Not color-only: state carries text alongside color (Connected, mode name,
  source label); stereo channels are labeled **L / R**, not distinguished by
  color alone.
