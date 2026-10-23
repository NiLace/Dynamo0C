/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* matched.h -- analog-matched designer (matched-Z + HF-weighted FIR correction) for Dynamo.
 * Replaces the EQ's bilinear+oversampling pair with a closed form that needs NO oversampling:
 *   1. roots of the analogue H(s)=B(s)/A(s)  (Durand-Kerner)
 *   2. matched-Z: digital pole/zero = exp(s/fs)  -> denominator D(z) of degree nd, numerator
 *      N0(z) of degree nz
 *   3. exact DC gain scaling
 *   4. FIR correction C(z) of degree ZC_MZ_CORR (=5): |C|^2 fitted by TREBLE-WEIGHTED least
 *      squares to |Han|^2/|Hdig|^2, then factored to minimum phase via the roots of the
 *      autocorrelation polynomial
 *   -> H(z) = N0(z)*C(z) / D(z) : numerator of degree (nz+ZC_MZ_CORR), denominator of degree nd.
 * The poles come from D(z) ALONE (matched-Z of a stable analogue prototype) => ALWAYS STABLE.
 * Only <math.h> and <complex.h> (C11: CMPLX). Stateless, no allocations.  */
#ifndef ZC_MATCHED_H
#define ZC_MATCHED_H
#include <math.h>
#include <complex.h>

#ifndef ZC_MZ_CORR
#define ZC_MZ_CORR   5      /* Order of the FIR correction (overridable with -DZC_MZ_CORR=N).
                             * 5 and 6 are the usable plateau: order 5 halves the worst-case error
                             * of order 3 (0.87 -> 0.45 dB on the high shelf) and holds at 44.1,
                             * 48 and 96 kHz, while from order 8 up the least-squares system turns
                             * ill conditioned and the fit degrades. The correction contributes
                             * ZEROS only, so stability does not depend on it: the poles come from
                             * matched-Z of a stable analogue prototype. */
#endif
#ifndef ZC_MZ_WPOW
#define ZC_MZ_WPOW   14.0   /* weight ~ (f/fnyq)^WPOW in the correction's least squares (overridable) */
#endif
/* Array headroom. These arrays serve TWO independent needs and the bound has to cover the larger:
 *   1. the autocorrelation polynomial of |C|^2 has degree 2*NC, whose Durand-Kerner roots need
 *      room for 2*NC+1, and the final numerator has degree nz(<=2)+NC;
 *   2. the ANALOGUE poles and zeros, whose count does not depend on NC at all -- a BELL prototype
 *      has na_s = 4, i.e. np = 3 poles, at every NC.
 * Deriving the bound from NC alone is wrong at both ends: too small for NC >= 6, and too small for
 * NC = 0, where it would leave zc_mz_roots writing past the end of the array. */
#define ZC_MZ_ANALOG_MAX 4          /* longest analogue coefficient array (BELL denominator) */
#define ZC_MZ_MAXNUM ((2 * ZC_MZ_CORR + 2) > (ZC_MZ_ANALOG_MAX + 2) \
                      ? (2 * ZC_MZ_CORR + 2) : (ZC_MZ_ANALOG_MAX + 2))
/* Degree of the digital numerator this designer can emit: nz (<=2) plus the correction order.
 * dsp.h sizes ZcSection.b[]/z[] from this, and asserts the two stay in step. */
#define ZC_MZ_MAX_NB (2 + ZC_MZ_CORR)

/* Roots of a real polynomial (coeffs high->low, degree n) by Durand-Kerner.
 * Returns 1 if the iteration converged, 0 if it ran out of iterations. The caller must check: a
 * non-converged root set yields a filter that is not the one that was asked for, and nothing
 * downstream can detect that. */
static inline int zc_mz_roots(const double *p, int n, double _Complex *r) {
  if (n <= 0) return 1;
  if (n == 1) { r[0] = -p[1] / p[0]; return 1; }
  if (n == 2) {                                  /* closed form: robust against a double root (disc=0) */
    double _Complex a1 = p[1] / p[0], a2 = p[2] / p[0];
    double _Complex disc = csqrt(a1 * a1 - 4.0 * a2);
    r[0] = (-a1 + disc) * 0.5; r[1] = (-a1 - disc) * 0.5; return 1;
  }
  double _Complex a[ZC_MZ_MAXNUM + 1];
  for (int k = 0; k <= n; k++) a[k] = p[k] / p[0];
  double _Complex seed = 0.4 + 0.9 * _Complex_I;
  for (int i = 0; i < n; i++) { double _Complex z = 1; for (int j = 0; j < i; j++) z *= seed; r[i] = z; }
  for (int it = 0; it < 8000; it++) {
    double mx = 0;
    for (int i = 0; i < n; i++) {
      double _Complex num = a[0];
      for (int k = 1; k <= n; k++) num = num * r[i] + a[k];
      double _Complex d = 1;
      for (int j = 0; j < n; j++) if (j != i) d *= (r[i] - r[j]);
      double _Complex dl = num / d; r[i] -= dl;
      /* RELATIVE step size, and it has to stay relative. These polynomials come from analogue
       * prototypes whose coefficients carry w0^2 terms of order 1e9, so the roots themselves have
       * magnitude 1e4-1e5, and an absolute threshold of 1e-15 sits far below the last bit they can
       * hold: the loop would never report convergence and would run all 8000 iterations on every
       * design. Scaling by the root's own magnitude asks the intended question, which is whether
       * this root has stopped moving. */
      double m = cabs(dl) / (1.0 + cabs(r[i])); if (m > mx) mx = m;
    }
    if (mx < 1e-14) return 1;
  }
  return 0;                    /* 8000 iterations without converging */
}
/* monic polynomial (ascending coeffs: c[0]+c[1]z^-1+...) built from nr roots. */
static inline void zc_mz_poly(const double _Complex *r, int nr, double *c) {
  double _Complex cc[ZC_MZ_MAXNUM + 1];
  cc[0] = 1; for (int i = 1; i <= nr; i++) cc[i] = 0;
  for (int i = 0; i < nr; i++)
    for (int k = i + 1; k > 0; k--) cc[k] = cc[k] - r[i] * cc[k - 1];
  for (int k = 0; k <= nr; k++) c[k] = creal(cc[k]);
}
/* |Han(jw)| in dB of the ANALOGUE prototype b/a (descending coeffs in s), evaluated at the digital
 * frequency w: s = j*w*fs, with w in rad/sample. */
static inline double zc_mz_andb(const double *b_s, int nb_s, const double *a_s, int na_s,
                                double fs, double w) {
  double _Complex s = CMPLX(0.0, 1.0) * w * fs, nu = b_s[0], de = a_s[0];
  for (int k = 1; k < nb_s; k++) nu = nu * s + b_s[k];
  for (int k = 1; k < na_s; k++) de = de * s + a_s[k];
  return 20.0 * log10(cabs(nu / de));
}

/* |H(e^jw)| in dB of a digital response num[0..nb]/den[0..na] (ascending coeffs). */
static inline double zc_mz_digdb(const double *B, int nb, const double *A, int na, double w) {
  double _Complex z = cexp(-_Complex_I * w), n = 0, d = 0, zp = 1;
  for (int k = 0; k <= nb; k++) { n += B[k] * zp; zp *= z; }
  zp = 1; for (int k = 0; k <= na; k++) { d += A[k] * zp; zp *= z; }
  return 20.0 * log10(cabs(n / d));
}

/* Designs the matched digital filter from the analogue b/a (DESCENDING coeffs in s, lengths
 * nb_s/na_s). Writes B[0..*ndB] (numerator) and A[0..*ndA] (denominator) as ASCENDING coeffs in
 * z^-1, with A[0]=1. fs = sample rate.  */
/* Returns 1 on success, 0 if the design could not be trusted. A caller that gets 0 must KEEP THE
 * COEFFICIENTS IT ALREADY HAD rather than use what is in B/A: every failure mode here is silent,
 * and the final re-normalisation at wref hides the worst of them by making the gain come out right
 * while the shape is wrong. */
static inline int zc_matched_design(const double *b_s, int nb_s, const double *a_s, int na_s,
                                    double fs, double *B, int *ndB, double *A, int *ndA) {
  /* strip leading zeros from the analogue numerator (LP: b=[0,0,w0^2] -> zeros at infinity).
   * Zeros at s=inf have no matched-Z image; they are left out and the FIR correction fits the
   * magnitude (Nyquist included) in their place. */
  int off = 0; while (off < nb_s - 1 && b_s[off] == 0.0) off++;
  b_s += off; nb_s -= off;
  int nz = nb_s - 1, np = na_s - 1;
  #define ZC_MZ_ANDB(W) zc_mz_andb(b_s, nb_s, a_s, na_s, fs, (W))
  /* 1-2. matched-Z of poles and zeros */
  double _Complex zr[ZC_MZ_MAXNUM], pr[ZC_MZ_MAXNUM], dz[ZC_MZ_MAXNUM], dp[ZC_MZ_MAXNUM];
  if (!zc_mz_roots(b_s, nz, zr) || !zc_mz_roots(a_s, np, pr)) return 0;
  /* mirror RIGHT-half-plane zeros (non-minimum-phase analogue) into the LHP: this preserves |H(jw)|
   * EXACTLY (|jw-(s+jw0)| = |jw-(-s+jw0)|) and avoids the exp(+s/fs) overflow in matched-Z. */
  for (int i = 0; i < nz; i++) if (creal(zr[i]) > 0.0) zr[i] = -creal(zr[i]) + cimag(zr[i]) * _Complex_I;
  for (int i = 0; i < nz; i++) dz[i] = cexp(zr[i] / fs);
  for (int i = 0; i < np; i++) dp[i] = cexp(pr[i] / fs);
  zc_mz_poly(dz, nz, B); zc_mz_poly(dp, np, A);
  int nb2 = nz, na2 = np;
  /* reference frequency for the gain anchor: DC when its gain is NOT negligible (bells/shelves/LP
   * anchor at DC -- flat, clean, and far from the correction's HF action); when the DC gain is ~0
   * (HP: it has a zero at DC) anchor instead at the frequency of max |Han| (passband peak). */
  double wdc = 1e-6 * 2.0 * M_PI / fs, gdc_db = ZC_MZ_ANDB(wdc);
  double wmax = wdc, gmax_db = -1e9;
  /* The 400-point search only matters for prototypes whose DC gain is negligible, in practice the
   * high-pass, which has a zero at DC. Everything else anchors at DC and would discard the result,
   * so skipping it there saves about 16 % of the designer.
   *
   * It is skipped on a cheap BOUND rather than on the assumption: a coarse 24-point scan bounds the
   * peak, and the full search runs only when DC is not clearly the anchor. Over the taper range in
   * use the peak sits about 22 dB above DC against a 40 dB threshold, so the fast path is taken
   * with roughly 18 dB of margin; a taper that ever widened past that would trip the guard rather
   * than anchor at the wrong frequency. */
  {
    double coarse_max = -1e9;
    for (int m = 0; m < 24; m++) {
      double f = 20.0 * pow((fs * 0.5) / 20.0, (double)m / 23.0), w = 2.0 * M_PI * f / fs;
      double g = ZC_MZ_ANDB(w);
      if (g > coarse_max) coarse_max = g;
    }
    /* 6 dB of slack for what a 24-point scan can miss between its points */
    if (!(gdc_db > coarse_max - 40.0 + 6.0)) {
      for (int m = 0; m < 400; m++) { double f = 20.0 * pow((fs * 0.5) / 20.0, (double)m / 399.0), w = 2.0 * M_PI * f / fs;
        double g = ZC_MZ_ANDB(w); if (g > gmax_db) { gmax_db = g; wmax = w; } }
    }
  }
  double wref = (gdc_db > gmax_db - 40.0) ? wdc : wmax;
  double gref = pow(10.0, ZC_MZ_ANDB(wref) / 20.0);   /* |Han(wref)|, linear */
  /* 3. scale so that |Hdig(wref)| = gref */
  { double cur = pow(10.0, zc_mz_digdb(B, nb2, A, na2, wref) / 20.0);
    double sc = gref / cur; for (int k = 0; k <= nb2; k++) B[k] *= sc; }
  /* 4. FIR correction of degree NC: |C|^2 = sum_m rm*cos(mw), HF-weighted LS against |Han|^2/|Hdig|^2 */
  const int NC = ZC_MZ_CORR, P = NC + 1;
  double S[ZC_MZ_CORR + 1][ZC_MZ_CORR + 1], T[ZC_MZ_CORR + 1];
  for (int i = 0; i < P; i++) { T[i] = 0; for (int j = 0; j < P; j++) S[i][j] = 0; }
  for (int m = 0; m < 500; m++) {
    double f = 20.0 * pow((fs * 0.5) / 20.0, (double)m / 499.0), w = 2.0 * M_PI * f / fs;
    double cur = zc_mz_digdb(B, nb2, A, na2, w), tg = ZC_MZ_ANDB(w);
    if (cur < -120.0 || tg < -120.0) continue;          /* avoids 0/0 at the zeros (HP@DC, etc.) */
    double D2 = pow(10.0, (tg - cur) / 10.0);           /* (|Han|/|Hdig|)^2 */
    double wt = pow(f / (fs * 0.5), ZC_MZ_WPOW) + 0.02;
    double g[ZC_MZ_CORR + 1]; for (int aa = 0; aa < P; aa++) g[aa] = cos(aa * w);
    for (int aa = 0; aa < P; aa++) { for (int bb = 0; bb < P; bb++) S[aa][bb] += wt * g[aa] * g[bb]; T[aa] += wt * D2 * g[aa]; }
  }
  /* solve S*rm = T (Gauss) */
  double rm[ZC_MZ_CORR + 1];
  for (int i = 0; i < P; i++) {
    int pv = i; for (int r = i + 1; r < P; r++) if (fabs(S[r][i]) > fabs(S[pv][i])) pv = r;
    for (int k = 0; k < P; k++) { double t = S[i][k]; S[i][k] = S[pv][k]; S[pv][k] = t; }
    double t = T[i]; T[i] = T[pv]; T[pv] = t;
    for (int r = 0; r < P; r++) if (r != i) { double fct = S[r][i] / S[i][i];
      for (int k = 0; k < P; k++) S[r][k] -= fct * S[i][k];
      T[r] -= fct * T[i]; }
  }
  /* A ~zero pivot means the least-squares system is singular; dividing by it yields inf/nan that
   * would reach the filter state and never leave. */
  for (int i = 0; i < P; i++) if (!(fabs(S[i][i]) > 1e-300)) return 0;
  for (int i = 0; i < P; i++) rm[i] = T[i] / S[i][i];
  /* factor |C|^2 -> minimum-phase C: symmetric polynomial of degree 2NC, keep the roots inside the
   * unit circle */
  double lc[2 * ZC_MZ_CORR + 1]; for (int k = 0; k <= 2 * NC; k++) lc[k] = 0;
  lc[NC] = rm[0]; for (int mm = 1; mm <= NC; mm++) { lc[NC - mm] += rm[mm] / 2; lc[NC + mm] += rm[mm] / 2; }
  double hp[2 * ZC_MZ_CORR + 1]; for (int k = 0; k <= 2 * NC; k++) hp[k] = lc[2 * NC - k]; /* high->low */
  double _Complex rr[2 * ZC_MZ_CORR];
  if (NC > 0 && !zc_mz_roots(hp, 2 * NC, rr)) return 0;
  double _Complex cin[ZC_MZ_CORR]; int ci = 0;
  for (int i = 0; i < 2 * NC && ci < NC; i++) if (cabs(rr[i]) < 1.0) cin[ci++] = rr[i];
  /* The autocorrelation polynomial of |C|^2 is symmetric, so its roots come in reciprocal pairs
   * and exactly NC of them must lie inside the unit circle. Fewer means a pair landed ON the
   * circle and the correction is a degree short, which the re-normalisation at wref would hide by
   * making the gain come out right while the shape is wrong. */
  if (ci != NC) return 0;
  double cc[ZC_MZ_CORR + 1]; zc_mz_poly(cin, ci, cc);
  /* apply C(z) to the numerator: B <- B * C */
  double nn[ZC_MZ_MAXNUM + 1]; for (int k = 0; k <= nb2 + ci; k++) nn[k] = 0;
  for (int k = 0; k <= nb2; k++) for (int aa = 0; aa <= ci; aa++) nn[k + aa] += B[k] * cc[aa];
  for (int k = 0; k <= nb2 + ci; k++) B[k] = nn[k];
  nb2 += ci;
  /* re-normalise at wref after the correction */
  { double cur = pow(10.0, zc_mz_digdb(B, nb2, A, na2, wref) / 20.0);
    double sc = gref / cur; for (int k = 0; k <= nb2; k++) B[k] *= sc; }
  *ndB = nb2; *ndA = na2;
  for (int k = 0; k <= nb2; k++) if (!isfinite(B[k])) return 0;
  for (int k = 0; k <= na2; k++) if (!isfinite(A[k])) return 0;
  return 1;
  #undef ZC_MZ_ANDB
}
#endif
