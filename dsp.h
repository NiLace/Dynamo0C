/* dsp.h -- DSP core of the equalizer (header-only, C99, no dependencies).
 *
 * WHAT IT DOES
 *   Processes audio through a chain of IIR sections: high-pass filter (HP),
 *   four EQ bands (the two outer ones switch bell<->shelf) and low-pass filter (LP).
 *
 * HOW IT WORKS (summary for the first-time reader)
 *   1. Each band/filter is first defined as an ANALOG transfer function
 *      H(s)=B(s)/A(s). For bells/shelves the coefficients come from closed-form formulas
 *      tabulated in coeffs.h (generated file; see its header); HP/LP are standard
 *      Sallen-Key biquads. The user controls (norm 0..1) are mapped to the filter
 *      parameters via interpolation tables ("tapers"), also in coeffs.h.
 *   2. That H(s) is converted to a DIGITAL filter with the BILINEAR transform, evaluated at a
 *      frequency OS*fs (oversampling) so the response is not warped near Nyquist.
 *   3. The chain runs sample by sample (Direct Form II transposed), with upsampling and
 *      downsampling by a half-band FIR when OS>1.
 *
 *   The coefficients are only recomputed when a control changes (not per sample) and can be
 *   refreshed "live" without resetting the state, avoiding clicks when moving the controls.
 */
#ifndef ZC_DSP_H
#define ZC_DSP_H

#include <math.h>
#include <string.h>
#include "coeffs.h"

/* number of elements in a static array (so table lengths aren't hard-coded by hand) */
#define ZC_ARRLEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* high-pass Q per the BUMP button: 0.707 = flat Butterworth (no peak); ZC_HP_Q (measured,
 * ~1.0) = the slight resonance of the original circuit that boosts ~+1.5 dB just above the cutoff. */
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

/* ----------------------------------------------------------------- bilinear transform */

/* binomial coefficient C(n,k), for the expansion of the bilinear substitution */
static inline double zc_comb(int n, int k) {
  if (k < 0 || k > n) return 0.0;
  double r = 1.0;
  for (int i = 0; i < k; i++) r = r * (n - i) / (i + 1);
  return r;
}

/* Bilinear transform: converts an ANALOG filter H(s)=B(s)/A(s) into a DIGITAL one at the
 * sample rate fs, substituting s = 2*fs*(1-z^-1)/(1+z^-1) and expanding.
 *   - b/a: coefficients in DESCENDING powers of s (b[0] = highest-degree term).
 *   - bz/az: output, digital coefficients in descending powers of z, normalized az[0]=1.
 *   - nb/na: lengths of b/a. Returns M = order of the digital filter (= max(deg B, deg A)). */
static inline int zc_bilinear(const double *b, int nb, const double *a, int na,
                                 double fs, double *bz, double *az) {
  int N = nb - 1, D = na - 1;
  int M = (D > N) ? D : N;
  double two_fs[4]; two_fs[0] = 1.0;
  for (int i = 1; i <= M; i++) two_fs[i] = two_fs[i - 1] * (2.0 * fs);
  for (int j = 0; j <= M; j++) {
    double vb = 0.0, va = 0.0;
    for (int i = 0; i <= M; i++) {
      for (int k = 0; k <= i; k++) {
        int l = j - k;
        if (l < 0 || l > M - i) continue;
        double w = zc_comb(i, k) * zc_comb(M - i, l) * two_fs[i] * ((k & 1) ? -1.0 : 1.0);
        if (i <= N) vb += w * b[N - i];
        if (i <= D) va += w * a[D - i];
      }
    }
    bz[j] = vb; az[j] = va;
  }
  double a0 = az[0];
  for (int j = 0; j <= M; j++) { bz[j] /= a0; az[j] /= a0; }
  return M;
}

/* ----------------------------------------------------------------- sections */

typedef enum { ZC_BELL, ZC_HISHELF, ZC_LOSHELF, ZC_HP, ZC_LP } ZcKind;

/* One section realized as a digital IIR of order <=3 (Direct Form II transposed). */
typedef struct {
  int    order;            /* 1..3 */
  double b[4], a[4];       /* digital coeffs (a[0]=1) */
  double z[3];             /* state */
} ZcSection;

static inline void zc_section_reset(ZcSection *s) { s->z[0] = s->z[1] = s->z[2] = 0.0; }

static inline double zc_section_tick(ZcSection *s, double x) {
  double y = s->b[0] * x + s->z[0];
  for (int i = 0; i < s->order - 1; i++)
    s->z[i] = s->b[i + 1] * x - s->a[i + 1] * y + s->z[i + 1];
  s->z[s->order - 1] = s->b[s->order] * x - s->a[s->order] * y;
  return y;
}

/* gain divider: the taper position W in [0,1] is split into two resistances
 * Rt, Rb of the model (W=0.5 -> Rt=Rb -> neutral gain; +1 avoids division by zero) */
static inline void zc_Rt_Rb(double W, double *Rt, double *Rb) {
  *Rt = 10000.0 * W + 1.0; *Rb = 10000.0 * (1.0 - W) + 1.0;
}

/* Builds the ANALOG coeffs (b,a descending) of a bell/shelf section
 * from the norm controls (nf, ng) and their tapers/caps. */
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

/* Builds a section: analog -> bilinear at fs_os -> fills ZcSection. */
/* NOTE: these builders only (re)compute b/a/order; they do NOT touch the z[] state, so they serve to
 * update coefficients live (when moving a control) without clicks. The state reset is done by
 * zc_chain_build when the STRUCTURE of the chain changes. */
static inline void zc_section_build_bellshelf(ZcSection *sec, ZcKind kind,
        const float *ftap, const float *wtap, const double *cap,
        double nf, double ng, double fs_os) {
  double b[4], a[4]; int nb, na;
  zc_bellshelf_analog(kind, ftap, wtap, cap, nf, ng, b, &nb, a, &na);
  sec->order = zc_bilinear(b, nb, a, na, fs_os, sec->b, sec->a);
}
static inline void zc_section_build_filter(ZcSection *sec, ZcKind kind,
        double fc, double Q, double fs_os) {
  double b[4], a[4]; int nb, na;
  if (kind == ZC_HP) zc_hp_analog(fc, Q, b, &nb, a, &na);
  else                  zc_lp_analog(fc, Q, b, &nb, a, &na);
  sec->order = zc_bilinear(b, nb, a, na, fs_os, sec->b, sec->a);
}

/* ----------------------------------------------------------------- oversampling (windowed-sinc 2x FIR, cascade) */
/* 16-tap windowed-sinc low-pass kernel (sinc with Blackman window): cutoff at the original
 * Nyquist, DC gain = 1. Used to upsample and downsample in the oversampling. */
#define ZC_OS_TAPS 16
#define ZC_OS_MASK (ZC_OS_TAPS - 1)        /* TAPS is a power of 2 -> circular indexing via mask */
static const double zc_os_kernel[ZC_OS_TAPS] = {
   0.000000000, -0.000580148,  0.003153358,  0.010039765,
  -0.025332544, -0.056707105,  0.127400442,  0.442026233,
   0.442026233,  0.127400442, -0.056707105, -0.025332544,
   0.010039765,  0.003153358, -0.000580148,  0.000000000
};
/* Delay line as a CIRCULAR BUFFER: 'w' is the index of the newest sample. push() only
 * moves the index (before: memmove of 15 doubles PER SAMPLE = ~138 MB/s of useless shuffling). */
typedef struct { double z[ZC_OS_TAPS]; int w; } ZcFir;
static inline void zc_fir_reset(ZcFir *f) { memset(f->z, 0, sizeof f->z); f->w = 0; }
static inline void zc_fir_push(ZcFir *f, double x) {
  f->w = (f->w - 1) & ZC_OS_MASK; f->z[f->w] = x;
}
/* conv = sum_k kernel[k]*z[k]  (z[0]=newest -> z[(w+k)&mask] in the circular buffer).
 * Folded by the SYMMETRY of the kernel (kernel[k]==kernel[15-k]): 8 multiplications instead of 16. */
static inline double zc_fir_conv(const ZcFir *f) {
  const double *z = f->z; int w = f->w; double s = 0.0;
  for (int k = 0; k < ZC_OS_TAPS / 2; k++)
    s += zc_os_kernel[k] * (z[(w + k) & ZC_OS_MASK] + z[(w + ZC_OS_MASK - k) & ZC_OS_MASK]);
  return s;
}

/* ----------------------------------------------------------------- channel drive (input stage)
 * ASYMMETRIC memoryless shaper (console/transformer style) BOUNDED: a SKEWED tanh that matches a
 * reference cubic curve over its valid range and SATURATES cleanly outside it (the raw cubic folds back at
 * -7.7 dBFS). Followed by a 1st-order DC-blocker ~14.6 Hz (removes the offset introduced by the asymmetry).
 * Memoryless.
 * Knob d (0..1): 0 = BYPASS; 0.5 = nominal intensity (pre-gain g=1); g = 2*d.
 *   y = f(g*x)/g  -> small-signal gain is CONSTANT in g, equal to f'(0) (ZC_DRV_FP0 ~= 0.978, not unity). */
#define ZC_DRV_S  0.4195088267
#define ZC_DRV_K  2.4111696100
#define ZC_DRV_B  0.0758583852
#define ZC_DRV_TB 0.1808946335   /* tanh(K*B): DC offset of the shaper, to be subtracted */
#define ZC_DRV_FP0 0.9784075257  /* f'(0): small-signal slope (g->0 limit) */
static inline double zc_drive_shape(double u) { return ZC_DRV_S * (tanh(ZC_DRV_K * (u + ZC_DRV_B)) - ZC_DRV_TB); }

typedef struct {
  double g;            /* pre-gain (0 = stage inactive) */
  double r;            /* DC-blocker coeff (~0.998) */
  double x1, y1;       /* DC-blocker state (post-shaper) */
} ZcDrive;
static inline void zc_drive_reset(ZcDrive *d) { d->x1 = 0.0; d->y1 = 0.0; }
static inline void zc_drive_set(ZcDrive *d, double knob, double base_fs) {
  d->g = 2.0 * knob;                                   /* knob 0.5 -> g=1 = nominal intensity */
  d->r = 1.0 - 2.0 * M_PI * 14.62 / base_fs;           /* 1st-order DC-blocker at 14.6 Hz */
}
static inline double zc_drive_tick(ZcDrive *d, double x) {
  double g = d->g;
  double s = (g > 1e-6) ? zc_drive_shape(g * x) / g : ZC_DRV_FP0 * x;   /* clean g->0 limit */
  double y = s - d->x1 + d->r * d->y1;                  /* y[n] = s[n] - s[n-1] + r*y[n-1] */
  d->x1 = s; d->y1 = y;
  return y;
}

/* ----------------------------------------------------------------- full chain */
#define ZC_MAX_SEC 6      /* HP + 4 bands + LP */
#define ZC_MAX_STAGES 3   /* 2^3 = 8x maximum */

typedef struct {
  ZcSection sec[ZC_MAX_SEC];
  int    nsec;
  int    os;                 /* 1, 2, 4, 8 */
  int    stages;             /* log2(os) */
  double fs_os;              /* fs * os */
  ZcFir up[ZC_MAX_STAGES], dn[ZC_MAX_STAGES];
} ZcChain;

static inline void zc_chain_reset_state(ZcChain *c) {
  for (int i = 0; i < c->nsec; i++) zc_section_reset(&c->sec[i]);
  for (int s = 0; s < c->stages; s++) { zc_fir_reset(&c->up[s]); zc_fir_reset(&c->dn[s]); }
}

/* runs the cascade of sections (at fs_os) over one sample */
static inline double zc_chain_tick(ZcChain *c, double x) {
  for (int i = 0; i < c->nsec; i++) x = zc_section_tick(&c->sec[i], x);
  return x;
}

/* processes a block: upsample (cascade of 2x) -> IIR cascade -> downsample.
 * 'sa' and 'sb' = ping-pong scratch, each one >= n*os samples. */
static inline void zc_chain_process(ZcChain *c, const float *in, float *out,
                                       int n, double *sa, double *sb) {
  if (c->os == 1) {
    for (int i = 0; i < n; i++) out[i] = (float)zc_chain_tick(c, in[i]);
    return;
  }
  double *cur = sa, *nxt = sb;
  int len = n;
  for (int i = 0; i < n; i++) cur[i] = in[i];
  /* upsampling: each stage 2x, FORWARD (the FIR has memory) */
  for (int s = 0; s < c->stages; s++) {
    for (int i = 0; i < len; i++) {
      zc_fir_push(&c->up[s], cur[i]);
      nxt[2 * i] = zc_fir_conv(&c->up[s]) * 2.0;
      zc_fir_push(&c->up[s], 0.0);
      nxt[2 * i + 1] = zc_fir_conv(&c->up[s]) * 2.0;
    }
    len *= 2; double *t = cur; cur = nxt; nxt = t;
  }
  /* IIR cascade at fs_os */
  for (int i = 0; i < len; i++) cur[i] = zc_chain_tick(c, cur[i]);
  /* downsampling: each stage /2 */
  for (int s = 0; s < c->stages; s++) {
    int half = len / 2;
    for (int i = 0; i < half; i++) {
      zc_fir_push(&c->dn[s], cur[2 * i]);
      zc_fir_push(&c->dn[s], cur[2 * i + 1]);
      nxt[i] = zc_fir_conv(&c->dn[s]);
    }
    len = half; double *t = cur; cur = nxt; nxt = t;
  }
  for (int i = 0; i < n; i++) out[i] = (float)cur[i];
}

/* ----------------------------------------------------------------- controls -> chain */
/* State of the controls the chain needs to build itself.
 * Band indices: 0=LOW, 1=LO-MID, 2=HI-MID, 3=HIGH. Only the outer ones (0 and 3)
 * can switch between bell and shelf; the two central ones are always bell.
 * All continuous values (freq, gain) are norm 0..1 (gain 0.5 = neutral gain). */
typedef struct {
  int    hp_on;    double hp_freq;
  int    hp_bump;        /* HP: 0 = clean Butterworth / 1 = resonant (BUMP, ~+1.5 dB at the cutoff) */
  struct { int on; int shelf; double freq, gain; } band[4];
  int    lp_on;    double lp_freq;
  int    lp_resonant;     /* LP: 0 = Q=0.707 (no peak) / 1 = Q=0.99 (with resonant boost) */
} ZcControls;

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

/* fills c->sec[] and c->nsec from the controls (at c->fs_os). Does NOT touch the z[] state. */
static inline void zc_chain_fill(ZcChain *c, const ZcControls *ct) {
  double fs = c->fs_os; c->nsec = 0;
  if (ct->hp_on) {
    double fc = zc_interp(ct->hp_freq, ZC_HP_NORM, ZC_HP_HZ, ZC_ARRLEN(ZC_HP_NORM));
    double q = ct->hp_bump ? ZC_HP_Q : ZC_HP_Q_FLAT;
    zc_section_build_filter(&c->sec[c->nsec++], ZC_HP, fc, q, fs);
  }
  for (int b = 0; b < 4; b++) {
    if (!ct->band[b].on) continue;
    if ((b == 0 || b == 3) && ct->band[b].shelf) {
      ZcKind kind = (b == 3) ? ZC_HISHELF : ZC_LOSHELF;
      const float *ft = (b == 3) ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF;
      const float *wt = (b == 3) ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF;
      const double *cap = (b == 3) ? ZC_CAP_HI_SHELF : ZC_CAP_LO_SHELF;
      zc_section_build_bellshelf(&c->sec[c->nsec++], kind, ft, wt, cap,
                                    ct->band[b].freq, ct->band[b].gain, fs);
    } else {
      zc_section_build_bellshelf(&c->sec[c->nsec++], ZC_BELL,
            zc_ftap_band(b), zc_wtap_band(b), zc_cap_band(b),
            ct->band[b].freq, ct->band[b].gain, fs);
    }
  }
  if (ct->lp_on) {
    double fc = zc_interp(ct->lp_freq, ZC_LP_NORM, ZC_LP_HZ, ZC_ARRLEN(ZC_LP_NORM));
    double Q = ct->lp_resonant ? ZC_LP_Q_RESONANT : ZC_LP_Q_CLEAN;
    zc_section_build_filter(&c->sec[c->nsec++], ZC_LP, fc, Q, fs);
  }
}

/* builds the chain FROM SCRATCH (structure/OS change): fills + RESETS state. */
static inline void zc_chain_build(ZcChain *c, const ZcControls *ct,
                                     double base_fs, int os) {
  c->os = os; c->stages = 0; for (int o = os; o > 1; o >>= 1) c->stages++;
  c->fs_os = base_fs * os;
  zc_chain_fill(c, ct);
  zc_chain_reset_state(c);
}

/* LIVE refresh of coefficients (structure/OS unchanged): keeps state -> no clicks. */
static inline void zc_chain_update(ZcChain *c, const ZcControls *ct) {
  zc_chain_fill(c, ct);
}

/* ----------------------------------------------------------------- ADAPTIVE oversampling
 * The EQ is LINEAR: there is no aliasing to suppress. The only reason for oversampling is to avoid
 * bilinear "cramping" near Nyquist (high resonant/bright sections). So the OS
 * needed depends on the MATERIAL: a dark chain runs at 1x (= minimum cost); it only rises to
 * 2x/4x when there is high resonant content. This is the C version of modelo_v2.discretiza_adaptive:
 * for each section we look for the minimum OS whose bilinear-vs-analog error < target_db in the
 * AUDIBLE band (where |H|>=-15 dB; cramping in the deep stopband is inaudible); the chain's OS =
 * the maximum of those per-section OS. */

/* |H| of a polynomial (DESCENDING coeffs) evaluated at s = jw.
 * (jw)^d = j^d * w^d; j^d cycles {1, j, -1, -j} with d mod 4 -> (cs[d&3], sn[d&3]). */
static inline double zc_mag_s(const double *c, int n, double w) {
  static const double cs[4] = {1,0,-1,0}, sn[4] = {0,1,0,-1};
  int M = n - 1; double re = 0.0, im = 0.0;
  for (int k = 0; k <= M; k++) {
    int d = M - k;
    double wd = 1.0; for (int e = 0; e < d; e++) wd *= w;   /* w^d (d small: <= polynomial order) */
    re += c[k]*wd*cs[d&3]; im += c[k]*wd*sn[d&3];
  }
  return hypot(re, im);
}
/* |H| of a polynomial (DESCENDING coeffs in z) evaluated at z = e^{j*th}. */
static inline double zc_mag_z(const double *c, int n, double th) {
  int M = n - 1; double re = 0.0, im = 0.0;
  for (int k = 0; k <= M; k++) { int d = M - k; re += c[k]*cos(d*th); im += c[k]*sin(d*th); }
  return hypot(re, im);
}
/* minimum OS (from os_opts) that keeps the bilinear error < target_db in the section's audible band.
 * b/a = ANALOG descending coeffs (nb/na = lengths). */
static inline int zc_section_os(const double *b, int nb, const double *a, int na,
                                double base_fs, double target_db, const int *os_opts, int nopt) {
  enum { NF = 48 };
  double fmax = 0.45 * base_fs; if (fmax > 16000.0) fmax = 16000.0;
  const double fmin = 20.0, lr = log(fmax / fmin);
  double ha[NF], fr[NF];
  for (int i = 0; i < NF; i++) {
    double f = fmin * exp(lr * i / (NF - 1)); fr[i] = f; double w = 2.0*M_PI*f;
    ha[i] = 20.0 * log10(zc_mag_s(b, nb, w) / zc_mag_s(a, na, w));
  }
  for (int o = 0; o < nopt; o++) {
    int OS = os_opts[o]; double fsos = base_fs * OS;
    double bz[4], az[4]; int order = zc_bilinear(b, nb, a, na, fsos, bz, az);
    double emax = 0.0;
    for (int i = 0; i < NF; i++) {
      if (ha[i] < -15.0) continue;                 /* only where the filter PASSES: the rest is inaudible */
      double th = 2.0*M_PI*fr[i]/fsos;
      double hd = 20.0 * log10(zc_mag_z(bz, order+1, th) / zc_mag_z(az, order+1, th));
      double e = fabs(hd - ha[i]); if (e > emax) emax = e;
    }
    if (emax <= target_db) return OS;
  }
  return os_opts[nopt - 1];
}
/* GLOBAL adaptive OS of the chain = max of the per-active-section minimum OS. Iterates over the same
 * sections as zc_chain_fill, but requesting the ANALOG coeffs (does not discretize). */
static inline int zc_chain_adaptive_os(const ZcControls *ct, double base_fs, double target_db) {
  static const int OPTS[3] = {1, 2, 4};
  int os = 1; double b[4], a[4]; int nb, na;
  if (ct->hp_on) {
    double fc = zc_interp(ct->hp_freq, ZC_HP_NORM, ZC_HP_HZ, ZC_ARRLEN(ZC_HP_NORM));
    double q = ct->hp_bump ? ZC_HP_Q : ZC_HP_Q_FLAT;
    zc_hp_analog(fc, q, b, &nb, a, &na);
    int o = zc_section_os(b, nb, a, na, base_fs, target_db, OPTS, 3); if (o > os) os = o;
  }
  for (int bd = 0; bd < 4; bd++) {
    if (!ct->band[bd].on) continue;
    if ((bd == 0 || bd == 3) && ct->band[bd].shelf) {
      ZcKind kind = (bd == 3) ? ZC_HISHELF : ZC_LOSHELF;
      const float *ft = (bd == 3) ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF;
      const float *wt = (bd == 3) ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF;
      const double *cap = (bd == 3) ? ZC_CAP_HI_SHELF : ZC_CAP_LO_SHELF;
      zc_bellshelf_analog(kind, ft, wt, cap, ct->band[bd].freq, ct->band[bd].gain, b, &nb, a, &na);
    } else {
      zc_bellshelf_analog(ZC_BELL, zc_ftap_band(bd), zc_wtap_band(bd), zc_cap_band(bd),
                          ct->band[bd].freq, ct->band[bd].gain, b, &nb, a, &na);
    }
    int o = zc_section_os(b, nb, a, na, base_fs, target_db, OPTS, 3); if (o > os) os = o;
  }
  if (ct->lp_on) {
    double fc = zc_interp(ct->lp_freq, ZC_LP_NORM, ZC_LP_HZ, ZC_ARRLEN(ZC_LP_NORM));
    double Q = ct->lp_resonant ? ZC_LP_Q_RESONANT : ZC_LP_Q_CLEAN;
    zc_lp_analog(fc, Q, b, &nb, a, &na);
    int o = zc_section_os(b, nb, a, na, base_fs, target_db, OPTS, 3); if (o > os) os = o;
  }
  return os;
}

#endif /* ZC_DSP_H */
