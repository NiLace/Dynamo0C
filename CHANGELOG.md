# Changelog

All notable changes to **Dynamo 0C EQ** are recorded here, newest first. Versions
follow [semantic versioning](https://semver.org).

## [1.4.0] — 2026-10-23

A new face and a harder shell. Dynamo 0C now looks like the other plugins of the family: a
photorealistic one-slot 500-series module. Underneath, a cold audit of the whole plugin found
ways a host could crash it or make it play the wrong filter, and every one is fixed and
guarded by a test that fails if the defect comes back. With its knobs set, the equaliser sounds as it did.
The plugin reports version 10.0 to the host (it was 8.0).

### Changed
- A new, photorealistic panel: Dynamo 0C is now drawn as a one-slot 500-series module, rendered from a 3D scene
  (brushed and anodised aluminium, black knobs with white heads, buttons set into the metal that light up from
  inside). Everything it did before it still does, in the same way: drag a knob (hold Shift for fine control),
  double-click to reset, click the title to bypass, click DRIVE for the oversampling card, LF and HF choose bell or
  shelf (their buttons, or the curve symbols above them, which light up to show the selected one). The panel images
  add 0.7 MB to the bundle.

### Fixed
- Closing the editor no longer frees the panel's fonts underneath the host's cairo. The last
  editor to close used to free its FreeType faces while cairo still held them, and cairo files
  those faces by memory address: a face the host opened later at the same address was handed the
  stale one. Each face now owns its FreeType library and is freed by FreeType itself when cairo
  lets go of it, so nothing is freed early and nothing calls back into an unloaded editor.
  A new gate (`make ui-unload`) opens and closes the editor twice, unloads it, and checks that
  every face the host opens afterwards comes back fresh; the old code fails it.
- **A broken control value can no longer crash the host.** A NaN on a band's frequency or gain
  port (from a misbehaving host, script or automation curve) crashed the host outright, and +Inf
  on DRIVE left the plugin outputting garbage for good. Every control input is now checked
  against the range the plugin declares: a NaN reads as the port's default, anything else is
  clamped.
- **A very loud input no longer kills the drive.** About +37 dBFS into the drive at full knob
  with 2x or 4x oversampling (two stock +20 dB gain stages before the plugin are enough) made the
  channel output NaN until the host re-activated the plugin. It now saturates as it should.
- **One bad input sample no longer silences the channel.** A single NaN or infinite input sample
  stayed in the filters' memory and the channel stayed dead until re-activation. Now the plugin
  outputs one silent block and carries on.
- **The EQ always lands on the right filter.** After a host primed the plugin with an empty
  `run(0)` call and then re-activated it, or flipped a switch, the EQ could play a filter built
  from the previous settings (measured: 10 dB off) until a knob moved again. And a band moved on
  the block right before a switch flip kept its approximate, interpolated curve for good (up to
  −36 dB of error) instead of its exact one.
- **Turning a knob slowly is cheaper.** While a knob was moving, every other block paid for a full
  exact filter design that the next block threw away; now a moving knob only interpolates, and
  the exact design lands once, when the knob stops.
- **An impossible sample rate is refused.** A host asking for a negative, zero or absurd rate got
  an instance whose drive could amplify instead of saturate (at −48 kHz, +10 dBFS out of a −6 dBFS
  input). The plugin now declines to load at such a rate, as LV2 allows; every real rate is
  unaffected.
- The plugin's shared table of pre-computed filters (0.6 MB) is freed when the last instance is
  removed, instead of being kept until the host exits.
- The reported latency at 2x is **14.75 samples** (the plugin always reported that; the port's
  description and the 1.3.0 notes below said 7.25).
- The plugin description validates against the LV2 specification (`sord_validate`: 0 errors).
- **The value shown while you turn a knob is now the real one.** It used to interpolate the
  numbers printed around the knob, which are the console's legend and not a measurement: at full
  boost a gain knob read +10.0 dB while the band really gave +11 to +12 dB (and a high shelf with
  its frequency at the bottom, +23 dB); a frequency knob at its centre could read half the frequency
  where the band actually peaks (LOW: 130 Hz shown, 267 Hz real). The read-out is now computed from
  the same circuit model the equaliser is built from: gain within 0.2 dB and frequency within 2 % of
  what the plugin does. The printed scales are unchanged.
- The plugin binary is 13 % smaller: it carried a third processor-specific copy of its audio code
  that no existing processor would ever select. Every processor still gets the same code path it
  did before.

## [1.3.0] — 2026-08-20

A quality-and-cost release for the channel drive. The drive now anti-aliases properly on
material it used to fail on, and it does so while costing **78 % less CPU than before**.

The equaliser is unchanged in behaviour. Measured against the published 1.2.0 binary over
224 configurations — every band, both filters, four sample rates, three signals, two block
sizes, with the drive off or at 1x — the worst residual is **−170.27 dB**, which is the
float noise floor rather than a difference anyone can hear; 738 of 1584 rendered channels
come out bit-identical. The last bits move because the audio loop now selects wider vector
instructions at load time on CPUs that have them.

> **Heads-up: the drive sounds different, and the factory default moved to 4x.** The
> aliasing that 2x left on bright, percussive material is gone. Existing sessions keep
> whatever oversampling they saved; only newly inserted instances get the new default.
> The reported latency at 4x also changed, from 22.62 to **16.93 samples**, because the
> filter is shorter. Your host compensates for it, but a project that had bounced stems
> against the old value will not line up sample-for-sample with a re-render.

### Fixed — the drive aliased, and it was not a small margin
- **4x is now the default and the only setting that meets the aliasing criterion.** The
  criterion is the worst-frame ANMR (a noise-to-mask ratio, A-weighted) below −10 dB.
  Measured on bright percussive material, the old default scored **+1.56 dB** — that is,
  the alias sat *above* the masking threshold — and it now scores **−17.84 dB**, with
  7.8 dB of margin.
- **2x cannot be fixed by a longer filter, and is now documented as an economy setting.**
  A deliberately oversized 64-tap filter at 2x only moves the score from −2.24 to −2.93 dB.
  At 2x the internal Nyquist is 48 kHz, so the fifth harmonic of an 18 kHz partial folds
  back to 6 kHz, in the middle of the audible band, where no decimation filter can reach
  it. The second stage is structural, not a refinement.
- **The drive knob no longer crunches while it is being turned.** The antiderivative
  anti-aliasing keeps one sample of state that is scaled by the drive gain; refreshing the
  gain without re-deriving it mixed two scalings and divided by a very small number. A knob
  move of a few thousandths produced a full-scale spike, at +8 dBFS of output peak. With
  the knob still, the output is bit-identical to before the fix.
- **Moving several controls at once no longer risks an xrun.** Redesigning every section
  in the block where its knob moved peaked at **35.4 %** of a 64-sample block; it is now
  **0.06 %**, which is a block doing nothing at all.
- The reported latency is no longer declared as an integer. It never was one: 7.25 samples
  at 2x and 16.93 at 4x.

### Changed — the same work, far cheaper
- **The drive costs 78 % less at 4x**: 237.59 → **51.88 ns per sample**. Thirty-two
  instances of the plugin went from 37.2 % of one core to **7.8 %**.
- **The oversampler was rewritten as overlap-save.** A circular delay line writes the
  newest sample and immediately reads a window that overlaps that store, so every sample
  paid a store-to-load stall. Same terms, same kernel, same SIMD, 4.3x faster.
- **A 31-tap half-band kernel replaced the dense one**, which is both better and cheaper:
  in a half-band every other tap is exactly zero, so one polyphase phase collapses to a
  pure delay.
- **The inner stage of the cascade got its own shorter filter** (11 taps). Going from 2x
  to 4x the signal is already band-limited, so the transition band is enormous. It scores
  better than using the long filter everywhere, and costs less.
- **The audio loop and the drive path dispatch by CPU at load time**, using 256-bit vectors
  and FMA where available and plain SSE3 where not. The fallback path is verified under
  emulation across eight CPU models that cannot run the fast one.
- **A shared coefficient grid**, filled once per process on a background thread at lowest
  priority, lets a moving knob interpolate instead of running the filter designer. The
  exact design is restored as soon as the knob stops, so what you hear at rest is always
  the exact filter, never the approximation.

### Added
- **The interface reports gestures to the host (`ui:touch`).** Without it a host cannot
  tell a user's knob move from an automation write, so automation in touch mode and MIDI
  learn did not behave.
- Nine automated gates run on `make test`, each with a negative leg that must fail, and
  `make install` validates the installed bundle before finishing.

### Notes
- The plugin's flat response is deliberately **faithful to the analogue prototype**, which
  is not perfectly flat: about +0.144 dB in the centre, with a gentle fall at the very top.
  That is the unit being modelled, not an error in the port.
- Address, undefined-behaviour and thread sanitizers all run clean, each against a control
  that must trip.

LV2 `minorVersion` 6 → 8.

## [1.2.0] — 2026-07-18

An interface release. **The sound is untouched** — the DSP is byte-for-byte the same
code as 1.1.0 (verified: `coeffs.h`/`matched.h`/`lv2.c`/`ports.h` identical, 0 lines
changed), so 1.2.0 renders identically.

### Changed — the interface
- Panel rebuilt in the 0C family visual language (floating chassis, shared knob spec,
  bevels, glows, 0.2 s engage/bypass fades).
- **The title is now the bypass**; the Power button is gone from the switch bank,
  leaving `LF HF EQ BUMP` wider.
- **The settings window opens from the DRIVE label** (the title took the bypass job).
- The oversampling setting is visible without opening the window: the DRIVE label
  reads `DRIVE` / `DRIVE 2x` / `DRIVE 4x`. It costs latency, so it must not hide.
- **The DRIVE label is coloured like the control it is**: copper idle, flat white
  while its window is open (the same grammar as title=bypass). It is redrawn ON TOP
  of the window's dim, or it washes out to grey and loses the link to what you clicked.

### Fixed
- **Letter spacing no longer breaks up words.** Cairo rounds every glyph advance to a
  whole pixel: measured on "DRIVE" at 11px the true advances are D=5.192 R=5.071
  I=2.464 V=5.181 E=4.785, and hinting crushes them to 6,6,3,5,5 — inflating D by
  +0.81px and R by +0.93 while SHRINKING V by 0.18. That opened a 2.33px D-R gap vs
  2.00px for I-V ⇒ it read "D RIVE". `CAIRO_HINT_METRICS_OFF` in `zc_setface()`:
  fractional advances, spread halved (0.33 → 0.17px). Affects ALL panel text, and it
  matters more the further the window is resized from 1:1, since the panel scales.
- An open plugin window no longer eats a CPU core: static layers cached and blitted,
  live pass draws only what moved, repaints capped at 30 fps. **~19.9 ms → ~0.3 ms
  per frame (~64×)**.
- Knob skirts cached at device resolution (they were blurry on HiDPI).
- `make install` is atomic; it rewrote the `.so` in place and could crash a host.

LV2 `minorVersion` 4 → 6.

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
- **1.1.0 → 1.2.0:** <https://github.com/NiLace/Dynamo0C/compare/v1.1.0...v1.2.0>
- **1.2.0 → 1.3.0:** <https://github.com/NiLace/Dynamo0C/compare/v1.2.0...v1.3.0>

[1.3.0]: https://github.com/NiLace/Dynamo0C/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/NiLace/Dynamo0C/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/NiLace/Dynamo0C/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/NiLace/Dynamo0C/releases/tag/v1.0.0
