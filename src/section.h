/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* section.h -- one IIR section of the equaliser (Direct Form II transposed) and its designers: the analogue
 * prototype (analog.h) turned into a digital section by the analog-matched design (matched.h). Header-only. */
#ifndef ZC_SECTION_H
#define ZC_SECTION_H

#include <math.h>
#include <string.h>

#include "analog.h"    /* the analogue prototypes H(s), their tables and the pot laws (shared with the UI) */
#include "matched.h"   /* analog-matched designer (matched-Z + FIR correction) */

/* ----------------------------------------------------------------- sections */


/* One section: digital IIR, Direct Form II transposed. The poles come from the denominator alone
 * (matched-Z of a stable analogue prototype), so it is always stable.
 * The array sizes are derived from the designer's own bound rather than written out by hand. With
 * ZC_MZ_CORR = 5 that gives b[8] / z[7]; building with -DZC_MZ_CORR=6, which matched.h explicitly
 * allows, would overflow both if the sizes were fixed. Derived, it either fits or fails to
 * compile. */
#define ZC_MAX_NB   ZC_MZ_MAX_NB           /* highest numerator degree the designer can emit */
#define ZC_MAX_NA   3                      /* denominator degree: analogue prototype, not NC */
/* zc_section_tick indexes z[0..N-1] with N = max(nb, na), so the state is sized by the LARGER of
 * the two degrees: sized from the numerator alone it would be too short at low correction orders, and the assertion
 * below fails at compile time if it ever is. */
#define ZC_MAX_N    (ZC_MAX_NB > ZC_MAX_NA ? ZC_MAX_NB : ZC_MAX_NA)
_Static_assert(ZC_MAX_N >= ZC_MAX_NA && ZC_MAX_N >= ZC_MAX_NB, "the state must hold the larger of the two degrees");

typedef struct {
  int    nb;                          /* numerator degree (0..ZC_MAX_NB) */
  int    na;                          /* denominator degree (0..ZC_MAX_NA) */
  /* Both arrays run to ZC_MAX_N so the tick can index them without a bound test; everything past
   * the section's own degree is kept at zero by the builders. */
  double b[ZC_MAX_N + 1], a[ZC_MAX_N + 1];     /* digital coeffs, ASCENDING in z^-1 (a[0]=1) */
  double z[ZC_MAX_N];                 /* state, size max(nb, na) */
} ZcSection;

static inline void zc_section_reset(ZcSection *s) {
  for (int i = 0; i < ZC_MAX_N; i++) s->z[i] = 0.0;
}

/* Branch-free Direct Form II transposed step.
 *
 * b[] and a[] are ZERO-PADDED past their own degree by the builders, so the loop can run a fixed
 * length with no per-tap test for "coefficient or 0.0". That is exact, not an approximation: the
 * padded taps contribute 0.0*x - 0.0*y, and adding an exact zero changes nothing in IEEE
 * arithmetic. The tail entries of z[] therefore hold zeros and keep holding them, so a section
 * whose real degree is below the maximum behaves exactly as its own degree dictates.
 * The fixed length is also the FAST form, measured: the x86-64-v3 clone vectorises this loop across the taps, so
 * the zero taps ride in lanes that cost nothing. Stopping the feedback at ZC_MAX_NA breaks that vectorisation and made
 * the plugin 24 % slower with the EQ alone and 4 % with the drive at 4x (paired, 21 rounds, i7-8700K), and not
 * bit-identical either (the split loop contracts differently into FMA). Do not "optimise" it. */
static inline double zc_section_tick(ZcSection *s, double x) {
  double y = s->b[0] * x + s->z[0];
  for (int i = 0; i < ZC_MAX_N - 1; i++)
    s->z[i] = s->b[i + 1] * x - s->a[i + 1] * y + s->z[i + 1];
  s->z[ZC_MAX_N - 1] = s->b[ZC_MAX_N] * x - s->a[ZC_MAX_N] * y;
  return y;
}


/* Builds a section: analog -> matched-Z + FIR correction (at fs) -> fills ZcSection. */
/* NOTE: these builders only (re)compute b/a/nb/na; they do NOT touch the z[] state, so they serve to
 * update coefficients live (when moving a control) without clicks. The state reset is done by
 * zc_chain_build when the STRUCTURE of the chain changes. */
/* Stores a fresh design, ZERO-PADDING both arrays past their degree. The padding is what lets
 * zc_section_tick run branch-free: every tap it reads is either a real coefficient or an exact 0. */
static inline void zc_section_store(ZcSection *sec, const double *b, int nb,
                                    const double *a, int na) {
  for (int k = 0; k <= ZC_MAX_N; k++) sec->b[k] = (k <= nb) ? b[k] : 0.0;
  for (int k = 0; k <= ZC_MAX_N; k++) sec->a[k] = (k <= na) ? a[k] : 0.0;
  sec->nb = nb; sec->na = na;
}

/* If the designer reports failure, the section KEEPS the coefficients it already had -- a stale
 * but valid filter is always better than a NaN, which would poison z[] forever. A section that has
 * never been designed falls back to unity so it is at least transparent rather than silent. */
static inline void zc_section_keep_or_bypass(ZcSection *sec, int ok) {
  if (ok) return;
  if (sec->nb == 0 && sec->b[0] == 0.0) {       /* never successfully designed: pass through */
    sec->nb = 0; sec->na = 0; sec->b[0] = 1.0; sec->a[0] = 1.0;
  }
}

static inline int zc_section_build_bellshelf(ZcSection *sec, ZcKind kind,
        const float *ftap, const float *wtap, const double *cap,
        double nf, double ng, double fs) {
  double b[4], a[4]; int nb, na;
  double tb[ZC_MAX_N + 1], ta[ZC_MAX_N + 1]; int tnb, tna;
  zc_bellshelf_analog(kind, ftap, wtap, cap, nf, ng, b, &nb, a, &na);
  int ok = zc_matched_design(b, nb, a, na, fs, tb, &tnb, ta, &tna);
  if (ok) zc_section_store(sec, tb, tnb, ta, tna);
  zc_section_keep_or_bypass(sec, ok);
  return ok;
}
static inline int zc_section_build_filter(ZcSection *sec, ZcKind kind,
        double fc, double Q, double fs) {
  double b[4], a[4]; int nb, na;
  double tb[ZC_MAX_N + 1], ta[ZC_MAX_N + 1]; int tnb, tna;
  if (kind == ZC_HP) zc_hp_analog(fc, Q, b, &nb, a, &na);
  else                  zc_lp_analog(fc, Q, b, &nb, a, &na);
  int ok = zc_matched_design(b, nb, a, na, fs, tb, &tnb, ta, &tna);
  if (ok) zc_section_store(sec, tb, tnb, ta, tna);
  zc_section_keep_or_bypass(sec, ok);
  return ok;
}

/* The band designer: every path that designs a band (the full fill, the reuse rebuild, the round-robin refresh,
 * the refinement and the grid's filler) goes through it, so a band is designed the same way wherever it is designed. shelf is honoured only for the Low (b = 0) and High (b = 3) bands. Each returns
 * the designer's verdict (0 = it failed and the section kept its previous design, or passes through). */
static inline int zc_section_build_band(ZcSection *s, int b, int shelf, double freq, double gain, double fs) {
  if ((b == 0 || b == 3) && shelf)
    return zc_section_build_bellshelf(s, (b == 3) ? ZC_HISHELF : ZC_LOSHELF,
                               (b == 3) ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF,
                               (b == 3) ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF,
                               (b == 3) ? ZC_CAP_HI_SHELF  : ZC_CAP_LO_SHELF, freq, gain, fs);
  return zc_section_build_bellshelf(s, ZC_BELL, zc_ftap_band(b), zc_wtap_band(b), zc_cap_band(b), freq, gain, fs);
}
/* copies a section's design (degrees and coefficients), never its state */
static inline void zc_section_copy_design(ZcSection *dst, const ZcSection *src) {
  dst->nb = src->nb; dst->na = src->na;
  for (int k = 0; k <= ZC_MAX_NB; k++) dst->b[k] = src->b[k];
  for (int k = 0; k <= ZC_MAX_NA; k++) dst->a[k] = src->a[k];
}

#endif /* ZC_SECTION_H */
