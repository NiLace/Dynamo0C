/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* drive.h -- the channel drive: its static curve (from a three-number spec), antiderivative anti-aliasing, and the
 * oversampling cascade (two half-band stages, polyphase, overlap-save; the plugin runs it at 4x), with its latency.
 * The engine also takes 1x and 2x, which only the measurement harness uses as references. Header-only. */
#ifndef ZC_DRIVE_H
#define ZC_DRIVE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- drive oversampling FIR (windowed-sinc 2x, cascade)
 * Used ONLY by the DRIVE (below): its non-linear shaper aliases, so it is run at 2x/4x and this
 * half-band FIR does the up/down sampling. The EQ is analog-matched and never oversamples. */
/* Half-band low-pass kernel for the drive's oversampling: 31 taps, Kaiser(beta = 5.0), cutoff
 * exactly fs_os/4, DC gain 1, padded with one trailing zero to 32 so the direct-form reference convolution keeps a
 * power-of-two buffer.
 *
 * In a half-band every tap at an even offset from the centre is exactly zero, which here makes all
 * the ODD indices zero except the centre. In polyphase form one phase of the interpolator
 * collapses to a single coefficient -- a pure delay -- so 32 multiplies per input sample become
 * 16 + 1. Those zeros are structural rather than a fitting accident, and its generator (genhalfband.py, a development tool)
 * refuses any length that is not 4m+3 rather than quietly designing a worse filter.
 *
 * The symmetry is around index 15, NOT 15.5: kernel[k] == kernel[30-k] with kernel[31] == 0, so
 * kernel[k] == kernel[31-k] is false and a convolution folded on that identity would compute the
 * wrong thing. The reference convolution is unfolded for exactly this reason.
 *
 * Group delay is 15 samples at the oversampled rate. Nothing here hardcodes the resulting plugin
 * latency; it is derived from the oversampling factor where it is reported.
 *
 * Regenerate with that generator, which pins the symmetry after normalising. Do not
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

/* ----------------------------------------------------------------- channel drive (input stage)
 * ASYMMETRIC memoryless shaper (console/transformer style) BOUNDED: a SKEWED tanh that saturates cleanly.
 * Its constants are GENERATED from a three-number spec by a development generator (gen_drive.py: +ceiling -9.28 dBFS,
 * -ceiling -6.10 dBFS, small-signal slope 0.978), and a development test fails if these drift from it. Followed
 * by a 1st-order DC-blocker at ~14.6 Hz (removes the offset introduced by the asymmetry). Memoryless.
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
/* exp() restricted to x in [-36, 0], which is the ONLY range zc_drive_logcosh can hand it: its
   argument is -2*fabs(v), and zc_drive_logcosh returns early for fabs(v) > 18. Range reduction
   x = k*ln2 + r, degree-7 Taylor for exp(r) with |r| <= ln2/2, then scaling by 2^k through the
   exponent field. Worst relative error over the range is -163 dB, and it saves about 11 % of the
   plugin at drive 2x.
   NOT a general exp: there is no handling of inf, nan, overflow or subnormals. It is safe here
   because k lands in [-52, 0], so the scale factor is always a normal double. That is a property
   of the caller rather than something checked at run time, which is why the domain is stated
   here and the caller's early return is what keeps it.
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
#ifndef ZC_NO_LOGCOSH_GUARD
  /* Past |v| = 18, t = exp(-2a) < 2.4e-16 and a + t*P(t) rounds to a exactly, so returning a - ln2
   * is not an approximation: it is the same double. It is also what keeps zc_fast_exp_neg inside
   * its domain. Without it a loud input (about +37 dBFS into the oversampled drive at full knob)
   * hands it x < -708, the 2^k it builds in the exponent field wraps, and the drive emits a
   * non-finite value that its DC blocker then keeps for ever. ZC_NO_LOGCOSH_GUARD restores that
   * (the negative arm of make abuse). */
  if (a > 18.0) return a - 0.6931471805599453;
#endif
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
     Coefficients come from a development generator (genlogp.py) and are checked against libm by a development
     test (valpoly.c) over 200001 points. Do not hand-edit them: the Horner order is not obvious
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
  double cu, cs;       /* the ADAA fallback's last argument and value (zc_drive_fallback) */
} ZcDrive;
static inline void zc_drive_reset(ZcDrive *d) {
  d->x1 = 0.0; d->y1 = 0.0;
  d->ax = 0.0;
  d->cu = NAN; d->cs = 0.0;
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
 * the bare 1x path (a harness reference) keeps zc_drive_tick, the plain shaper.
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
/* The ADAA's |dx| <= 1e-6 branch: the plain shaper at the midpoint. On silence or a held input that argument repeats
 * every oversampled sample, so the last value is kept and reused (the same double: the output is bit-identical). Out of
 * line and cold, so that the cache costs the signal path nothing. */
__attribute__((noinline, cold)) static double zc_drive_fallback(ZcDrive *d, double u) {
  if (u != d->cu) { d->cu = u; d->cs = zc_drive_shape(d->g * u) / d->g; }
  return d->cs;
}
static inline void zc_drive_block_adaa(ZcDrive *restrict d, double *restrict x,
                                       double *restrict hbuf, int n) {
  const double g = d->g;
  if (g <= 1e-6) {                                    /* clean g->0 limit, same as the tick */
    const double last_in = n ? x[n - 1] : d->ax;      /* the ADAA state is the last INPUT, read
                                                       * before the loop overwrites x with y */
    for (int i = 0; i < n; i++) {
      double s = ZC_DRV_FP0 * x[i];
      double y = s - d->x1 + d->r * d->y1;
      d->x1 = s; d->y1 = y; x[i] = y;
    }
    d->ax = last_in; d->aH = 0.0;
    return;
  }
  const double ig2 = 1.0 / (g * g);
  for (int i = 0; i < n; i++) hbuf[i] = zc_drive_antideriv(g * x[i]) * ig2;   /* PASS 1: no deps */
  double ax = d->ax, aH = d->aH, x1 = d->x1, y1 = d->y1, r = d->r;
  for (int i = 0; i < n; i++) {                                              /* PASS 2: recurrence */
    double xi = x[i], dx = xi - ax;
    double s = (fabs(dx) > 1e-6) ? (hbuf[i] - aH) / dx
                                 : zc_drive_fallback(d, 0.5 * (xi + ax));
    ax = xi; aH = hbuf[i];
    double y = s - x1 + r * y1;
    x1 = s; y1 = y; x[i] = y;
  }
  d->ax = ax; d->aH = aH; d->x1 = x1; d->y1 = y1;
}

/* ---- drive with OPTIONAL oversampling ---------------------------------------------------------
 * The drive shaper is NON-LINEAR and MEMORYLESS, so it generates harmonics above Nyquist that fold
 * back as aliasing (audible on bright material at high drive without oversampling). Unlike
 * the EQ (linear, no aliasing), the only clean fix is to run the shaper at a higher rate. This wraps
 * the drive in the windowed-sinc 2x FIR cascade above (up -> shaper@fs_os -> down), with a user
 * switch. With os==1 it is BIT-IDENTICAL to the bare zc_drive path (stages=0, no FIR, DC-blocker at
 * base_fs), so "Off" reproduces the plain drive exactly (no regression).
 * With os > 1 the shaper inside the oversampled section is the ADAA form (antiderivative
 * anti-aliasing), which suppresses the alias the oversampling alone leaves. */
/* ================================================================================================
 * THE FAST OVERSAMPLING PATH -- polyphase + portable SIMD
 * ================================================================================================
 * With 32 taps at 4x the FIRs are where the plugin spends its time, and a normal session carries
 * dozens of instances. Two separate things make a direct-form path expensive (the direct form lives
 * in the development harnesses, which use it as the definition of the filter):
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
 *   target_clones("default","arch=x86-64-v3"), so this source is compiled once per clone: the
 *   x86-64-v3 clone gets 4-wide ymm FMA and the default clone gets 2-wide SSE2 from the same lines.
 *   AVX2 intrinsics here would pin the whole plugin to a CPU that has AVX2.
 *
 * This path is NOT bit-identical to the reference one, deliberately: the terms are the same, the
 *   ORDER of the additions is not. a development test (fir_fast_check.c) pins the difference at rounding level
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
    /* the direct-form reference pushes src[2i] then src[2i+1] and convolves, so the newest sample of the
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
 * near Nyquist. The full tables are in the project's development notes. */
#define ZC_DRV_MAX_STAGES 2   /* 2^2 = 4x maximum for the drive */

/* Block size the engine chunks to, and the scratch that implies. They live HERE, with the engine
 * that dictates them, rather than in lv2.c, so that everything that needs the figure -- the plugin
 * and the development memory report -- reads one definition. */
#define ZC_MAXBLK 1024
/* + ZC_OS_PAD: the oversampler's FIRs are overlap-save, so each buffer carries run-up in front of
 * the signal and the caller's pointer is offset past it. The run-up and the offset are the same
 * ZC_OS_PAD doubles, so one of the two is margin (256 bytes per buffer), kept deliberately. */
#define ZC_SCRATCH_LEN (ZC_MAXBLK * (1 << ZC_DRV_MAX_STAGES) + 2 * ZC_OS_PAD)
typedef struct {
  ZcDrive drv;                                   /* shaper + DC-blocker, coeffs set at fs_os */
  int os, stages;                                /* 1, 2 or 4 ; log2(os) */
  ZcUpState up[ZC_DRV_MAX_STAGES];               /* just the block tail, no circular buffer */
  ZcDnState dn[ZC_DRV_MAX_STAGES];
  /* [0] = the OUTER stage's filter (the long half-band), [1] = the INNER one (the short one).
   * At 2x (a harness reference) only [0] is used. Derived in zc_driveos_set. */
  ZcOsPhases ph[2];
} ZcDriveOS;

static inline void zc_driveos_reset(ZcDriveOS *d) {
  zc_drive_reset(&d->drv);
  for (int s = 0; s < d->stages; s++) { zc_upstate_reset(&d->up[s]); zc_dnstate_reset(&d->dn[s]); }
}
/* set gain/DC-blocker for the (possibly oversampled) rate; recomputes os/stages. Does NOT reset the
 * FIR/DC state -> safe for a live knob move (same os). Caller resets when os changes or on engage. */
static inline void zc_driveos_set(ZcDriveOS *d, double knob, double base_fs, int os) {
  /* The factor is the caller's (the plugin passes 4, ZC_DRIVE_OS in lv2.c); this is only the memory bound, so
   * that no value could size a cascade deeper than the scratch (ZC_DRV_MAX_STAGES). */
  if (os > 4) os = 4;
  d->os = os; d->stages = 0; for (int o = os; o > 1; o >>= 1) d->stages++;
  if (d->ph[0].T == 0) {    /* the kernels are constants: built once (d must start zeroed, as calloc does) */
    zc_os_phases_from(&d->ph[0], zc_os_kernel, ZC_OS_TAPS);   /* outer: long half-band */
    zc_os_phases_from(&d->ph[1], zc_in_kernel, ZC_IN_TAPS);   /* inner: short half-band */
  }
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
__attribute__((target_clones("default","arch=x86-64-v3")))
static void zc_driveos_process(ZcDriveOS *restrict d, const float *in,
                                       float *out, int n,
                                       double *restrict sa, double *restrict sb) {
  /* in and out may be the same buffer (a host may process in place), so neither is restrict. */
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
 * zc_driveos_latency_total below adds it. At 2x that is 14.5 + 0.25 = 14.75. */
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
  double a[N * 4 + 2 * ZC_OS_PAD], b[N * 4 + 2 * ZC_OS_PAD];   /* on the stack: no state shared between instances */
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
 * At 2x it is 14.75 and at 4x 16.93, i.e. NOT integer, which is why the latency port is not
 * declared as an integer. Cross-checked against the impulse centroid of the whole oversampled
 * path, which lands within a few hundredths of a sample of both figures. */
static inline float zc_driveos_latency_total(int os) {
  if (os <= 1) return 0.0f;
  return zc_driveos_latency(os) + 0.5f / (float)os;
}

#endif /* ZC_DRIVE_H */
