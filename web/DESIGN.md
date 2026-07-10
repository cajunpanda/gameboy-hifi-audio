# Design

Precision-instrument control panel for the GameBoy HiFi audio mod. A single
self-contained `index.html` (no build, no external assets) served from GitHub
Pages. Dark graphite enclosure, a phosphor-lime "signal" accent (a refined nod
to the DMG screen), violet as the stereo R-channel. Everything technical is
monospace; prose is a humanist sans.

## Theme

- Dark only (`color-scheme: dark`). **Committed** color strategy (one saturated
  signal color carries active states) on an otherwise restrained product surface.
- Metaphor: boutique audio test gear — hairline panels, corner ticks, monospace
  readouts, a live analyzer as the hero.

## Color (OKLCH)

Graphite neutral ramp (cool, hue ~200):

- `--bg` 0.165 · `--bg-2` 0.195 · `--surface` 0.216 · `--surface-2` 0.245 · `--control` 0.275
- `--line` 0.315 · `--line-2` 0.40 (hairlines, borders)
- Ink: `--ink` 0.965 · `--muted` 0.755 · `--faint` 0.605

**Signal — phosphor lime** (the one hot accent: active states, values, primary
actions):

- `--signal` 0.865 0.19 129 · `--signal-2` 0.80 · `--signal-dim` 0.60 · `--signal-soft` (15% signal)
- `--signal-ink` 0.20 0.04 130 — near-black text on lime fills (buttons, toggles, active segment)

**Violet — stereo R-channel + secondary:** `--violet` 0.64 0.19 292 · `--violet-2` 0.72

Semantic: `--warn` 0.82 0.14 82 · `--danger` 0.66 0.19 25. The Connected / OK /
Full-mode states reuse signal lime; Battery saver uses warn amber.

Contrast: body and labels sit at `--muted` / `--ink` on graphite (≥ 4.5:1); lime
and violet are used at high L for values and large text; near-black on lime for
button text.

## Typography

Two families on a **contrast axis (monospace vs humanist)** — no external fonts:

- `--font-sans` = `system-ui` stack — prose, control labels, headings, help text.
- `--font-mono` = `ui-monospace` stack — wordmark, section nav, every numeric
  readout (dB / % / ms), status pill, and equipment micro-labels.

Rationale: monospace *is* the instrument voice, and the mono/sans split carries
the hierarchy, so no display font is needed (preserves the single-file, offline
constraint). Fixed rem scale with tight ratios (product register).

Equipment **micro-labels** (`.label-mono` and the analyzer/output labels): mono,
uppercase, 0.11em tracking, `--faint`. These are diegetic gear labeling (analyzer
header, L/R legend, unit hints) — deliberately *not* the tracked-uppercase
marketing eyebrow anti-pattern.

## Layout — responsive app console

- Content max-width ~46rem. Breakpoint at **820px**.
- **Desktop (≥ 820px):** two-pane grid. A 15rem left **rail** (wordmark +
  `AUDIO MOD`, vertical Tune/Device/System nav with a lime active dot, and a
  pinned footer: mode chip · Save to device · Disconnect) beside a scrolling
  **main** pane led by the analyzer.
- **Mobile (< 820px):** the rail dissolves via `display: contents`. Its top block
  becomes a sticky top bar (wordmark + status pill + underline tab nav); its
  footer becomes a floating **fixed bottom action bar** (mode · Disconnect ·
  Save), with the "Saved ✓" confirmation floating above it.
- Container queries adapt inner grids (e.g. R-button hold timings go 2-up on
  wider main).

## Components

- **Spectrum analyzer (hero):** bordered instrument panel with corner ticks, an
  `ANALYZER` header + live dot + source label, a canvas stereo meter (lime L up /
  violet R down, gradient bars, slow-decay peak caps, faint dB gridlines, a 0 dB
  center line) and an L/R legend. DPR-scaled. Auto-labels and follows the live
  audio source.
- **Output segmented control:** 3-up Speaker / Headphones / Bluetooth; active =
  lime fill + near-black text. Drives which single EQ + volume shows — one
  contextual EQ, not three stacked.
- **Sliders:** thin graphite track, ink thumb ringed in lime, mono value readout
  in lime. EQ bands dim via `:has()` when that EQ is bypassed.
- **Toggles:** graphite off → lime on (near-black knob).
- **Buttons:** primary = solid lime + near-black text; ghost = outline. Flat — no
  gradient, no glow.
- **Status pill / mode chip:** mono; lime when Connected / Full mode, amber for
  Battery saver.
- **Cards:** graphite surface, hairline border, sentence-case titles (no
  eyebrows). Firmware/OTA drop zone, device console (mono), diagnostics/activity
  log.
- **Links:** lime, underlined.

## Motion

- 150–250 ms, state-conveying only. View Transitions crossfade section switches;
  a short staggered entrance plays once on connect.
- The analyzer runs on `requestAnimationFrame` (fast-attack / slow-decay
  smoothing + peak decay).
- `prefers-reduced-motion`: animations and transitions collapse to instant; view
  transitions disabled.

## Constraints

- Single self-contained `index.html`; inline CSS/JS; **no build step, no external
  requests** (CSP-safe, offline-friendly, forkable). Deployed to GitHub Pages via
  `.github/workflows/pages.yml`.
- Web Bluetooth only (Chrome/Edge desktop/Android; iPhone via a Web BLE browser
  app such as Bluefy or BLE-Link). All state comes from the BLE config server;
  no persistence beyond the device.
