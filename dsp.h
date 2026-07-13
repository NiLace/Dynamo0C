/* dsp.h -- DSP core of the equalizer (header-only, C99, no dependencies).
 *
 * WHAT IT DOES
 *   Processes audio through a chain of IIR sections: high-pass filter (HP),
 *   four EQ bands (the two outer ones switch bell<->shelf) and low-pass filter (LP).
 *   An optional non-linear DRIVE (input stage) sits before the chain and has its own oversampling.
 *
 * HOW IT WORKS (summary for the first-time reader)
 *   1. Each band/filter is first defined as an ANALOG transfer function
 *      H(s)=B(s)/A(s). For bells/shelves the coefficients come from closed-form formulas
 *      tabulated in coeffs.h (generated file; see its header); HP/LP are standard
 *      Sallen-Key biquads. The user controls (norm 0..1) are mapped to the filter
 *      parameters via interpolation tables ("tapers"), also in coeffs.h.
 *   2. That H(s) is converted to a DIGITAL filter with an ANALOG-MATCHED design (matched.h):
 *      matched-Z de polos+ceros + una corrección FIR que casa la magnitud analógica cerca de
 *      Nyquist SIN cramping. Fiel al analógico a ~0.3 dB en todo el recorrido, siempre estable
 *      (los polos vienen del denominador matched-Z de un analógico estable). El EQ es LINEAL, así
 *      que corre a la fs base: un biquad por banda, sin oversampling y con latencia cero.
 *   3. The chain runs sample by sample (Direct Form II transposed); el numerador puede ser de grado
 *      hasta 7 (nz<=2 + corrección FIR grado ZC_MZ_CORR=5), el denominador hasta 3.
 *
 *   The coefficients are only recomputed when a control changes (not per sample) and can be
 *   refreshed "live" without resetting the state, avoiding clicks when moving the controls.
 */
#ifndef ZC_DSP_H
#define ZC_DSP_H

#include <math.h>
#include <string.h>
#include "coeffs.h"
#include "matched.h"   /* diseñador analog-matched (matched-Z + corrección FIR), reemplaza bilineal+OS del EQ */

/* number of elements in a static array (so table lengths aren't hard-coded by hand) */
#define ZC_ARRLEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* high-pass Q per the BUMP button: 0.707 = flat Butterworth (no peak); ZC_HP_Q (~1.0) is a
 * deliberate VOICING choice — a gentle resonant lift (~+1.5 dB) just above the corner, in the
 * spirit of a console filter knee. It is hand-set, NOT part of the modelled hardware (whose
 * filter corners are flat); same for ZC_LP_Q_RESONANT on the low-pass. */
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

/* ----------------------------------------------------------------- sections */

typedef enum { ZC_BELL, ZC_HISHELF, ZC_LOSHELF, ZC_HP, ZC_LP } ZcKind;

/* One section: digital IIR con numerador de grado <=7 (nz<=2 + corrección FIR grado ZC_MZ_CORR=5) y
 * denominador de grado <=3, Direct Form II transposed. Los polos vienen solo del denominador
 * (matched-Z de un analógico estable) -> siempre estable. b[8] alberga hasta grado 7. */
typedef struct {
  int    nb;               /* grado del numerador (0..7) */
  int    na;               /* grado del denominador (0..3) */
  double b[8], a[4];       /* coeffs digitales ASC en z^-1 (a[0]=1) */
  double z[7];             /* estado, tamaño max(nb,na) */
} ZcSection;

static inline void zc_section_reset(ZcSection *s) { for (int i = 0; i < 7; i++) s->z[i] = 0.0; }

static inline double zc_section_tick(ZcSection *s, double x) {
  double y = s->b[0] * x + s->z[0];
  int N = (s->nb > s->na) ? s->nb : s->na;
  for (int i = 0; i < N - 1; i++) {
    double bi = (i + 1 <= s->nb) ? s->b[i + 1] : 0.0;
    double ai = (i + 1 <= s->na) ? s->a[i + 1] : 0.0;
    s->z[i] = bi * x - ai * y + s->z[i + 1];
  }
  { int i = N - 1;
    double bi = (i + 1 <= s->nb) ? s->b[i + 1] : 0.0;
    double ai = (i + 1 <= s->na) ? s->a[i + 1] : 0.0;
    s->z[i] = bi * x - ai * y; }
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

/* Builds a section: analog -> matched-Z + corrección FIR (a fs) -> fills ZcSection. */
/* NOTE: these builders only (re)compute b/a/nb/na; they do NOT touch the z[] state, so they serve to
 * update coefficients live (when moving a control) without clicks. The state reset is done by
 * zc_chain_build when the STRUCTURE of the chain changes. */
static inline void zc_section_build_bellshelf(ZcSection *sec, ZcKind kind,
        const float *ftap, const float *wtap, const double *cap,
        double nf, double ng, double fs) {
  double b[4], a[4]; int nb, na;
  zc_bellshelf_analog(kind, ftap, wtap, cap, nf, ng, b, &nb, a, &na);
  zc_matched_design(b, nb, a, na, fs, sec->b, &sec->nb, sec->a, &sec->na);
}
static inline void zc_section_build_filter(ZcSection *sec, ZcKind kind,
        double fc, double Q, double fs) {
  double b[4], a[4]; int nb, na;
  if (kind == ZC_HP) zc_hp_analog(fc, Q, b, &nb, a, &na);
  else                  zc_lp_analog(fc, Q, b, &nb, a, &na);
  zc_matched_design(b, nb, a, na, fs, sec->b, &sec->nb, sec->a, &sec->na);
}

/* ----------------------------------------------------------------- drive oversampling FIR (windowed-sinc 2x, cascade)
 * Used ONLY by the DRIVE (below): its non-linear shaper aliases, so it is run at 2x/4x and this
 * half-band FIR does the up/down sampling. The EQ is analog-matched and never oversamples. */
/* 16-tap windowed-sinc low-pass kernel (sinc with Blackman window): cutoff at the original
 * Nyquist, DC gain = 1. Used to upsample and downsample the drive. */
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

/* ---- drive with OPTIONAL oversampling ---------------------------------------------------------
 * The drive shaper is NON-LINEAR and MEMORYLESS -> it generates harmonics above Nyquist that fold
 * back as aliasing (measured: -14 dB signal/alias @9 kHz at max drive; compare/drive_alias). Unlike
 * the EQ (linear, no aliasing), the only clean fix is to run the shaper at a higher rate. This wraps
 * the drive in the windowed-sinc 2x FIR cascade above (up -> shaper@fs_os -> down), with a user
 * switch (1x/2x/4x). With os==1 it is BIT-IDENTICAL to the bare zc_drive path (stages=0, no FIR,
 * DC-blocker at base_fs), so "1x" reproduces the plain drive exactly (no regression). */
#define ZC_DRV_MAX_STAGES 2   /* 2^2 = 4x maximum for the drive */
typedef struct {
  ZcDrive drv;                                   /* shaper + DC-blocker, coeffs set at fs_os */
  int os, stages;                                /* 1/2/4 ; log2(os) */
  ZcFir up[ZC_DRV_MAX_STAGES], dn[ZC_DRV_MAX_STAGES];
} ZcDriveOS;

static inline void zc_driveos_reset(ZcDriveOS *d) {
  zc_drive_reset(&d->drv);
  for (int s = 0; s < d->stages; s++) { zc_fir_reset(&d->up[s]); zc_fir_reset(&d->dn[s]); }
}
/* set gain/DC-blocker for the (possibly oversampled) rate; recomputes os/stages. Does NOT reset the
 * FIR/DC state -> safe for a live knob move (same os). Caller resets when os changes or on engage. */
static inline void zc_driveos_set(ZcDriveOS *d, double knob, double base_fs, int os) {
  d->os = os; d->stages = 0; for (int o = os; o > 1; o >>= 1) d->stages++;
  zc_drive_set(&d->drv, knob, base_fs * os);     /* DC-blocker frequency tracks fs_os */
}
/* process a block: upsample (2x cascade) -> memoryless shaper@fs_os -> downsample. os==1 = bare path.
 * sa/sb = ping-pong scratch, each >= n*os doubles. */
static inline void zc_driveos_process(ZcDriveOS *d, const float *in, float *out, int n,
                                       double *sa, double *sb) {
  if (d->os == 1) { for (int i = 0; i < n; i++) out[i] = (float)zc_drive_tick(&d->drv, in[i]); return; }
  double *cur = sa, *nxt = sb; int len = n;
  for (int i = 0; i < n; i++) cur[i] = in[i];
  for (int s = 0; s < d->stages; s++) {          /* upsample: each stage 2x (FIR has memory) */
    for (int i = 0; i < len; i++) {
      zc_fir_push(&d->up[s], cur[i]);   nxt[2 * i]     = zc_fir_conv(&d->up[s]) * 2.0;
      zc_fir_push(&d->up[s], 0.0);      nxt[2 * i + 1] = zc_fir_conv(&d->up[s]) * 2.0;
    }
    len *= 2; double *t = cur; cur = nxt; nxt = t;
  }
  for (int i = 0; i < len; i++) cur[i] = zc_drive_tick(&d->drv, cur[i]);   /* shaper at fs_os */
  for (int s = 0; s < d->stages; s++) {          /* downsample: each stage /2 */
    int half = len / 2;
    for (int i = 0; i < half; i++) {
      zc_fir_push(&d->dn[s], cur[2 * i]); zc_fir_push(&d->dn[s], cur[2 * i + 1]);
      nxt[i] = zc_fir_conv(&d->dn[s]);
    }
    len = half; double *t = cur; cur = nxt; nxt = t;
  }
  for (int i = 0; i < n; i++) out[i] = (float)cur[i];
}

/* Latency (samples, at the base rate) the drive oversampling adds: the group delay of the up/down FIR
 * round-trip for a given os. The shaper is memoryless (0 delay), so this IS the whole drive-OS latency.
 * os==1 -> 0. Measured with an impulse through the FIR cascade only (no shaper). */
static inline float zc_driveos_latency(int os) {
  if (os <= 1) return 0.0f;
  int stages = 0; for (int o = os; o > 1; o >>= 1) stages++;
  ZcFir up[ZC_DRV_MAX_STAGES], dn[ZC_DRV_MAX_STAGES];
  for (int s = 0; s < stages; s++) { zc_fir_reset(&up[s]); zc_fir_reset(&dn[s]); }
  enum { N = 256 };
  double a[N * 4], b[N * 4], *cur = a, *nxt = b; int len = N;
  for (int i = 0; i < N; i++) cur[i] = (i == 0) ? 1.0 : 0.0;
  for (int s = 0; s < stages; s++) {
    for (int i = 0; i < len; i++) {
      zc_fir_push(&up[s], cur[i]);  nxt[2 * i]     = zc_fir_conv(&up[s]) * 2.0;
      zc_fir_push(&up[s], 0.0);     nxt[2 * i + 1] = zc_fir_conv(&up[s]) * 2.0;
    }
    len *= 2; double *t = cur; cur = nxt; nxt = t;
  }
  for (int s = 0; s < stages; s++) {
    int half = len / 2;
    for (int i = 0; i < half; i++) {
      zc_fir_push(&dn[s], cur[2 * i]); zc_fir_push(&dn[s], cur[2 * i + 1]);
      nxt[i] = zc_fir_conv(&dn[s]);
    }
    len = half; double *t = cur; cur = nxt; nxt = t;
  }
  double num = 0, den = 0;
  for (int i = 0; i < len; i++) { double e = cur[i] * cur[i]; num += i * e; den += e; }
  return den > 0 ? (float)(num / den) : 0.0f;
}

/* ----------------------------------------------------------------- full chain
 * The EQ is analog-matched (matched-Z + FIR correction): it reproduces the analog magnitude near
 * Nyquist WITHOUT cramping, so the chain runs at the base sample rate — one biquad-style section per
 * band, no oversampling, no latency. (Aliasing only matters for the non-linear DRIVE, which has its
 * own oversampling above; the EQ is linear and needs none.) */
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

/* fills c->sec[] and c->nsec from the controls (at c->fs). Does NOT touch the z[] state. */
static inline void zc_chain_fill(ZcChain *c, const ZcControls *ct) {
  double fs = c->fs; c->nsec = 0;
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

/* builds the chain FROM SCRATCH (structure change): fills + RESETS state. */
static inline void zc_chain_build(ZcChain *c, const ZcControls *ct, double base_fs) {
  c->fs = base_fs;
  zc_chain_fill(c, ct);
  zc_chain_reset_state(c);
}

/* LIVE refresh of coefficients (structure unchanged): keeps state -> no clicks. */
static inline void zc_chain_update(ZcChain *c, const ZcControls *ct) {
  zc_chain_fill(c, ct);
}

#endif /* ZC_DSP_H */
