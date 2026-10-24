/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* chain.h -- the equaliser chain (HP, four bands, LP) and how a control reaches it in real time: the controls, the
 * full and the reuse rebuild, the per-block ramp between designs, the round-robin refresh, the coefficient grid used
 * while a knob moves, and the exact refinement once it stops. Header-only. */
#ifndef ZC_CHAIN_H
#define ZC_CHAIN_H

#include <string.h>

#include "section.h"
#include "grid.h"      /* the coefficient grid (it builds on section.h) */

/* ----------------------------------------------------------------- full chain
 * The EQ is analog-matched (matched-Z + FIR correction): it reproduces the analog magnitude near
 * Nyquist WITHOUT cramping, so the chain runs at the base sample rate -- one biquad-style section per
 * band, no oversampling, no latency. (Aliasing only matters for the non-linear DRIVE, which has its
 * own oversampling in drive.h; the EQ is linear and needs none.) */
#define ZC_MAX_SEC 6      /* HP + 4 bands + LP */

typedef struct {
  ZcSection sec[ZC_MAX_SEC];
  int    nsec;
  double fs;                 /* sample rate the sections were designed at */
} ZcChain;

static inline void zc_chain_reset_state(ZcChain *c) {
  for (int i = 0; i < c->nsec; i++) zc_section_reset(&c->sec[i]);
}

/* runs the cascade of sections over one sample */
static inline double zc_chain_tick(ZcChain *c, double x) {
  for (int i = 0; i < c->nsec; i++) x = zc_section_tick(&c->sec[i], x);
  return x;
}

/* processes a block: the IIR cascade, sample by sample, at the base rate */
static inline void zc_chain_process(ZcChain *c, const float *in, float *out, int n) {
  for (int i = 0; i < n; i++) out[i] = (float)zc_chain_tick(c, in[i]);
}

/* Both channels, INTERLEAVED. The IIR recurrence is a serial dependency (y needs z[0], z[0] needs
   y), so one channel alone leaves the machine latency-bound. L and R are independent chains, but
   run back to back they are a whole block apart -- far beyond the reorder buffer -- so the hardware
   never gets to overlap them. Ticking both per sample puts the two chains side by side.
   Bit-identical: each chain performs the same operations in the same order as before. */
static inline void zc_chain_process2(ZcChain *cl, ZcChain *cr,
                                     const float *inl, const float *inr,
                                     float *outl, float *outr, int n) {
  for (int i = 0; i < n; i++) {
    outl[i] = (float)zc_chain_tick(cl, inl[i]);
    outr[i] = (float)zc_chain_tick(cr, inr[i]);
  }
}

/* ----------------------------------------------------------------- controls -> chain */
/* State of the controls the chain needs to build itself.
 * Band indices: 0=LOW, 1=LO-MID, 2=HI-MID, 3=HIGH. Only the outer ones (0 and 3)
 * can switch between bell and shelf; the two central ones are always bell.
 * All continuous values (freq, gain) are norm 0..1 (gain 0.5 = neutral gain). */
typedef struct {
  int    hp_on;    double hp_freq;
  int    hp_bump;        /* HP: 0 = clean Butterworth / 1 = resonant (BUMP, +1.25 dB about half an octave above the corner) */
  struct { int on; int shelf; double freq, gain; } band[4];
  int    lp_on;    double lp_freq;
  int    lp_resonant;     /* LP: 0 = Q=0.707 (no peak) / 1 = Q=0.99 (with resonant boost) */
} ZcControls;


/* The HP and LP designers (the bands' one, zc_section_build_band, is in section.h): every path that designs a
 * section goes through these, so a slot is designed the same way wherever it is designed. */
static inline void zc_section_build_hp(ZcSection *s, const ZcControls *ct, double fs) {
  double fc = zc_interp(ct->hp_freq, ZC_HP_NORM, ZC_HP_HZ, ZC_ARRLEN(ZC_HP_NORM));
  zc_section_build_filter(s, ZC_HP, fc, ct->hp_bump ? ZC_HP_Q : ZC_HP_Q_FLAT, fs);
}
static inline void zc_section_build_lp(ZcSection *s, const ZcControls *ct, double fs) {
  double fc = zc_interp(ct->lp_freq, ZC_LP_NORM, ZC_LP_HZ, ZC_ARRLEN(ZC_LP_NORM));
  zc_section_build_filter(s, ZC_LP, fc, ct->lp_resonant ? ZC_LP_Q_RESONANT : ZC_LP_Q_CLEAN, fs);
}

/* fills c->sec[] and c->nsec from the controls (at c->fs). Does NOT touch the z[] state. */
static inline void zc_chain_fill(ZcChain *c, const ZcControls *ct) {
  double fs = c->fs; c->nsec = 0;
  if (ct->hp_on) zc_section_build_hp(&c->sec[c->nsec++], ct, fs);
  for (int b = 0; b < 4; b++)
    if (ct->band[b].on)
      zc_section_build_band(&c->sec[c->nsec++], b, ct->band[b].shelf, ct->band[b].freq, ct->band[b].gain, fs);
  if (ct->lp_on) zc_section_build_lp(&c->sec[c->nsec++], ct, fs);
}

/* builds the chain FROM SCRATCH (structure change): fills + RESETS state. */
static inline void zc_chain_build(ZcChain *c, const ZcControls *ct, double base_fs) {
  c->fs = base_fs;
  zc_chain_fill(c, ct);
  zc_chain_reset_state(c);
}

/* Copies the DESIGN (coefficients and degrees) from one chain to another, and deliberately NOT
 * the z[] state.
 *
 * zc_chain_fill is a pure function of (ct, fs) and both channels are always handed the SAME ct, so
 * designing twice produces bit-identical coefficients. Designing once and copying therefore halves
 * the cost for exactly zero difference in output, and that cost is not small: a full chain design
 * takes about 0.7 ms on an i7-8700K, over half a 64-sample block's budget at 48 kHz.
 *
 * The state must NOT come along. z[] is the filter's memory of the audio that channel has heard,
 * so copying it would inject the left channel's history into the right one. */
static inline void zc_chain_copy_design(ZcChain *dst, const ZcChain *src) {
  dst->nsec = src->nsec;
  dst->fs   = src->fs;
  for (int i = 0; i < src->nsec; i++) zc_section_copy_design(&dst->sec[i], &src->sec[i]);
}

/* Processes a block while RAMPING the coefficients from one design to another.
 *
 * THE PROBLEM. The designer runs once per block, so a moving knob makes the response advance in
 * steps whose size depends on the host's buffer. Against the finest render the plugin can produce
 * that is -53 dB of residual at a 64-sample block, -35.6 dB at 512 and -29.5 dB at 1024, scaling
 * at about 6 dB per doubling exactly as a step-size argument predicts.
 *
 * Redesigning every 16 to 32 samples would fix it and is not affordable: one section design costs
 * about 0.11 ms on an i7-8700K, so redesigning one section every 16 samples at 48 kHz would be
 * about a third of a core per moving knob.
 *
 * Instead this uses the two designs it ALREADY has -- the previous one and this block's -- and
 * walks between them in a few sub-steps, delivering the same total change in K smaller steps. No
 * extra designs, and the cost is a handful of interpolations per sub-step, paid only on blocks
 * where a control actually moved.
 *
 * Note that this INTERPOLATES a[], the denominator. In general nothing about the magnitude of an
 * interpolated filter guarantees its poles stay inside the unit circle; it is defensible here only
 * because the two endpoints are one block apart, so the path between them is short. Since that is
 * an argument rather than evidence, a development test (ramp_stability.c) checks the pole radius of every
 * interpolated set produced by a fast sweep of every control. */
static inline void zc_chain_process_ramp(ZcChain *c, const ZcChain *from, const ZcChain *to,
                                         const float *in, float *out, int n, int steps) {
  if (steps < 1) steps = 1;
  int done = 0;
  for (int s = 0; s < steps && done < n; s++) {
    const double t = (double)(s + 1) / (double)steps;
    for (int i = 0; i < c->nsec; i++) {
      if (s == steps - 1) {
        /* the last sub-step lands ON the target design: from + 1*(to - from) need not round back
         * to `to`, and the chain keeps whatever this step leaves until the next knob move */
        zc_section_copy_design(&c->sec[i], &to->sec[i]);
        continue;
      }
      for (int k = 0; k <= ZC_MAX_N; k++) {
        c->sec[i].b[k] = from->sec[i].b[k] + t * (to->sec[i].b[k] - from->sec[i].b[k]);
        c->sec[i].a[k] = from->sec[i].a[k] + t * (to->sec[i].a[k] - from->sec[i].a[k]);
      }
      c->sec[i].nb = to->sec[i].nb;
      c->sec[i].na = to->sec[i].na;
    }
    const int upto = (int)((long)n * (s + 1) / steps);
    zc_chain_process(c, in + done, out + done, upto - done);
    done = upto;
  }
}

/* Does band b need redesigning, given the previous controls? */
static inline int zc_band_changed(const ZcControls *a, const ZcControls *b, int i) {
  return a->band[i].freq != b->band[i].freq || a->band[i].gain != b->band[i].gain
      || a->band[i].shelf != b->band[i].shelf || a->band[i].on != b->band[i].on;
}

/* Index of a logical slot inside a chain built from `ct`, or -1 if that slot is absent.
   slot: 0 = HP, 1..4 = bands 0..3, 5 = LP. */
static inline int zc_slot_index(const ZcControls *ct, int slot) {
  int s = 0;
  if (slot == 0) return ct->hp_on ? 0 : -1;
  if (ct->hp_on) s++;
  for (int b = 0; b < 4; b++) {
    if (slot == 1 + b) return ct->band[b].on ? s : -1;
    if (ct->band[b].on) s++;
  }
  if (slot == 5) return ct->lp_on ? s : -1;
  return -1;
}

/* Rebuild after a STRUCTURE change, reusing the design of every section whose own controls did
   not move. A toggle (HP on/off, shelf/bell, EQ engage, BUMP, LP resonant) shifts the section
   indices, which is why the plain rebuild throws all six designs away -- but only one of them
   actually changed. This walks the new structure and the old one in parallel and copies the
   coefficients across when the slot survived unchanged.
   Exactly equivalent, not an approximation: zc_section_build_* is a pure function of its inputs,
   so a copied design is the design that would have been computed. State is NOT copied -- the
   caller still zeroes it, exactly as zc_chain_build did.
   `approx` is the old chain's mask of INTERPOLATED sections (bit = old physical index): those are
   not the design their controls would compute, so they are redesigned instead of copied. Copying
   one would keep the grid's interpolation error for good, because the mask that would have
   refined it is indexed by the OLD section order and is meaningless after the rebuild. */
static inline void zc_chain_rebuild_reuse(ZcChain *c, const ZcChain *old, double base_fs,
                                          const ZcControls *ct, const ZcControls *prev,
                                          unsigned approx, const ZcGrid *grid, unsigned *approx_out) {
  /* base_fs is taken as an argument and set here, exactly like zc_chain_build, rather than read
   * from c->fs. Reading it from the chain would assume the chain had already been built once; if
   * it has not, fs is 0, every design fails and every section falls back to pass-through, which
   * makes the plugin silent. */
  c->fs = base_fs;
  double fs = base_fs;
  /* ZEROED, and not as defensive tidiness: zc_section_keep_or_bypass() READS the section it is
   * handed. When a design fails it inspects (nb == 0 && b[0] == 0.0) to choose between keeping the
   * previous design and falling back to pass-through, so the builders are not pure writers and
   * uninitialised stack here would feed garbage into the coefficients. */
  ZcSection out[ZC_MAX_SEC] = {0};
  int n = 0;
  for (int slot = 0; slot < 6; slot++) {
    int ns = zc_slot_index(ct, slot);
    if (ns < 0) continue;                       /* absent in the new structure */
    int os = zc_slot_index(prev, slot);
    int same = 0;
    if (os >= 0 && os < old->nsec && !(approx & (1u << os))) {
      if (slot == 0)      same = (ct->hp_freq == prev->hp_freq && ct->hp_bump == prev->hp_bump);
      else if (slot == 5) same = (ct->lp_freq == prev->lp_freq && ct->lp_resonant == prev->lp_resonant);
      else                same = !zc_band_changed(ct, prev, slot - 1);
    }
    if (same)           zc_section_copy_design(&out[n], &old->sec[os]);   /* reuse: the coefficients, not the state */
    else if (slot == 0) zc_section_build_hp(&out[n], ct, fs);
    else if (slot == 5) zc_section_build_lp(&out[n], ct, fs);
    else if (grid) {    /* a band to design (EQ engaged, a shelf toggled): interpolated now, made exact one per
                         * block by zc_chain_refine_one, so the switch is not four exact designs in one block */
      const int b = slot - 1, shelf = (b == 0 || b == 3) && ct->band[b].shelf;
      zc_grid_lookup(&grid->band[shelf ? ((b == 3) ? ZC_GB_HISHELF : ZC_GB_LOSHELF) : b],
                     ct->band[b].freq, ct->band[b].gain, &out[n]);
      if (approx_out) *approx_out |= 1u << n;
    }
    else                zc_section_build_band(&out[n], slot - 1, ct->band[slot - 1].shelf,
                                              ct->band[slot - 1].freq, ct->band[slot - 1].gain, fs);
    n++;
  }
  c->nsec = n;
  for (int i = 0; i < n; i++) zc_section_copy_design(&c->sec[i], &out[i]);
  for (int i = 0; i < n; i++) zc_section_reset(&c->sec[i]);
}

/* ---- round-robin refresh: cap the redesign work of ANY single block ----------------------------
 * Redesigning EVERY section whose controls moved, in the block where they moved, produces a peak
 * that does not scale: one section costs about 8 % of a 64-sample block and all four cost 32 %.
 * At 64 samples that budget belongs to the whole project, so a handful of automated instances
 * exhaust it.
 *
 * So at most `budget` sections are redesigned per call, starting where the last call left off, and
 * only what was actually redesigned is marked done, by copying those controls into `prev`. Anything
 * left keeps differing from `prev`, so the caller's memcmp brings us back on the next block and the
 * work drains over a few blocks.
 *
 * THE CURSOR IS LOAD-BEARING. Always starting at section 0 would starve the last sections whenever
 * every band is automated at once: section 0 changes again every block and the budget never reaches
 * the others. Starting where the previous call stopped bounds the wait for every section.
 *
 * WHEN NOTHING IS LEFT PENDING, `prev` is synced WHOLE. ZcControls carries fields that belong to no
 * section -- they change the structure and go through the rebuild path instead -- and if any of
 * them differed, the caller's memcmp would fire on every block forever. Syncing on the way out
 * makes convergence structural rather than dependent on the field list below staying complete.
 *
 * The price is deliberate: a move that touches several bands is applied over several blocks instead
 * of one, which at 64 samples is about 8 ms for six blocks. Each SECTION is still updated
 * atomically, so no section ever runs a mix of two designs; what is briefly inconsistent is the
 * chain as a whole, exactly as if the knobs had been turned a few milliseconds apart.
 *
 * With a coefficient grid available, a moving knob interpolates (0.017 us) instead of designing
 * (116 us). `grid` may be NULL, while the table is still being filled or if it was never built, and
 * then this behaves as if there were none. `approx` accumulates one bit per PHYSICAL section that
 * was set by interpolation, so zc_chain_refine_one below can replace it with the exact design once
 * the knob stops moving: what is heard in a steady state is the exact filter, and the approximation
 * exists only while a control is being turned. */
static inline int zc_chain_update_rr(ZcChain *c, const ZcControls *ct, ZcControls *prev,
                                     int budget, int *cursor, const ZcGrid *grid,
                                     unsigned *approx) {
  double fs = c->fs;
  int slot[6], phys[6], nlog = 0, s = 0;
  if (ct->hp_on) { slot[nlog] = 0; phys[nlog++] = s++; }
  for (int b = 0; b < 4; b++) if (ct->band[b].on) { slot[nlog] = 1 + b; phys[nlog++] = s++; }
  if (ct->lp_on) { slot[nlog] = 5; phys[nlog++] = s++; }
  if (nlog == 0) { *prev = *ct; return 0; }
  if (*cursor >= nlog) *cursor = 0;

  /* `start` is a COPY on purpose. Reading *cursor inside the loop while the body also writes it
   * makes the walk skip slots, and a skipped slot is not counted as pending either, so the
   * function would report "nothing left", the caller would sync prev, and those sections would keep
   * their OLD coefficients indefinitely. a development test (rr_converge.c) is the check for that. */
  const int start = *cursor;
  int done = 0, pending = 0;
  for (int i = 0; i < nlog; i++) {
    int k = (start + i) % nlog, sl = slot[k], si = phys[k];
    int changed;
    if (sl == 0)      changed = (ct->hp_freq != prev->hp_freq || ct->hp_bump != prev->hp_bump);
    else if (sl == 5) changed = (ct->lp_freq != prev->lp_freq || ct->lp_resonant != prev->lp_resonant);
    else              changed = zc_band_changed(ct, prev, sl - 1);
    if (!changed) continue;
    if (done >= budget) { pending++; continue; }

    if (sl == 0) {
      zc_section_build_hp(&c->sec[si], ct, fs);
      prev->hp_freq = ct->hp_freq; prev->hp_bump = ct->hp_bump;
    } else if (sl == 5) {
      zc_section_build_lp(&c->sec[si], ct, fs);
      prev->lp_freq = ct->lp_freq; prev->lp_resonant = ct->lp_resonant;
    } else {
      int b = sl - 1;
      int shelf = (b == 0 || b == 3) && ct->band[b].shelf;
      if (grid) {                                  /* O(1): interpolate, and mark it as approximate */
        int which = shelf ? ((b == 3) ? ZC_GB_HISHELF : ZC_GB_LOSHELF) : b;
        zc_grid_lookup(&grid->band[which], ct->band[b].freq, ct->band[b].gain, &c->sec[si]);
        if (approx) *approx |= 1u << si;
      } else {
        zc_section_build_band(&c->sec[si], b, shelf, ct->band[b].freq, ct->band[b].gain, fs);
      }
      prev->band[b] = ct->band[b];
    }
    done++;
    *cursor = (k + 1) % nlog;
  }
  if (pending == 0) *prev = *ct;              /* structural convergence; see the note above */
  return pending;
}

/* Replace ONE interpolated section with its exact design. Called on blocks where nothing moved, so
 * it never adds to the peak it was introduced to remove: the cost lands where there is room for it.
 * Returns 1 if it refined something. */
static inline int zc_chain_refine_one(ZcChain *c, const ZcControls *ct, unsigned *approx) {
  if (!approx || !*approx) return 0;
  double fs = c->fs;
  int s = 0;
  if (ct->hp_on) s++;
  for (int b = 0; b < 4; b++) {
    if (!ct->band[b].on) continue;
    if (*approx & (1u << s)) {
      zc_section_build_band(&c->sec[s], b, ct->band[b].shelf, ct->band[b].freq, ct->band[b].gain, fs);
      *approx &= ~(1u << s);
      return 1;
    }
    s++;
  }
  *approx = 0;                                   /* nothing matched: do not spin forever */
  return 0;
}

#endif /* ZC_CHAIN_H */
