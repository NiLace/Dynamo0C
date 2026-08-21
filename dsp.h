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
 *      matched-Z of poles+zeros plus an FIR correction that fits the analogue magnitude near
 *      Nyquist WITHOUT cramping. Faithful to the analogue within ~0.3 dB over the whole range, and
 *      always stable (the poles come from the matched-Z denominator of a stable analogue
 *      prototype). The EQ is LINEAR, so it runs at the base rate: one biquad-style section per
 *      band, no oversampling, zero latency.
 *   3. The chain runs sample by sample (Direct Form II transposed); the numerator can reach degree
 *      7 (nz<=2 plus the FIR correction of degree ZC_MZ_CORR=5), the denominator degree 3.
 *
 *   The coefficients are only recomputed when a control changes (not per sample) and can be
 *   refreshed "live" without resetting the state, avoiding clicks when moving the controls.
 */
#ifndef ZC_DSP_H
#define ZC_DSP_H

#include <math.h>
#include <string.h>
#include <stdint.h>
/* coeffs.h is a GENERATED file and trips -Wunused-parameter twice: zc_hishelf_coeffs takes Ct and
 * zc_loshelf_coeffs takes Cf, and neither topology uses that capacitor. That is by design, not a
 * defect -- the three coefficient functions share ONE uniform signature so zc_bellshelf_analog
 * below can call any of them through the same line, and each uses the cap it needs.
 *
 * The suppression lives here rather than inside coeffs.h because a regeneration would drop it, and
 * its scope is just this include: warnings stay switched on for the hand-written code, which is
 * what lets these sources build with -Wextra -Werror.
 *
 * Note that compare/provenance.c and compare/eqdata.c include coeffs.h directly and are NOT
 * covered by this; they need the same wrapper if they are ever built with -Wextra. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "coeffs.h"
#pragma GCC diagnostic pop

#include "matched.h"   /* analog-matched designer (matched-Z + FIR correction); replaces the EQ's bilinear+OS pair */

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

/* ----------------------------------------------------------------- sections */

typedef enum { ZC_BELL, ZC_HISHELF, ZC_LOSHELF, ZC_HP, ZC_LP } ZcKind;

/* One section: digital IIR, Direct Form II transposed. The poles come from the denominator alone
 * (matched-Z of a stable analogue prototype), so it is always stable.
 * The array sizes are derived from the designer's own bound rather than written out by hand. With
 * ZC_MZ_CORR = 5 that gives b[8] / z[7]; building with -DZC_MZ_CORR=6, which matched.h explicitly
 * allows, would overflow both if the sizes were fixed. Derived, it either fits or fails to
 * compile. */
#define ZC_MAX_NB   ZC_MZ_MAX_NB           /* highest numerator degree the designer can emit */
#define ZC_MAX_NA   3                      /* denominator degree: analogue prototype, not NC */
/* zc_section_tick indexes z[0..N-1] with N = max(nb, na), so the state is sized by the LARGER of
 * the two degrees. Sizing it from the numerator alone is wrong at low correction orders, which is
 * what the assertion below is there to catch at compile time. */
#define ZC_MAX_N    (ZC_MAX_NB > ZC_MAX_NA ? ZC_MAX_NB : ZC_MAX_NA)
_Static_assert(ZC_MAX_NB >= 2, "numerator must hold at least the analogue zeros");

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
 * whose real degree is below the maximum behaves exactly as its own degree dictates. */
static inline double zc_section_tick(ZcSection *s, double x) {
  double y = s->b[0] * x + s->z[0];
  for (int i = 0; i < ZC_MAX_N - 1; i++)
    s->z[i] = s->b[i + 1] * x - s->a[i + 1] * y + s->z[i + 1];
  s->z[ZC_MAX_N - 1] = s->b[ZC_MAX_N] * x - s->a[ZC_MAX_N] * y;
  return y;
}

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

static inline void zc_section_build_bellshelf(ZcSection *sec, ZcKind kind,
        const float *ftap, const float *wtap, const double *cap,
        double nf, double ng, double fs) {
  double b[4], a[4]; int nb, na;
  double tb[ZC_MAX_N + 1], ta[ZC_MAX_N + 1]; int tnb, tna;
  zc_bellshelf_analog(kind, ftap, wtap, cap, nf, ng, b, &nb, a, &na);
  int ok = zc_matched_design(b, nb, a, na, fs, tb, &tnb, ta, &tna);
  if (ok) zc_section_store(sec, tb, tnb, ta, tna);
  zc_section_keep_or_bypass(sec, ok);
}
static inline void zc_section_build_filter(ZcSection *sec, ZcKind kind,
        double fc, double Q, double fs) {
  double b[4], a[4]; int nb, na;
  double tb[ZC_MAX_N + 1], ta[ZC_MAX_N + 1]; int tnb, tna;
  if (kind == ZC_HP) zc_hp_analog(fc, Q, b, &nb, a, &na);
  else                  zc_lp_analog(fc, Q, b, &nb, a, &na);
  int ok = zc_matched_design(b, nb, a, na, fs, tb, &tnb, ta, &tna);
  if (ok) zc_section_store(sec, tb, tnb, ta, tna);
  zc_section_keep_or_bypass(sec, ok);
}

/* ----------------------------------------------------------------- drive oversampling FIR (windowed-sinc 2x, cascade)
 * Used ONLY by the DRIVE (below): its non-linear shaper aliases, so it is run at 2x/4x and this
 * half-band FIR does the up/down sampling. The EQ is analog-matched and never oversamples. */
/* Half-band low-pass kernel for the drive's oversampling: 31 taps, Kaiser(beta = 5.0), cutoff
 * exactly fs_os/4, DC gain 1, padded with one trailing zero to 32 so the reference path keeps a
 * power-of-two buffer.
 *
 * In a half-band every tap at an even offset from the centre is exactly zero, which here makes all
 * the ODD indices zero except the centre. In polyphase form one phase of the interpolator
 * collapses to a single coefficient -- a pure delay -- so 32 multiplies per input sample become
 * 16 + 1. Those zeros are structural rather than a fitting accident, and compare/genhalfband.py
 * refuses any length that is not 4m+3 rather than quietly designing a worse filter.
 *
 * The symmetry is around index 15, NOT 15.5: kernel[k] == kernel[30-k] with kernel[31] == 0, so
 * kernel[k] == kernel[31-k] is false and a convolution folded on that identity would compute the
 * wrong thing. zc_fir_conv below is unfolded for exactly this reason.
 *
 * Group delay is 15 samples at the oversampled rate. Nothing here hardcodes the resulting plugin
 * latency; it is derived from the oversampling factor where it is reported.
 *
 * Regenerate with compare/genhalfband.py, which pins the symmetry after normalising. Do not
 * hand-edit: the folded and polyphase paths both rely on the exact zeros and the exact mirror. */
#define ZC_OS_TAPS 32
#define ZC_OS_MASK (ZC_OS_TAPS - 1)        /* TAPS is a power of 2 -> circular indexing via mask */
static const double zc_os_kernel[ZC_OS_TAPS] = {
  -0.00077911205399922078,  0,  0.0029449330549970549,  0,
  -0.0072044221929927947,  0,  0.014675772709985322,  0,
  -0.027254942768972744,  0,  0.049363779515950633,  0,
  -0.096968811873903021,  0,  0.31519625626968473,  0.50005309467850001,
   0.31519625626968473,  0, -0.096968811873903021,  0,
   0.049363779515950633,  0, -0.027254942768972744,  0,
   0.014675772709985322,  0, -0.0072044221929927947,  0,
   0.0029449330549970549,  0, -0.00077911205399922078,  0,
};
/* Delay line as a CIRCULAR BUFFER: 'w' is the index of the newest sample, so a push only moves
   the index instead of shifting the window.
   The line is stored TWICE, z[i] == z[i + ZC_OS_TAPS], so a window of ZC_OS_TAPS consecutive taps
   starting anywhere in 0..ZC_OS_MASK is CONTIGUOUS and needs no wrap. That removes the & mask from
   every read in conv() below, which is what otherwise stops the compiler from vectorising it. It
   costs one extra store per push and 128 bytes per instance. */
typedef struct { double z[2 * ZC_OS_TAPS]; int w; } ZcFir;
static inline void zc_fir_reset(ZcFir *f) { memset(f->z, 0, sizeof f->z); f->w = 0; }
static inline void zc_fir_push(ZcFir *f, double x) {
  f->w = (f->w - 1) & ZC_OS_MASK; f->z[f->w] = x; f->z[f->w + ZC_OS_TAPS] = x;
}
/* conv = sum_k kernel[k]*z[k]  (z[0] = newest -> z[(w+k)&mask] in the circular buffer). */
static inline double zc_fir_conv(const ZcFir *f) {
  const double *z = f->z + f->w;      /* mirrored buffer: no wrap for the next ZC_OS_TAPS taps */
  double s = 0.0;
  /* Deliberately UNFOLDED. The kernel is a padded half-band whose symmetry is about index 15,
   * with kernel[31] the padding zero, so folding on kernel[k] == kernel[31-k] would be wrong.
   * This is the REFERENCE path, treated by the harnesses in compare/ as the definition of the
   * filter, so it is written for obviousness rather than speed; the plugin runs zc_up2_block. */
  for (int k = 0; k < ZC_OS_TAPS; k++) s += zc_os_kernel[k] * z[k];
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

/* ---- antiderivative anti-aliasing (ADAA) ------------------------------------------------------
 * Instead of evaluating the shaper AT each sample, integrate it BETWEEN consecutive samples:
 *
 *     y[n] = ( H(x[n]) - H(x[n-1]) ) / ( x[n] - x[n-1] ),      H' = h
 *
 * i.e. the average value of the nonlinearity over the segment the signal actually travelled during
 * that sample period. Averaging is a low-pass, so the harmonics that would have folded back are
 * attenuated before they alias -- at the cost of an inherent HALF-SAMPLE delay (an average over
 * [n-1, n] is centred at n-1/2) and a slight softening of the very top end.
 *
 * The shaper is h(x) = f(g*x)/g, so its antiderivative is H(x) = F(g*x)/g^2, with
 *     F(u) = S * ( ln(cosh(K*(u+B)))/K - TB*u ).
 *
 * ln(cosh(v)) is evaluated as |v| + log1p(exp(-2|v|)) - ln2. The direct form overflows for
 * |v| >~ 350, which this drive reaches on a loud input at a high knob setting -- not a theoretical
 * nicety. The stable form costs nothing extra and cannot overflow. */
/* exp() restricted to x in [-20, 0], which is the ONLY range zc_drive_logcosh can hand it, since
   its argument is -2*fabs(v). Range reduction x = k*ln2 + r, degree-7 Taylor for exp(r) with
   |r| <= ln2/2, then scaling by 2^k through the exponent field. Worst relative error over the
   range is -163 dB, and it saves about 11 % of the plugin at drive 2x.
   NOT a general exp: there is no handling of inf, nan, overflow or subnormals. It is safe here
   because k lands in [-29, 0], so the scale factor is always a normal double. That is a property
   of the caller rather than something checked at run time, which is why the domain is stated
   above and pinned by `make valpoly`.
   It only pays once SSE4.1 is available: without it GCC expands floor() into a branchy integer
   round trip and this is about 20 % SLOWER than libm, which is why it sits behind the function
   clones below. */
static inline double zc_fast_exp_neg(double x) {
  const double LOG2E  = 1.4426950408889634;
  const double LN2_HI = 6.93147180369123816490e-01;   /* split so k*ln2 stays exact */
  const double LN2_LO = 1.90821492927058770002e-10;
  double kd = floor(x * LOG2E + 0.5);
  double r  = (x - kd * LN2_HI) - kd * LN2_LO;
  double p  = 1.0 + r*(1.0 + r*(0.5 + r*(1.0/6.0 + r*(1.0/24.0 + r*(1.0/120.0
                   + r*(1.0/720.0 + r*(1.0/5040.0)))))));
  union { double d; uint64_t u; } sc;
  sc.u = (uint64_t)(int64_t)(1023 + (int)kd) << 52;   /* 2^k */
  return p * sc.d;
}
static inline double zc_drive_logcosh(double v) {
  double a = fabs(v);
  /* log1p(t) with t = exp(-2a), evaluated as t*P(t) by a degree-7 Chebyshev fit instead of calling
     libm. log1p and exp together accounted for roughly 40 % of the drive path; replacing both
     saves about 21 % of the whole plugin at drive 2x, for a worst-case deviation of -107.5 dB
     against the libm version over the full control grid.
     Two properties make the fit safe here:
       - a = fabs(v), so t = exp(-2a) lies in (0,1] ALWAYS. The fit covers that entire domain:
         there is no seam, no branch, and no way to ask the polynomial for a value outside it.
       - the ADAA divides a difference of two antiderivative values by their spacing, which would
         amplify a JAGGED error by up to a factor of 1e6. A polynomial's error is SMOOTH, so it
         very nearly cancels in the difference and what survives is its derivative rather than its
         value divided by dx. The measured amplification is a constant ~18 dB.
     Coefficients come from compare/genlogp.py and are checked against libm by compare/valpoly.c
     (`make valpoly`) over 200001 points. Do not hand-edit them: the Horner order is not obvious
     from the values, and getting it wrong shows up in the audio rather than in review. */
  { double t = zc_fast_exp_neg(-2.0 * a);
    return a + t*(0.99999951797449871 + t*(-0.49996354789502573 + t*(0.33265239874731906
              + t*(-0.24453324768369011 + t*(0.17659732263226022 + t*(-0.10679787080809072
              + t*(0.043658514556329284 + t*(-0.0084662531066211043)))))))) - 0.6931471805599453; }
}
static inline double zc_drive_antideriv(double u) {       /* antiderivative of zc_drive_shape */
  /* Multiplying by the reciprocal rather than dividing: without -ffast-math GCC would emit a real
     division here, once per oversampled sample. It is not bit-identical to the division -- the
     reciprocal rounds once -- but the difference is -243 dB worst case over the control grid. */
  return ZC_DRV_S * (zc_drive_logcosh(ZC_DRV_K * (u + ZC_DRV_B)) * (1.0 / ZC_DRV_K) - ZC_DRV_TB * u);
}

typedef struct {
  double g;            /* pre-gain (0 = stage inactive) */
  double r;            /* DC-blocker coeff (~0.998) */
  double x1, y1;       /* DC-blocker state (post-shaper) */
  double ax, aH;       /* ADAA state: previous input and its antiderivative */
} ZcDrive;
static inline void zc_drive_reset(ZcDrive *d) {
  d->x1 = 0.0; d->y1 = 0.0;
  d->ax = 0.0;
  /* guarded: reset can run before set (activate() sees a calloc'd struct with g = 0) */
  d->aH = (d->g > 1e-6) ? zc_drive_antideriv(0.0) / (d->g * d->g) : 0.0;
}
/* Changing the gain requires re-deriving the ADAA state, and skipping that is audible.
 *
 * The ADAA carries H(x[n-1]) as state, and H(x) = F(g*x)/g^2 is scaled by g. Refreshing g alone
 * would make the very next sample compute
 *        s = ( H_new(x[n]) - H_old(x[n-1]) ) / ( x[n] - x[n-1] )
 * which mixes two different scalings and then DIVIDES by dx, tiny at 4x oversampling. A knob move
 * of a few thousandths comes out as a full-scale spike.
 *
 * So the stored antiderivative is re-derived for the new gain from the sample the state already
 * holds. That costs one evaluation per BLOCK, and only when the knob actually moved, against the
 * 4n per block the shaper already does. With the knob still, the output is bit-identical to not
 * doing it at all. */
static inline void zc_drive_set(ZcDrive *d, double knob, double base_fs) {
  double g = 2.0 * knob;                               /* knob 0.5 -> g=1 = nominal intensity */
  d->r = 1.0 - 2.0 * M_PI * 14.62 / base_fs;           /* 1st-order DC-blocker at 14.6 Hz */
  if (g != d->g) {
    d->g = g;
    d->aH = (g > 1e-6) ? zc_drive_antideriv(g * d->ax) / (g * g) : 0.0;
  }
}
static inline double zc_drive_tick(ZcDrive *d, double x) {
  double g = d->g;
  double s = (g > 1e-6) ? zc_drive_shape(g * x) / g : ZC_DRV_FP0 * x;   /* clean g->0 limit */
  double y = s - d->x1 + d->r * d->y1;                  /* y[n] = s[n] - s[n-1] + r*y[n-1] */
  d->x1 = s; d->y1 = y;
  return y;
}

/* Same stage, but with the shaper replaced by its ADAA form. Used only by the oversampled path;
 * the bare 1x path keeps zc_drive_tick so that "Off" stays bit-identical to what shipped.
 *
 * The guard is not optional. As x[n] -> x[n-1] the quotient is 0/0 and blows up in floating point
 * long before it is singular in exact arithmetic -- and a held input (silence, a sustained DC-ish
 * excursion) hits that case constantly. Falling back to the shaper at the MIDPOINT keeps the
 * function continuous across the switch, which is what stops the fallback itself from becoming an
 * audible discontinuity. */
/* ---- the ADAA over a whole block, in TWO passes ------------------------------------------------
 * The shaper is about 65 % of the drive's cost at 4x, and a per-sample tick would mix two halves
 * with very different natures:
 *
 *   H(x[i])            depends ONLY on x[i]. No dependency between samples: vectorisable.
 *   the DC blocker     y[i] = s[i] - s[i-1] + r*y[i-1]. A recurrence: NOT vectorisable, and cheap
 *                      enough (two multiplies) that there would be nothing to win there anyway.
 *
 * As one loop the expensive half runs at the speed of the serial half. Split, the first pass is a
 * straight map over the block that the compiler can vectorise with the same clone-provided ISA as
 * the FIRs.
 *
 * The split does not change a single value: same arithmetic, same order, same branch. What it
 * changes is what the compiler is allowed to do with the first pass.
 *
 * The |dx| <= 1e-6 branch stays scalar and stays exactly where it is. It is rare on real signal
 * but not on silence, where dx is 0 every sample, so it has to stay correct and not merely fast. */
static inline void zc_drive_block_adaa(ZcDrive *restrict d, double *restrict x,
                                       double *restrict hbuf, int n) {
  const double g = d->g;
  if (g <= 1e-6) {                                    /* clean g->0 limit, same as the tick */
    for (int i = 0; i < n; i++) {
      double s = ZC_DRV_FP0 * x[i];
      double y = s - d->x1 + d->r * d->y1;
      d->x1 = s; d->y1 = y; x[i] = y;
    }
    d->ax = n ? x[n - 1] : d->ax; d->aH = 0.0;
    return;
  }
  const double ig2 = 1.0 / (g * g);
  for (int i = 0; i < n; i++) hbuf[i] = zc_drive_antideriv(g * x[i]) * ig2;   /* PASS 1: no deps */
  double ax = d->ax, aH = d->aH, x1 = d->x1, y1 = d->y1, r = d->r;
  for (int i = 0; i < n; i++) {                                              /* PASS 2: recurrence */
    double xi = x[i], dx = xi - ax;
    double s = (fabs(dx) > 1e-6) ? (hbuf[i] - aH) / dx
                                 : zc_drive_shape(g * (0.5 * (xi + ax))) / g;
    ax = xi; aH = hbuf[i];
    double y = s - x1 + r * y1;
    x1 = s; y1 = y; x[i] = y;
  }
  d->ax = ax; d->aH = aH; d->x1 = x1; d->y1 = y1;
}

static inline double zc_drive_tick_adaa(ZcDrive *d, double x) {
  double g = d->g, s;
  if (g > 1e-6) {
    const double ig2 = 1.0 / (g * g);
    double H = zc_drive_antideriv(g * x) * ig2;
    double dx = x - d->ax;
    s = (fabs(dx) > 1e-6) ? (H - d->aH) / dx
                          : zc_drive_shape(g * (0.5 * (x + d->ax))) / g;
    d->ax = x; d->aH = H;
  } else {
    s = ZC_DRV_FP0 * x;                                 /* same clean g->0 limit as the bare path */
    d->ax = x; d->aH = 0.0;
  }
  double y = s - d->x1 + d->r * d->y1;
  d->x1 = s; d->y1 = y;
  return y;
}

/* ---- drive with OPTIONAL oversampling ---------------------------------------------------------
 * The drive shaper is NON-LINEAR and MEMORYLESS, so it generates harmonics above Nyquist that fold
 * back as aliasing: about -14 dB of alias to signal at 9 kHz and maximum drive. Unlike
 * the EQ (linear, no aliasing), the only clean fix is to run the shaper at a higher rate. This wraps
 * the drive in the windowed-sinc 2x FIR cascade above (up -> shaper@fs_os -> down), with a user
 * switch. With os==1 it is BIT-IDENTICAL to the bare zc_drive path (stages=0, no FIR, DC-blocker at
 * base_fs), so "Off" reproduces the plain drive exactly (no regression).
 * With os==2 the shaper inside the oversampled section is the ADAA form, which is what makes this
 * beat the old 4x while costing less than the old 2x. */
/* ================================================================================================
 * THE FAST OVERSAMPLING PATH -- polyphase + portable SIMD
 * ================================================================================================
 * With 32 taps at 4x the FIRs are where the plugin spends its time, and a normal session carries
 * dozens of instances. Two separate things make the reference path above expensive (it is kept
 * regardless, because the harnesses in compare/ use it as the definition of the filter):
 *
 *   1. THE UPSAMPLER MULTIPLIES BY ZEROS. It pushes a real sample, convolves 32 taps, pushes a
 *      literal 0.0, and convolves 32 taps again. Half of every convolution is therefore
 *      multiplying coefficients by inserted zeros. The polyphase form below splits the kernel into
 *      its even and odd coefficient sets and runs each over the REAL samples only: the same terms,
 *      none of the zeros, and no folding sums.
 *   2. THE REDUCTION DOES NOT VECTORISE. `s += k[i]*z[i]` accumulating into a scalar cannot be
 *      reassociated by the compiler without -fassociative-math, which this build does not use and
 *      must not, so it stays one FMA at a time chained on the accumulator. Below, the reduction is
 *      written explicitly with GCC vector extensions and TWO accumulators, which breaks that
 *      dependency chain.
 *
 * VECTOR EXTENSIONS RATHER THAN <immintrin.h>, and that is the point. zc_driveos_process carries
 *   target_clones("default","avx2","arch=x86-64-v3"), so this source is compiled once per clone:
 *   the avx2 clone gets 4-wide ymm FMA and the default clone gets 2-wide SSE2 from the same lines.
 *   AVX2 intrinsics here would pin the whole plugin to a CPU that has AVX2.
 *
 * This path is NOT bit-identical to the reference one, deliberately: the terms are the same, the
 *   ORDER of the additions is not. compare/fir_fast_check.c pins the difference at rounding level
 *   and has a negative leg.
 *
 * The two polyphase tables are DERIVED from zc_os_kernel at prepare() time, never written out by
 *   hand, so there is only ever one copy of each number.
 */
#define ZC_OS_HALF (ZC_OS_TAPS / 2)
/* THE INNER STAGE OF THE CASCADE DOES NOT NEED THE SAME FILTER.
 * Going 1x -> 2x the images sit right next to the signal, so that stage needs the long kernel
 * above. Going 2x -> 4x the signal is ALREADY limited to the outer Nyquist, the transition band is
 * enormous, and a much shorter half-band does the same job. The same holds coming down: the
 * 4x -> 2x stage is the cheap one and the 2x -> 1x stage is the one that has to be good.
 *
 * Inner-kernel length against alias audibility (ANMR of the worst frame at 4x, two independent
 * hard materials; criterion < -10 dB):
 *      31 taps inner       -15.27   -16.22
 *      15 taps inner       -14.92   -16.13
 *      11 taps inner       -17.84   -18.30      <- in use: better, and cheaper
 *       7 taps inner        -7.49    -8.45      too short, misses the criterion outright
 *
 * 11-tap Kaiser(4.0) half-band, padded with zeros to 16 so the dot product stays a multiple of 8. */
#define ZC_IN_TAPS 16
static const double zc_in_kernel[ZC_IN_TAPS] = {
   0.0056490133399943517,  0, -0.054110083442945899,  0,
   0.2977434885817023,  0.50143516304249858,  0.2977434885817023,  0,
  -0.054110083442945899,  0,  0.0056490133399943517,  0,
   0,  0,  0,  0,
};

/* Every FIR here reads a window that starts BEFORE the block, so each scratch buffer carries this
 * many doubles of run-up in front of the signal. ZC_OS_TAPS covers the deepest window (the
 * decimator's 31 samples of history) with room to spare. */
#define ZC_OS_PAD  ZC_OS_TAPS

typedef double zc_v4 __attribute__((vector_size(4 * sizeof(double)), aligned(8)));

/* dot product of n doubles (n a multiple of 8), two accumulators, unaligned loads.
 * TWO accumulators on purpose: one would serialise the whole reduction on its own latency. */
static inline double zc_dot(const double *restrict h, const double *restrict z, int n) {
  zc_v4 a0 = {0.0, 0.0, 0.0, 0.0}, a1 = {0.0, 0.0, 0.0, 0.0};
  for (int k = 0; k < n; k += 8) {
    zc_v4 h0, h1, z0, z1;
    __builtin_memcpy(&h0, h + k,     sizeof h0);
    __builtin_memcpy(&z0, z + k,     sizeof z0);
    __builtin_memcpy(&h1, h + k + 4, sizeof h1);
    __builtin_memcpy(&z1, z + k + 4, sizeof z1);
    a0 += h0 * z0;
    a1 += h1 * z1;
  }
  zc_v4 a = a0 + a1;
  return (a[0] + a[1]) + (a[2] + a[3]);
}

/* Coefficients in the order the windows are read: TIME-REVERSED, so the convolution is a straight
 * dot product over ascending memory. All DERIVED from zc_os_kernel, never hand-written.
 *   her/hor : the even/odd polyphase sets of the interpolator, pre-scaled by its 2.0
 *   kr      : the whole kernel, for the decimator */
typedef struct {
  double her[ZC_OS_HALF];    /* the interpolator's EVEN phase, time-reversed, pre-scaled by 2 */
  double kr[ZC_OS_TAPS];     /* the whole kernel, time-reversed, for the decimator */
  double cdel;               /* the interpolator's ODD phase: ONE coefficient (half-band) */
  int H, T, cj;              /* half-length, length, and where the delay reads in the window */
} ZcOsPhases;

/* Build the polyphase tables for ONE stage from ONE kernel. Everything is derived from the kernel
 * that is passed in. */
static inline void zc_os_phases_from(ZcOsPhases *p, const double *k, int T) {
  const int H = T / 2;
  p->T = T; p->H = H;
  double he[ZC_OS_HALF];
  for (int j = 0; j < H; j++) he[j] = 2.0 * k[2 * j];
  /* A SEPARATE loop: reversing inside the one above would read entries not written yet. */
  for (int j = 0; j < H; j++) p->her[j] = he[H - 1 - j];
  for (int i = 0; i < T; i++) p->kr[i] = k[T - 1 - i];
  /* The centre tap is the ONLY odd-indexed non-zero one in a half-band. Find it rather than assume
   * it, and poison the tables if the kernel is not one -- a non-half-band kernel would make the
   * single-coefficient odd phase silently wrong, and no null would say so. */
  int c = -1, odd_nonzero = 0;
  for (int i = 1; i < T; i += 2) if (k[i] != 0.0) { c = i; odd_nonzero++; }
  if (odd_nonzero != 1) { p->cdel = 0.0 / 0.0; p->cj = 0; return; }   /* NaN: the gates catch it */
  p->cdel = 2.0 * k[c];
  p->cj   = (H - 1) - (c - 1) / 2;
}

/* State is the tail of the previous block rather than a circular buffer, and the difference is
 * large. A circular delay line writes the newest sample and immediately reads a window that
 * overlaps that store, so every sample pays a store-to-load forwarding stall. With the same terms,
 * the same kernel and the same SIMD, overlap-save runs the interpolator at 2.58 ns per input
 * sample against 11.09 ns, a factor of 4.3. The multiply count is not what dominates here. */
typedef struct { double hist[ZC_OS_HALF]; } ZcUpState;   /* ZC_OS_HALF-1 used */
typedef struct { double hist[ZC_OS_TAPS]; } ZcDnState;   /* ZC_OS_TAPS-1 used */

static inline void zc_upstate_reset(ZcUpState *f) { memset(f->hist, 0, sizeof f->hist); }
static inline void zc_dnstate_reset(ZcDnState *f) { memset(f->hist, 0, sizeof f->hist); }

/* One 2x interpolation stage over a whole block. src[0..len-1] is the signal and src[-(HALF-1)..-1]
 * is writable run-up (the caller guarantees ZC_OS_PAD doubles in front). Writes 2*len samples. */
/* THE LENGTH HAS TO BE A COMPILE-TIME CONSTANT. With p->H passed as a runtime argument the loop
 * stops being fully unrolled, and the short inner filter then runs 2.2 % SLOWER than the long one
 * it replaces despite having a third fewer multiplies. So the body is a template over H and the
 * dispatch happens once per block, where an unpredictable branch costs nothing. */
static inline void zc_up2_body(ZcUpState *restrict f, const ZcOsPhases *restrict p,
                               double *restrict src, double *restrict dst, int len, const int H) {
  for (int j = 0; j < H - 1; j++) src[j - (H - 1)] = f->hist[j];
  const double cdel = p->cdel; const int cj = p->cj;
  for (int i = 0; i < len; i++) {
    const double *restrict w = src + i - (H - 1);      /* w[H-1] is x[i] */
    dst[2 * i]     = zc_dot(p->her, w, H);             /* even phase */
    dst[2 * i + 1] = cdel * w[cj];                     /* odd phase: ONE tap -- the half-band delay */
  }
  for (int j = 0; j < H - 1; j++) f->hist[j] = src[len - (H - 1) + j];
}
static inline void zc_up2_block(ZcUpState *restrict f, const ZcOsPhases *restrict p,
                                double *restrict src, double *restrict dst, int len) {
  if (p->H == ZC_OS_TAPS / 2) zc_up2_body(f, p, src, dst, len, ZC_OS_TAPS / 2);
  else                        zc_up2_body(f, p, src, dst, len, ZC_IN_TAPS / 2);
}

/* One /2 decimation stage. Reads 2*len samples from src (with run-up in front), writes len. */
static inline void zc_dn2_body(ZcDnState *restrict f, const ZcOsPhases *restrict p,
                               double *restrict src, double *restrict dst, int len, const int T) {
  for (int j = 0; j < T - 1; j++) src[j - (T - 1)] = f->hist[j];
  for (int i = 0; i < len; i++) {
    /* the reference path pushes src[2i] then src[2i+1] and convolves, so the newest sample of the
     * window is src[2i+1] and the window is the T samples ending there. */
    const double *restrict w = src + 2 * i + 1 - (T - 1);
    dst[i] = zc_dot(p->kr, w, T);
  }
  for (int j = 0; j < T - 1; j++) f->hist[j] = src[2 * len - (T - 1) + j];
}
static inline void zc_dn2_block(ZcDnState *restrict f, const ZcOsPhases *restrict p,
                                double *restrict src, double *restrict dst, int len) {
  if (p->T == ZC_OS_TAPS) zc_dn2_body(f, p, src, dst, len, ZC_OS_TAPS);
  else                    zc_dn2_body(f, p, src, dst, len, ZC_IN_TAPS);
}

/* TWO stages, i.e. up to 4x, and the second one is structural rather than a refinement.
 *
 * At 2x there is a floor near -3 dB of alias audibility that no filter length reaches: lengthening
 * the decimator from 32 to a deliberately oversized 64 taps moves it only from -2.24 to -2.93 dB.
 * The reason is that the folding happens INSIDE the oversampled section. At 2x the internal
 * Nyquist is 48 kHz, so the 5th harmonic of an 18 kHz partial lands at 90 kHz and comes back at
 * 6 kHz, in the middle of the audible band, where no decimation filter can act on it. Only raising
 * the internal rate moves it: with 32 taps, 4x measures -12.27 dB against -2.24 dB at 2x, on a
 * criterion of < -10 dB.
 *
 * Measuring this on a single high-frequency sine is misleading, because a short FIR's own leakage
 * becomes the ceiling and hides the difference. It has to be judged on material with real energy
 * near Nyquist. Full tables: docs/MEDICIONES_HANDOFF.md. */
#define ZC_DRV_MAX_STAGES 2   /* 2^2 = 4x maximum for the drive */

/* Block size the engine chunks to, and the scratch that implies. They live HERE, with the engine
 * that dictates them, rather than in lv2.c, so that everything that needs the figure -- the plugin
 * and the memory report in compare/ -- reads one definition. */
#define ZC_MAXBLK 1024
/* + ZC_OS_PAD twice: the oversampler's FIRs are overlap-save, so each buffer carries run-up in
 * front of the signal AND the caller's pointer is offset into it. Derived, not a magic number. */
#define ZC_SCRATCH_LEN (ZC_MAXBLK * (1 << ZC_DRV_MAX_STAGES) + 2 * ZC_OS_PAD)
typedef struct {
  ZcDrive drv;                                   /* shaper + DC-blocker, coeffs set at fs_os */
  int os, stages;                                /* 1, 2 or 4 ; log2(os) */
  ZcUpState up[ZC_DRV_MAX_STAGES];               /* just the block tail, no circular buffer */
  ZcDnState dn[ZC_DRV_MAX_STAGES];
  /* [0] = the OUTER stage's filter (the long half-band), [1] = the INNER one (the short one).
   * At 2x only [0] is ever used. Derived in zc_driveos_set. */
  ZcOsPhases ph[2];
} ZcDriveOS;

static inline void zc_driveos_reset(ZcDriveOS *d) {
  zc_drive_reset(&d->drv);
  for (int s = 0; s < d->stages; s++) { zc_upstate_reset(&d->up[s]); zc_dnstate_reset(&d->dn[s]); }
}
/* set gain/DC-blocker for the (possibly oversampled) rate; recomputes os/stages. Does NOT reset the
 * FIR/DC state -> safe for a live knob move (same os). Caller resets when os changes or on engage. */
static inline void zc_driveos_set(ZcDriveOS *d, double knob, double base_fs, int os) {
  if (os > 4) os = 4;                            /* 4x is the maximum; see ZC_DRV_MAX_STAGES above */
  d->os = os; d->stages = 0; for (int o = os; o > 1; o >>= 1) d->stages++;
  zc_os_phases_from(&d->ph[0], zc_os_kernel, ZC_OS_TAPS);   /* outer: long half-band */
  zc_os_phases_from(&d->ph[1], zc_in_kernel, ZC_IN_TAPS);   /* inner: short half-band */
  zc_drive_set(&d->drv, knob, base_fs * os);     /* DC-blocker frequency tracks fs_os */
}
/* process a block: upsample (2x cascade) -> memoryless shaper@fs_os -> downsample. os==1 = bare path.
 * sa/sb = ping-pong scratch, each >= n*os doubles. */
/* restrict: the scratch buffers sa/sb belong to the instance and never alias in/out, but the
   compiler cannot prove that, so without it it reloads d->g and d->r inside the ADAA loop and
   cannot hoist the invariant reciprocal out. Worth about 5 % of the plugin at drive 2x, and
   bit-identical.
   The dispatch has to be HERE and not only on run(): this function is its own symbol and is not
   inlined into run()'s clones, so without its own target_clones the whole drive would run at the
   baseline ISA even on a machine with AVX2. It is called once per block, so the indirect call
   costs nothing at this boundary; per sample it would cost more than it buys by blocking
   inlining. */
__attribute__((target_clones("default","avx2","arch=x86-64-v3")))
static void zc_driveos_process(ZcDriveOS *restrict d, const float *restrict in,
                                       float *restrict out, int n,
                                       double *restrict sa, double *restrict sb) {
  if (d->os == 1) { for (int i = 0; i < n; i++) out[i] = (float)zc_drive_tick(&d->drv, in[i]); return; }
  /* Both scratch buffers are used from ZC_OS_PAD onwards: every FIR below reads a window that
   * starts before the block, and that run-up is written in place. The caller sizes them for it. */
  double *cur = sa + ZC_OS_PAD, *nxt = sb + ZC_OS_PAD; int len = n;
  for (int i = 0; i < n; i++) cur[i] = in[i];
  for (int s = 0; s < d->stages; s++) {          /* upsample: s = 0 is the OUTER stage */
    zc_up2_block(&d->up[s], &d->ph[s == 0 ? 0 : 1], cur, nxt, len);
    len *= 2; double *t = cur; cur = nxt; nxt = t;
  }
  /* ADAA shaper at fs_os. The 1x branch above deliberately keeps the plain zc_drive_tick, so
   * "Off" is bit-identical to the non-oversampled drive rather than merely close to it. */
  zc_drive_block_adaa(&d->drv, cur, nxt, len);   /* nxt is free here: used as the H scratch */
  for (int s = 0; s < d->stages; s++) {          /* downsample: the LAST stage is the OUTER one */
    int half = len / 2;
    zc_dn2_block(&d->dn[s], &d->ph[s == d->stages - 1 ? 0 : 1], cur, nxt, half);
    len = half; double *t = cur; cur = nxt; nxt = t;
  }
  for (int i = 0; i < n; i++) out[i] = (float)cur[i];
}

/* Latency (samples, at the base rate) the drive oversampling adds: the group delay of the up/down FIR
 * round-trip for a given os. The shaper is memoryless (0 delay), so this IS the whole drive-OS latency.
 * os==1 -> 0. It is obtained from an impulse through the FIR cascade only, with no shaper.
 * This is the FIR round trip ONLY. When the oversampled path runs, the ADAA shaper adds its own
 * half-sample delay AT THE OVERSAMPLED RATE, i.e. 0.5/os samples at the base rate;
 * zc_driveos_latency_total below adds it. At 2x that is 7.0 + 0.25 = 7.25. */
static inline float zc_driveos_latency(int os) {
  if (os <= 1) return 0.0f;
  int stages = 0; for (int o = os; o > 1; o >>= 1) stages++;
  /* This runs the SAME polyphase path the plugin runs, not the reference one, so that any skew
   * the polyphase form ever picked up would show in the reported latency instead of being hidden
   * by measuring a twin. */
  ZcUpState up[ZC_DRV_MAX_STAGES]; ZcDnState dn[ZC_DRV_MAX_STAGES];
  ZcOsPhases ph[2];
  zc_os_phases_from(&ph[0], zc_os_kernel, ZC_OS_TAPS);
  zc_os_phases_from(&ph[1], zc_in_kernel, ZC_IN_TAPS);
  for (int s = 0; s < stages; s++) { zc_upstate_reset(&up[s]); zc_dnstate_reset(&dn[s]); }
  enum { N = 256 };
  static double a[N * 4 + 2 * ZC_OS_PAD], b[N * 4 + 2 * ZC_OS_PAD];
  memset(a, 0, sizeof a); memset(b, 0, sizeof b);
  double *cur = a + ZC_OS_PAD, *nxt = b + ZC_OS_PAD; int len = N;
  for (int i = 0; i < N; i++) cur[i] = (i == 0) ? 1.0 : 0.0;
  for (int s = 0; s < stages; s++) {
    zc_up2_block(&up[s], &ph[s == 0 ? 0 : 1], cur, nxt, len);
    len *= 2; double *t = cur; cur = nxt; nxt = t;
  }
  for (int s = 0; s < stages; s++) {
    int half = len / 2;
    zc_dn2_block(&dn[s], &ph[s == stages - 1 ? 0 : 1], cur, nxt, half);
    len = half; double *t = cur; cur = nxt; nxt = t;
  }
  double num = 0, den = 0;
  for (int i = 0; i < len; i++) { double e = cur[i] * cur[i]; num += i * e; den += e; }
  return den > 0 ? (float)(num / den) : 0.0f;
}

/* Total latency the drive stage adds, in samples at the BASE rate.
 * = FIR round trip + the ADAA half sample, expressed at the base rate.
 * At 2x it is 7.25 and at 4x 16.93, i.e. NOT integer, which is why the latency port is not
 * declared as an integer. Cross-checked against a direct measurement of the whole path: a residual
 * minimisation against a 32x reference puts the 2x delay at exactly 7.25. */
static inline float zc_driveos_latency_total(int os) {
  if (os <= 1) return 0.0f;
  return zc_driveos_latency(os) + 0.5f / (float)os;
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

/* Copies the DESIGN (coefficients and degrees) from one chain to another, and deliberately NOT
 * the z[] state.
 *
 * zc_chain_fill is a pure function of (ct, fs) and both channels are always handed the SAME ct, so
 * designing twice produces bit-identical coefficients. Designing once and copying therefore halves
 * the cost for exactly zero difference in output, and that cost is not small: a full chain design
 * takes about 19.7 ms, which is over seven times a 256-sample block's budget at 48 kHz.
 *
 * The state must NOT come along. z[] is the filter's memory of the audio that channel has heard,
 * so copying it would inject the left channel's history into the right one. */
static inline void zc_chain_copy_design(ZcChain *dst, const ZcChain *src) {
  dst->nsec = src->nsec;
  dst->fs   = src->fs;
  for (int i = 0; i < src->nsec; i++) {
    dst->sec[i].nb = src->sec[i].nb;
    dst->sec[i].na = src->sec[i].na;
    for (int k = 0; k <= ZC_MAX_NB; k++) dst->sec[i].b[k] = src->sec[i].b[k];
    for (int k = 0; k <= ZC_MAX_NA; k++) dst->sec[i].a[k] = src->sec[i].a[k];
  }
}

/* Processes a block while RAMPING the coefficients from one design to another.
 *
 * THE PROBLEM. The designer runs once per block, so a moving knob makes the response advance in
 * steps whose size depends on the host's buffer. Against the finest render the plugin can produce
 * that is -53 dB of residual at a 64-sample block, -35.6 dB at 512 and -29.5 dB at 1024, scaling
 * at about 6 dB per doubling exactly as a step-size argument predicts.
 *
 * Redesigning every 16 to 32 samples would fix it and is not affordable: one section design costs
 * about 0.39 ms, so redesigning every 16 samples would be roughly 1.2 seconds of CPU per second of
 * audio.
 *
 * Instead this uses the two designs it ALREADY has -- the previous one and this block's -- and
 * walks between them in a few sub-steps, delivering the same total change in K smaller steps. No
 * extra designs, and the cost is a handful of interpolations per sub-step, paid only on blocks
 * where a control actually moved.
 *
 * Note that this INTERPOLATES a[], the denominator. In general nothing about the magnitude of an
 * interpolated filter guarantees its poles stay inside the unit circle; it is defensible here only
 * because the two endpoints are one block apart, so the path between them is short. Since that is
 * an argument rather than evidence, compare/ramp_stability.c checks the pole radius of every
 * interpolated set produced by a fast sweep of every control. */
static inline void zc_chain_process_ramp(ZcChain *c, const ZcChain *from, const ZcChain *to,
                                         const float *in, float *out, int n, int steps) {
  if (steps < 1) steps = 1;
  int done = 0;
  for (int s = 0; s < steps && done < n; s++) {
    const double t = (double)(s + 1) / (double)steps;
    for (int i = 0; i < c->nsec; i++) {
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

/* LIVE refresh that only redesigns the sections whose controls actually moved.
 *
 * The structure is unchanged here (the caller checks the signature first), so the section order is
 * fixed and each section maps to a known slice of the controls. A section whose inputs did not
 * change would be handed the same (ct, fs) and would produce the same coefficients it already
 * holds, so skipping it is bit-identical -- not an approximation.
 *
 * In the common case a user moves ONE knob, so this designs one section instead of six. */
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
   caller still zeroes it, exactly as zc_chain_build did. */
static inline void zc_chain_rebuild_reuse(ZcChain *c, const ZcChain *old, double base_fs,
                                          const ZcControls *ct, const ZcControls *prev) {
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
    if (os >= 0 && os < old->nsec) {
      if (slot == 0)      same = (ct->hp_freq == prev->hp_freq && ct->hp_bump == prev->hp_bump);
      else if (slot == 5) same = (ct->lp_freq == prev->lp_freq && ct->lp_resonant == prev->lp_resonant);
      else                same = !zc_band_changed(ct, prev, slot - 1);
    }
    if (same) {                                  /* reuse: copy the coefficients, not the state */
      out[n].nb = old->sec[os].nb; out[n].na = old->sec[os].na;
      for (int k = 0; k <= ZC_MAX_NB; k++) out[n].b[k] = old->sec[os].b[k];
      for (int k = 0; k <= ZC_MAX_NA; k++) out[n].a[k] = old->sec[os].a[k];
    } else if (slot == 0) {
      double fc = zc_interp(ct->hp_freq, ZC_HP_NORM, ZC_HP_HZ, ZC_ARRLEN(ZC_HP_NORM));
      zc_section_build_filter(&out[n], ZC_HP, fc, ct->hp_bump ? ZC_HP_Q : ZC_HP_Q_FLAT, fs);
    } else if (slot == 5) {
      double fc = zc_interp(ct->lp_freq, ZC_LP_NORM, ZC_LP_HZ, ZC_ARRLEN(ZC_LP_NORM));
      zc_section_build_filter(&out[n], ZC_LP, fc,
                              ct->lp_resonant ? ZC_LP_Q_RESONANT : ZC_LP_Q_CLEAN, fs);
    } else {
      int b = slot - 1;
      if ((b == 0 || b == 3) && ct->band[b].shelf) {
        ZcKind kind = (b == 3) ? ZC_HISHELF : ZC_LOSHELF;
        zc_section_build_bellshelf(&out[n], kind,
              (b == 3) ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF,
              (b == 3) ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF,
              (b == 3) ? ZC_CAP_HI_SHELF  : ZC_CAP_LO_SHELF,
              ct->band[b].freq, ct->band[b].gain, fs);
      } else {
        zc_section_build_bellshelf(&out[n], ZC_BELL, zc_ftap_band(b), zc_wtap_band(b),
              zc_cap_band(b), ct->band[b].freq, ct->band[b].gain, fs);
      }
    }
    n++;
  }
  c->nsec = n;
  for (int i = 0; i < n; i++) {
    c->sec[i].nb = out[i].nb; c->sec[i].na = out[i].na;
    for (int k = 0; k <= ZC_MAX_NB; k++) c->sec[i].b[k] = out[i].b[k];
    for (int k = 0; k <= ZC_MAX_NA; k++) c->sec[i].a[k] = out[i].a[k];
  }
  for (int i = 0; i < n; i++) zc_section_reset(&c->sec[i]);
}

#include "grid.h"

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
   * their OLD coefficients indefinitely. compare/rr_converge.c is the check for that. */
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
      double fc = zc_interp(ct->hp_freq, ZC_HP_NORM, ZC_HP_HZ, ZC_ARRLEN(ZC_HP_NORM));
      zc_section_build_filter(&c->sec[si], ZC_HP, fc,
                              ct->hp_bump ? ZC_HP_Q : ZC_HP_Q_FLAT, fs);
      prev->hp_freq = ct->hp_freq; prev->hp_bump = ct->hp_bump;
    } else if (sl == 5) {
      double fc = zc_interp(ct->lp_freq, ZC_LP_NORM, ZC_LP_HZ, ZC_ARRLEN(ZC_LP_NORM));
      zc_section_build_filter(&c->sec[si], ZC_LP, fc,
                              ct->lp_resonant ? ZC_LP_Q_RESONANT : ZC_LP_Q_CLEAN, fs);
      prev->lp_freq = ct->lp_freq; prev->lp_resonant = ct->lp_resonant;
    } else {
      int b = sl - 1;
      int shelf = (b == 0 || b == 3) && ct->band[b].shelf;
      if (grid) {                                  /* O(1): interpolate, and mark it as approximate */
        int which = shelf ? ((b == 3) ? ZC_GB_HISHELF : ZC_GB_LOSHELF) : b;
        zc_grid_lookup(&grid->band[which], ct->band[b].freq, ct->band[b].gain, &c->sec[si]);
        if (approx) *approx |= 1u << si;
      } else if (shelf) {
        ZcKind kind = (b == 3) ? ZC_HISHELF : ZC_LOSHELF;
        const float *ft = (b == 3) ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF;
        const float *wt = (b == 3) ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF;
        const double *cap = (b == 3) ? ZC_CAP_HI_SHELF : ZC_CAP_LO_SHELF;
        zc_section_build_bellshelf(&c->sec[si], kind, ft, wt, cap,
                                   ct->band[b].freq, ct->band[b].gain, fs);
      } else {
        zc_section_build_bellshelf(&c->sec[si], ZC_BELL,
              zc_ftap_band(b), zc_wtap_band(b), zc_cap_band(b),
              ct->band[b].freq, ct->band[b].gain, fs);
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
      if ((b == 0 || b == 3) && ct->band[b].shelf) {
        ZcKind kind = (b == 3) ? ZC_HISHELF : ZC_LOSHELF;
        const float *ft = (b == 3) ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF;
        const float *wt = (b == 3) ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF;
        const double *cap = (b == 3) ? ZC_CAP_HI_SHELF : ZC_CAP_LO_SHELF;
        zc_section_build_bellshelf(&c->sec[s], kind, ft, wt, cap,
                                   ct->band[b].freq, ct->band[b].gain, fs);
      } else {
        zc_section_build_bellshelf(&c->sec[s], ZC_BELL,
              zc_ftap_band(b), zc_wtap_band(b), zc_cap_band(b),
              ct->band[b].freq, ct->band[b].gain, fs);
      }
      *approx &= ~(1u << s);
      return 1;
    }
    s++;
  }
  *approx = 0;                                   /* nothing matched: do not spin forever */
  return 0;
}

static inline void zc_chain_update_selective(ZcChain *c, const ZcControls *ct,
                                             const ZcControls *prev) {
  double fs = c->fs;
  int s = 0;
  if (ct->hp_on) {
    if (ct->hp_freq != prev->hp_freq || ct->hp_bump != prev->hp_bump) {
      double fc = zc_interp(ct->hp_freq, ZC_HP_NORM, ZC_HP_HZ, ZC_ARRLEN(ZC_HP_NORM));
      double q = ct->hp_bump ? ZC_HP_Q : ZC_HP_Q_FLAT;
      zc_section_build_filter(&c->sec[s], ZC_HP, fc, q, fs);
    }
    s++;
  }
  for (int b = 0; b < 4; b++) {
    if (!ct->band[b].on) continue;
    if (zc_band_changed(ct, prev, b)) {
      if ((b == 0 || b == 3) && ct->band[b].shelf) {
        ZcKind kind = (b == 3) ? ZC_HISHELF : ZC_LOSHELF;
        const float *ft = (b == 3) ? ZC_FTAP_HI_SHELF : ZC_FTAP_LO_SHELF;
        const float *wt = (b == 3) ? ZC_WTAP_HI_SHELF : ZC_WTAP_LO_SHELF;
        const double *cap = (b == 3) ? ZC_CAP_HI_SHELF : ZC_CAP_LO_SHELF;
        zc_section_build_bellshelf(&c->sec[s], kind, ft, wt, cap,
                                   ct->band[b].freq, ct->band[b].gain, fs);
      } else {
        zc_section_build_bellshelf(&c->sec[s], ZC_BELL,
              zc_ftap_band(b), zc_wtap_band(b), zc_cap_band(b),
              ct->band[b].freq, ct->band[b].gain, fs);
      }
    }
    s++;
  }
  if (ct->lp_on) {
    if (ct->lp_freq != prev->lp_freq || ct->lp_resonant != prev->lp_resonant) {
      double fc = zc_interp(ct->lp_freq, ZC_LP_NORM, ZC_LP_HZ, ZC_ARRLEN(ZC_LP_NORM));
      double Q = ct->lp_resonant ? ZC_LP_Q_RESONANT : ZC_LP_Q_CLEAN;
      zc_section_build_filter(&c->sec[s], ZC_LP, fc, Q, fs);
    }
    s++;
  }
}

#endif /* ZC_DSP_H */
