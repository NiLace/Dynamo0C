# Changelog

All notable changes to **Dynamo 0C EQ** are recorded here, newest first. Versions
follow [semantic versioning](https://semver.org).

## [1.1.0] — 2026-07-13

A big release. The equaliser was rebuilt from the ground up for higher fidelity, the
channel drive gained optional anti-aliasing, and the interface got a settings window.

> **Heads-up:** the **top end sounds different from 1.0.0** — on purpose. The old EQ
> pinched the highs in near Nyquist ("cramping"); the new one keeps the analogue
> curve all the way up. Boosts and shelves up high are truer and a touch brighter.

### Changed — sound & quality
- **EQ rebuilt as an analogue-matched design.** Each band used to be a bilinear
  filter propped up by oversampling; it's now a closed-form *matched-Z + FIR
  correction* that reproduces the analogue magnitude right up to Nyquist **without
  oversampling, without cramping, and at zero latency**. Measured against the
  analogue prototype it tracks to **< 0.03 dB across the audible band** (worst case
  ~0.46 dB only at an extreme hi-shelf boost), and it's **~25–50× cheaper** than the
  oversampling it replaces.
- **Higher hi-shelf fidelity.** The FIR magnitude correction went from order 3 to
  order 5, which roughly **halves the worst-case error** (the extreme hi-shelf
  shoulder dropped from ~0.87 dB to ~0.45 dB) and tightens every band.

### Added
- **Drive oversampling (1x / 2x / 4x).** The channel drive is non-linear, so it
  aliases; you can now oversample just the drive to push that aliasing far below the
  noise floor. It only engages **while the drive is on**, and only then adds a small,
  host-compensated latency. **1x** is the default and is bit-identical to the old
  drive (zero latency).
- **Settings window.** Click the plugin title (“DYNAMO 0C”) to open an overlay card
  — it turns orange while open — that hosts the Drive Oversampling selector. Click
  the title again, the ✕, or outside the card to close it.
- **The graphical interface is now built by default** (`make`); use `BUILDUI=no`
  for a headless DSP-only build.

### Removed
- **The EQ oversampling control is gone.** With the analogue-matched design the EQ
  never needs oversampling (it already matches the analogue up to Nyquist), so the
  old *Oversampling* selector — including its "Auto" mode, which had become just a
  synonym for 1x — was removed along with all of its code. The EQ now always runs at
  the session rate. The only oversampling left is the drive's.

### Compatibility note
- **Control-port layout changed.** Removing the EQ *oversample* port shifted the
  control ports above it (latency, HP-bump, drive) and the drive-oversampling port
  now sits at the end. Hosts read these by name, so re-inserting the plugin is fine,
  but any saved **automation that referenced the old ports by numeric index** may
  need re-pointing.

### Fidelity notes (for the curious)
- Measured against the analogue prototype it's modelled on, the EQ tracks the target
  magnitude to **< 0.03 dB across the audible band** — worst case ~0.46 dB only at an
  extreme hi-shelf boost. The realisation matches its own design to the float floor.

## [1.0.0] — 2026-06-27

- Initial release. Four proportional-Q bands (Low / Lo-Mid / Hi-Mid / High), with the
  outer two switchable bell ↔ shelf; high-pass and low-pass filters with a resonant
  **BUMP** option; an asymmetric analogue-style channel drive; and a custom
  hardware-style interface (Pugl + Cairo). Stereo LV2 for Linux.

---

**Compare between versions** (full source diff):

- **1.0.0 → 1.1.0:** <https://github.com/NiLace/Dynamo0C/compare/v1.0.0...v1.1.0>

[1.1.0]: https://github.com/NiLace/Dynamo0C/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/NiLace/Dynamo0C/releases/tag/v1.0.0
