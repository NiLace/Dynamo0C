/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* analog.h -- the analogue prototypes H(s) of the EQ's sections, the tables behind the knobs and the pot laws.
 * Pure and header-only (C11, <math.h> and <complex.h>): the DSP designs its digital sections from these (dsp.h), and
 * the UI computes the read-out of a knob from the same equations (zc_band_readout below), so the number on screen is
 * the response the plugin actually builds, not an interpolation of the printed scale. */
#ifndef ZC_ANALOG_H
#define ZC_ANALOG_H

#include <complex.h>
#include <math.h>

/* coeffs.h is a GENERATED file and trips -Wunused-parameter twice: zc_hishelf_coeffs takes Ct and
 * zc_loshelf_coeffs takes Cf, and neither topology uses that capacitor. That is by design, not a
 * defect -- the three coefficient functions share ONE uniform signature so zc_bellshelf_analog
 * below can call any of them through the same line, and each uses the cap it needs.
 *
 * The suppression lives here rather than inside coeffs.h because a regeneration would drop it, and
 * its scope is just this include: warnings stay switched on for the hand-written code, which is
 * what lets these sources build with -Wextra -Werror.
 *
 * Note that the laboratory's provenance.c and the laboratory's eqdata.c include coeffs.h directly and are NOT
 * covered by this; they need the same wrapper if they are ever built with -Wextra. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "coeffs.h"
#pragma GCC diagnostic pop

/* number of elements in a static array (so table lengths aren't hard-coded by hand) */
#define ZC_ARRLEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* high-pass Q per the BUMP button: 0.707 = flat Butterworth (no peak); ZC_HP_Q (~1.0) is a
 * deliberate VOICING choice — a gentle resonant lift (~+1.5 dB) just above the corner. It is
 * hand-set and NOT taken from the schematic: the netlist has no such resonance. Same for
 * ZC_LP_Q_RESONANT on the low-pass. (The README says the same thing to users, under BUMP.) */
#define ZC_HP_Q_FLAT 0.70710678118654752

/* ----------------------------------------------------------------- interpolation */

/* uniform LUT in norm 0..1 (tapers norm->F / norm->W), ZC_TAP_N points. */
static inline double zc_lut(const float *tab, double norm) {
  if (norm <= 0.0) return tab[0];
  if (norm >= 1.0) return tab[ZC_TAP_N - 1];
  double x = norm * (ZC_TAP_N - 1);
  int i = (int)x; double fr = x - i;
  return tab[i] * (1.0 - fr) + tab[i + 1] * fr;
}

/* linear interp over an irregular table (norm->Hz for HP/LP). */
static inline double zc_interp(double xq, const float *xs, const float *ys, int n) {
  if (xq <= xs[0]) return ys[0];
  if (xq >= xs[n - 1]) return ys[n - 1];
  int i = 0; while (i < n - 1 && xs[i + 1] < xq) i++;
  double fr = (xq - xs[i]) / (xs[i + 1] - xs[i]);
  return ys[i] * (1.0 - fr) + ys[i + 1] * fr;
}

typedef enum { ZC_BELL, ZC_HISHELF, ZC_LOSHELF, ZC_HP, ZC_LP } ZcKind;

/* gain divider: the taper position W in [0,1] is split into two resistances
 * Rt, Rb of the model (W=0.5 -> Rt=Rb -> neutral gain; +1 avoids division by zero) */
static inline void zc_Rt_Rb(double W, double *Rt, double *Rb) {
  *Rt = 10000.0 * W + 1.0; *Rb = 10000.0 * (1.0 - W) + 1.0;
}

/* Builds the ANALOG coeffs (b,a descending) of a bell/shelf section
 * from the norm controls (nf, ng) and their tapers/caps.
 * The ftap/wtap tables are POT-WIPER fractions, not magic numbers: they are design tables derived
 * from the schematic that make each band's peak sweep log-uniformly (F ~= exp(-5.55*nf+c), one
 * shared log-pot law; the caps differ per band). They also carry a ~1 % circuit-compensation term,
 * which is why the closed-form pot law is documented here but NOT substituted for the tables --
 * substituting it would move the audio. */
static inline void zc_bellshelf_analog(ZcKind kind,
        const float *ftap, const float *wtap, const double *cap,
        double nf, double ng, double *b, int *nb, double *a, int *na) {
  double F = zc_lut(ftap, nf), W = zc_lut(wtap, ng);
  double Rt, Rb; zc_Rt_Rb(W, &Rt, &Rb); double Rf = 50000.0 * F + 1.0;
  double Cf = cap[0], Ct = cap[1];
  if (kind == ZC_HISHELF)      { zc_hishelf_coeffs(Rt, Rb, Rf, Cf, Ct, b, a); *nb = 3; *na = 3; }
  else if (kind == ZC_LOSHELF) { zc_loshelf_coeffs(Rt, Rb, Rf, Cf, Ct, b, a); *nb = 2; *na = 3; }
  else                            { zc_bell_coeffs   (Rt, Rb, Rf, Cf, Ct, b, a); *nb = 3; *na = 4; }
}

/* HP/LP Sallen-Key 2nd order, normalized analog coeffs (b,a descending). */
static inline void zc_hp_analog(double fc, double Q, double *b, int *nb, double *a, int *na) {
  double w0 = 2.0 * M_PI * fc;
  b[0] = 1.0; b[1] = 0.0; b[2] = 0.0;                 /* s^2 */
  a[0] = 1.0; a[1] = w0 / Q; a[2] = w0 * w0; *nb = 3; *na = 3;
}
static inline void zc_lp_analog(double fc, double Q, double *b, int *nb, double *a, int *na) {
  double w0 = 2.0 * M_PI * fc;
  b[0] = 0.0; b[1] = 0.0; b[2] = w0 * w0;             /* unity at DC */
  a[0] = 1.0; a[1] = w0 / Q; a[2] = w0 * w0; *nb = 3; *na = 3;
}

static inline const float *zc_ftap_band(int b) {
  switch (b) { case 0: return ZC_FTAP_LOW; case 1: return ZC_FTAP_LOMID;
               case 2: return ZC_FTAP_HIMID; default: return ZC_FTAP_HI; }
}
static inline const float *zc_wtap_band(int b) {
  switch (b) { case 0: return ZC_WTAP_LOW; case 1: return ZC_WTAP_LOMID;
               case 2: return ZC_WTAP_HIMID; default: return ZC_WTAP_HI; }
}
static inline const double *zc_cap_band(int b) {
  switch (b) { case 0: return ZC_CAP_LOW; case 1: return ZC_CAP_LOMID;
               case 2: return ZC_CAP_HIMID; default: return ZC_CAP_HI; }
}

/* ---- the read-out: what a band knob really does ---------------------------------------------------------------------
 * |H(j 2 pi f)| in dB of a section given as analogue coefficients (descending powers, nb/na coefficients). */
static inline double zc_analog_db(const double *b, int nb, const double *a, int na, double f) {
  double complex s = I * 2.0 * M_PI * f, num = 0, den = 0;
  for (int k = 0; k < nb; k++) num = num * s + b[k];
  for (int k = 0; k < na; k++) den = den * s + a[k];
  return 20.0 * log10(cabs(num / den));
}
/* The band's response sampled over 20 Hz .. 20 kHz (ZC_RO_N log-spaced points). */
#define ZC_RO_N 241
static inline void zc_band_response(int b, int shelf, double nf, double ng, double *db, double *f) {
  double bb[4], aa[4]; int nb, na;
  const int sh = (b == 0 || b == 3) && shelf;
  if (sh) zc_bellshelf_analog(b == 3 ? ZC_HISHELF : ZC_LOSHELF, b == 3 ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF,
                              b == 3 ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF, b == 3 ? ZC_CAP_HI_SHELF : ZC_CAP_LO_SHELF,
                              nf, ng, bb, &nb, aa, &na);
  else    zc_bellshelf_analog(ZC_BELL, zc_ftap_band(b), zc_wtap_band(b), zc_cap_band(b), nf, ng, bb, &nb, aa, &na);
  for (int i = 0; i < ZC_RO_N; i++) {
    f[i] = 20.0 * pow(1000.0, (double)i / (ZC_RO_N - 1));
    db[i] = zc_analog_db(bb, nb, aa, na, f[i]);
  }
}
/* What the band's knobs read, from the prototype the plugin builds:
 *   gain_db  the largest deviation of the response from 0 dB within 20 Hz .. 20 kHz, signed (a bell's peak or dip, a
 *            shelf's plateau);
 *   freq_hz  where the band acts, taken at full boost so it does not depend on the gain knob: a bell's peak, a shelf's
 *            half-gain point. */
static inline void zc_band_readout(int b, int shelf, double nf, double ng, double *gain_db, double *freq_hz) {
  double db[ZC_RO_N], f[ZC_RO_N];
  if (gain_db) {
    zc_band_response(b, shelf, nf, ng, db, f);
    int ip = 0; for (int i = 1; i < ZC_RO_N; i++) if (fabs(db[i]) > fabs(db[ip])) ip = i;
    *gain_db = db[ip];
  }
  if (freq_hz) {
    zc_band_response(b, shelf, nf, 1.0, db, f);
    int ip = 0; for (int i = 1; i < ZC_RO_N; i++) if (db[i] > db[ip]) ip = i;
    const int sh = (b == 0 || b == 3) && shelf;
    if (!sh) { *freq_hz = f[ip]; return; }
    /* a shelf: the point where the response crosses half its plateau, walking from the plateau inwards */
    const double half = db[ip] / 2.0;
    int i = ip;
    if (b == 3) { while (i > 0 && db[i] > half) i--; }
    else        { while (i < ZC_RO_N - 1 && db[i] > half) i++; }
    *freq_hz = f[i];
  }
}

#endif /* ZC_ANALOG_H */
