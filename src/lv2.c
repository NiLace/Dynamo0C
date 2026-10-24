/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* lv2.c -- Dynamo 0C EQ : LV2 wrapper for the DSP core.
 *
 * Per-channel chain (L/R): optional DRIVE -> HP -> 4 bells (LOW/HI switch bell<->shelf) -> LP. Each
 * EQ section is an analog-matched design of the analog prototype (section.h, from the prototypes in analog.h and the data in coeffs.h),
 * run at the base rate (no oversampling). Only the non-linear drive is oversampled (4x).
 * Control + audio ports only: no worker, no atoms, no file state.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

#include <lv2/core/lv2.h>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#if defined(__SSE3__)
#include <pmmintrin.h>
#endif

#include "uris.h"
#include "ports.h"
#include "port_defaults.h"
#include "dsp.h"

/* Flush-to-zero + denormals-are-zero: avoids the CPU spikes denormals cause in the IIRs during
 * silence. A denormal multiply costs around 17x a normal one, and the tails of a six-section chain
 * stay in that range for a long time after the music stops.
 *
 * MXCSR is per-THREAD, not per-plugin. Setting these bits and leaving them set would change the
 * floating-point mode for everything that runs afterwards on the audio thread: every other plugin
 * in the chain and the host's own summing. That is a side effect on shared state that nothing
 * declares, so the register is saved on entry and put back on exit, at a cost of about 7.4 ns per
 * run() call.
 *
 * The restore must not be hoisted above the audio, which would silently disable FTZ for the very
 * processing it protects. It is not: a two-million-sample silent tail produces zero denormal output
 * samples, the same as a build without the restore. Nothing in the language orders an intrinsic
 * against the floating-point work around it, so this is checked rather than assumed. */
static inline unsigned denormal_off(void) {
#if defined(__SSE__)
  const unsigned saved = _mm_getcsr();
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#if defined(__SSE3__)
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
  return saved;
#else
  return 0;
#endif
}
static inline void denormal_restore(unsigned saved) {
#if defined(__SSE__)
  _mm_setcsr(saved);
#else
  (void)saved;
#endif
}

/* ZC_MAXBLK and ZC_SCRATCH_LEN come from drive.h, which sizes the scratch for one block at the maximum oversampling
 * factor plus the FIRs' run-up. What the wrapper adds is the 4x it always runs, which must fit that maximum. */
_Static_assert((1 << ZC_DRV_MAX_STAGES) >= 4, "the drive's scratch must hold a block at 4x");

typedef struct {
  float *port[ZC_N_PORTS];          /* pointers to port buffers, by index */
  double base_fs;

  ZcChain chL, chR;                 /* CURRENT chain, per channel */
  ZcChain chL_p, chR_p;             /* PREVIOUS chain (fades out during the crossfade) */
  /* The design in force at the END of the previous block. When a knob moves, the block is walked
   * from this design to the new one in a few sub-steps rather than jumping, which keeps the size
   * of the steps from depending on the host's buffer size. */
  ZcChain chL_from, chR_from;       /* design at the END of the previous block */
  ZcChain chL_to,   chR_to;         /* design this block is moving TO */
  int ramp_pending;                 /* set on a block where the design changed */
  /* Ping-pong scratch for the DRIVE oversampling. zc_driveos_process needs n*os doubles in each
   * buffer, with n <= ZC_MAXBLK and os <= 2^ZC_DRV_MAX_STAGES, so that product is the requirement
   * and the size is derived from it rather than written out: the scratch dominates per-instance
   * memory, so any margin here is paid dozens of times over in a session. The EQ chain runs at 1x
   * and needs no scratch. */
  double sa[ZC_SCRATCH_LEN];
  double sb[ZC_SCRATCH_LEN];

  ZcDriveOS drvL, drvR;             /* CURRENT drive (input stage, before the EQ; always 4x oversampled) */
  ZcDriveOS drvL_p, drvR_p;         /* PREVIOUS drive */
  float  pbufL[ZC_MAXBLK], pbufR[ZC_MAXBLK];   /* previous path's output, for mixing */

  int   cur_bypass, cur_drive_on;   /* config of the CURRENT path */
  int   prev_bypass, prev_drive_on;               /* config of the PREVIOUS path (during the fade) */
  /* ONE LATENCY FOR EVERY MODE. The plugin always reports dly_off, the drive's latency rounded to a whole sample (17);
   * the paths without the drive (bypass, the EQ alone) read the input dly_off samples earlier, the drive's path not at
   * all, so a crossfade between two paths mixes them aligned to within 0.07 samples. With each mode at its own latency
   * the fade summed copies 15-17 samples apart: a comb for 5 ms, worst-frame ANMR +16..+23 dB. The cost: 17 samples
   * (0.35 ms at 48 kHz) even with the drive off. The input history holds the last ZC_HIST samples in front of the current chunk; every path
   * reads from it, so input and output may be the same buffer (in-place) without either path reading what the
   * other wrote. ZC_LATENCY_PER_MODE restores the old behaviour: the negative arm of make abuse's xfade leg. */
#define ZC_HIST 32
  float histL[ZC_HIST + ZC_MAXBLK], histR[ZC_HIST + ZC_MAXBLK];
  int   cur_delay, prev_delay;      /* samples each path reads behind the input (0 or dly_off) */
  /* A LONGER input history, to warm the drive when it starts. Engaging the drive or leaving the bypass restarts its
   * state from zero, and a cold drive differs from one that had been running for as long as its slowest state takes
   * to settle: the DC blocker after the shaper (14.6 Hz, a time constant of 10.9 ms); the oversampling FIRs and the
   * ADAA fill in about 16 input samples. So the new drive is warmed in two steps (warm_drive): the blocker's state from
   * this history, kept DECIMATED to at most 48 kHz (each entry the mean of warm_dec input samples) so that 4096 entries
   * cover 85 ms or more at any rate, and the FIRs and the ADAA by running the drive over the last ZC_HIST input
   * samples. Once per switch, never per block. ZC_COLD_DRIVE skips it: the negative arm of make abuse's engage leg. */
#define ZC_WARM 4096
  float warmL[ZC_WARM], warmR[ZC_WARM];
  int   warm_w, warm_n;             /* ring write position; entries held (<= ZC_WARM) */
  int   warm_dec, warm_ph;          /* input samples per entry (ceil(rate / 48 kHz)); samples summed into the next one */
  float warm_accL, warm_accR;       /* their running sums */
  int   warm_pending;               /* the drive was restarted this block: warm it once its gain is set */
  int   dly_off;                    /* whole samples that bring the paths without the drive up to its latency */
  int   xfade;                      /* crossfade samples remaining (0 = no transition) */
  int   cfg;                        /* config token; a change triggers the crossfade */

  int   last_sig;                   /* structure signature, to detect rebuild */
  int   have;                       /* 0 until the first build */
  int   designed;                   /* activate() designed the chain for the controls it read (see activate) */
  float latency_samples;
  /* The drive's latency, computed ONCE per instance rather than on every block. It is the FIR cascade's group delay,
   * independent of the signal, the controls and the rate, and measuring it costs about 28.8 us plus 16 KB of stack,
   * which on the audio thread at a 64-sample block is more than the drive's real work. */
  float lat_drive;
  int rr_cursor;                    /* round-robin: which section the next refresh starts at */
  unsigned approxL;                 /* bitmask of sections currently set by interpolation (chR copies chL's design) */
  ZcControls cur_ct;                /* controls the CURRENT chain was (re)built/refreshed with:
                                     * lets us skip the per-block coeff refresh when nothing moved */
} Zc;

/* ---- the shared coefficient grid --------------------------------------------------------------
 * Pre-designed coefficients on a grid of control positions, so that a moving knob can interpolate
 * instead of running the designer. ONE table for the whole process rather than one per instance:
 * it is 0.63 MB, about five times the size of an instance, and a session has dozens of instances.
 * Filling it costs 0.71 s (i7-8700K, 48 kHz), so it cannot happen in instantiate() -- the host would freeze while the
 * user inserts the plugin. A background thread fills it and publishes it with a release store; the
 * audio thread does an acquire load and simply uses the designer until the pointer appears.
 * Nothing waits and nothing locks.
 *
 * The table is freed by the LAST instance's cleanup(), when no audio thread can be reading it (a
 * second instance arriving meanwhile keeps it). Leaving it allocated would leak 0.63 MB every time
 * a host unloads the plugin and loads it again.
 * It is built for ONE sample rate, the first one that asks. An instance at any other rate keeps
 * using the designer, which is correct, only slower. */
static ZcGrid       *zc_grid_ptr = NULL;      /* published; read with acquire */
static int           zc_grid_started = 0;
static pthread_t     zc_grid_th;
static int           zc_grid_th_valid = 0;
static int           zc_grid_stop = 0;        /* asks the filler to give up early */
static int           zc_inst_count = 0;
static pthread_mutex_t zc_grid_lock = PTHREAD_MUTEX_INITIALIZER;

/* THE THREAD MUST NOT OUTLIVE THE LAST INSTANCE. If it is left detached, removing the plugin
 * unloads the .so while the thread is still filling the table, and it jumps into freed code -- a
 * crash in the host, at plugin removal. So the thread is joinable, it checks a stop flag between
 * bands, and cleanup() of the LAST instance sets the flag and joins it. Worst case that costs one
 * band's fill, about 0.12 s on an i7-8700K, at close; usually the fill is long finished and it costs nothing. */
static void *zc_grid_thread(void *arg) {
  double fs = *(double *)arg; free(arg);
  /* LOWEST PRIORITY. The fill costs 0.71 s of solid CPU and starts exactly when the user inserts
   * the plugin, i.e. while audio is running: without this the audio path runs about 120 % slower
   * while the fill is in flight, which in a host is an xrun rather than a slow benchmark. On Linux
   * nice() applies per THREAD, so this demotes only the filler and leaves the audio thread alone.
   * A failure here needs no handling -- the table simply takes longer to appear and the plugin
   * keeps designing -- hence the cast to void. */
  (void)nice(19);
  ZcGrid *g = (ZcGrid *)calloc(1, sizeof(ZcGrid));
  if (!g) return NULL;
  g->fs = fs;
  int failed = 0;
  for (int w = 0; w < ZC_GB_N; w++) {
    if (__atomic_load_n(&zc_grid_stop, __ATOMIC_ACQUIRE)) { free(g); return NULL; }
    failed += zc_grid_fill_one(&g->band[w], w, fs);
  }
  /* a table with a failed node is not published: the plugin keeps designing exactly (see grid.h) */
  if (failed || __atomic_load_n(&zc_grid_stop, __ATOMIC_ACQUIRE)) { free(g); return NULL; }
  __atomic_store_n(&zc_grid_ptr, g, __ATOMIC_RELEASE);
  return NULL;
}
/* Starts the filler for rate fs. Called with zc_grid_lock held. */
static double zc_grid_fs;            /* the rate of the last fill asked for */
static void zc_grid_start_locked(double fs) {
  double *arg = (double *)malloc(sizeof(double));
  if (!arg) return;
  *arg = fs; zc_grid_fs = fs;
  __atomic_store_n(&zc_grid_stop, 0, __ATOMIC_RELEASE);
  /* an ordinary thread whatever the caller's policy: a host may instantiate from a real-time thread, and an inherited
   * SCHED_FIFO would let the 0.7 s fill compete with the audio (nice() has no effect on a real-time policy) */
  pthread_attr_t at; pthread_attr_init(&at);
#ifndef ZC_FILLER_INHERITS_SCHED   /* the negative arm of make abuse's rtsched leg */
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_OTHER);
  struct sched_param sp = { .sched_priority = 0 }; pthread_attr_setschedparam(&at, &sp);
#endif
  int ok = pthread_create(&zc_grid_th, &at, zc_grid_thread, arg) == 0;
  pthread_attr_destroy(&at);
  if (ok) { zc_grid_started = 1; zc_grid_th_valid = 1; }
  else free(arg);
}
/* Called from instantiate() (never from run()): starts the fill once, for the first rate seen. */
static void zc_grid_request(double fs) {
  pthread_mutex_lock(&zc_grid_lock);
  zc_inst_count++;
  if (!zc_grid_started && !zc_grid_ptr) zc_grid_start_locked(fs);
  else if (!zc_grid_th_valid && !zc_grid_ptr) zc_grid_fs = fs;   /* a stopped fill being joined: see zc_grid_release */
  pthread_mutex_unlock(&zc_grid_lock);
}
/* Called from cleanup(). The last instance out joins the filler and frees the table. */
static void zc_grid_release(void) {
  pthread_mutex_lock(&zc_grid_lock);
  int last = (--zc_inst_count <= 0);
  int join = last && zc_grid_th_valid;
  if (join) { zc_grid_th_valid = 0; __atomic_store_n(&zc_grid_stop, 1, __ATOMIC_RELEASE); }
  pthread_mutex_unlock(&zc_grid_lock);
  if (join) pthread_join(zc_grid_th, NULL);    /* joined OUTSIDE the lock: the thread takes none */
  if (!last) return;
  pthread_mutex_lock(&zc_grid_lock);
  /* Re-checked under the lock: an instance may have arrived while we joined, and then the table is
   * its to keep. With none, no run() can be reading it, so it is freed and a later instance starts
   * a fresh fill (for its own rate). ZC_GRID_NEVER_FREED restores the leak: make abuse's negative arm. */
  if (zc_inst_count <= 0) {
    ZcGrid *g = (ZcGrid *)__atomic_exchange_n(&zc_grid_ptr, NULL, __ATOMIC_ACQ_REL);
#ifndef ZC_GRID_NEVER_FREED
    free(g);
#else
    (void)g;
#endif
    zc_grid_started = 0;
  }
#ifndef ZC_GRID_NO_RESTART   /* the negative arm of make abuse's regrid leg */
  else if (!__atomic_load_n(&zc_grid_ptr, __ATOMIC_ACQUIRE) && !zc_grid_th_valid) {
    /* An instance arrived while the stopped fill was being joined: it saw the fill started and did not start one, and
     * no table will appear for it. Start one now, for the rate it asked. */
    zc_grid_start_locked(zc_grid_fs);
  }
#endif
  pthread_mutex_unlock(&zc_grid_lock);
}
static inline const ZcGrid *zc_grid_get(double fs) {
  const ZcGrid *g = (const ZcGrid *)__atomic_load_n(&zc_grid_ptr, __ATOMIC_ACQUIRE);
  return (g && g->fs == fs) ? g : NULL;
}

/* 1 if any of the m samples is Inf or NaN (exponent field all ones). Integer operations only, so it
 * vectorises, and it reads the bits, so no compiler flag can fold it away. */
static inline int zc_any_nonfinite(const float *y, int m) {
  unsigned bad = 0;
  for (int i = 0; i < m; i++) {
    uint32_t u; memcpy(&u, &y[i], sizeof u);
    bad |= (u & 0x7f800000u) == 0x7f800000u;
  }
  return bad != 0;
}

#define ZC_XFADE   256          /* declick crossfade samples (~5.3 ms @48k) */
#define ZC_CFG_BYPASS 0x40000000 /* config token when POWER is off (sig never reaches it) */
#define ZC_CFG_DRIVE  0x01000000 /* token bit: drive active */
/* The drive always runs 4x oversampled: the only factor that
 * meets the alias criterion on material with real energy near Nyquist (see ZC_DRV_MAX_STAGES in drive.h). The
 * drive_os port stays in the TTL so saved sessions load, and is not read. */
#define ZC_DRIVE_OS 4

/* processes a full path (optional drive -> EQ chain) in place. bypass = dry copy.
 * The drive (oversampled) writes into 'out' using the sa/sb scratch; the EQ chain
 * then runs in place at the base rate (no scratch). */
static inline void process_path(int bypass, ZcDriveOS *dv, int drive_on, ZcChain *ch,
                                const float *in, float *out, int m, double *sa, double *sb) {
  if (bypass) { if (out != in) memmove(out, in, m * sizeof(float)); return; }
  if (drive_on) { zc_driveos_process(dv, in, out, m, sa, sb);
                  zc_chain_process(ch, out, out, m); }
  else          zc_chain_process(ch, in, out, m);
}

/* Same, but walking the chain's coefficients from `from` to their current value across the block.
 * Only used on blocks where a control moved; every other block takes the plain path above and is
 * bit-identical to what it was. */
static inline void process_path_ramp(int bypass, ZcDriveOS *dv, int drive_on, ZcChain *ch,
                                     const ZcChain *from, const ZcChain *to,
                                     const float *in, float *out, int m,
                                     double *sa, double *sb, int steps) {
  if (bypass) { if (out != in) memmove(out, in, m * sizeof(float)); return; }
  if (drive_on) { zc_driveos_process(dv, in, out, m, sa, sb);
                  zc_chain_process_ramp(ch, from, to, out, out, m, steps); }
  else          zc_chain_process_ramp(ch, from, to, in, out, m, steps);
}

/* --- structure signature: which sections/kinds exist (rebuild only if it changes) --- */
static int structure_sig(const ZcControls *ct) {
  return (ct->hp_on ? 1 : 0)
       | (ct->lp_on ? 2 : 0)
       | (ct->band[0].shelf ? 4 : 0)
       | (ct->band[3].shelf ? 8 : 0)
       | (ct->lp_resonant ? 16 : 0)
       | (ct->band[0].on ? 32 : 0)            /* EQ engage: bands in/out = structure change */
       | (ct->hp_bump ? 64 : 0);              /* BUMP changes the HP's Q */
}

/* A control port's value, made safe to use. The host is supposed to stay inside the TTL's range, but
 * nothing enforces it -- automation curves, a scripted or broken host, a preset from elsewhere -- and
 * the designer turns a NaN band frequency into an array index: a host crash, not a wrong filter.
 * So a NaN reads as the port's default and anything else is clamped to [minimum, maximum], both as
 * the TTL states them (port_defaults.h is generated from it). For a value inside the range this is
 * the identity, so it changes nothing a well-behaved host can hear. ZC_TRUST_PORTS reads the raw
 * value: the negative arm of make abuse. */
static inline float zc_port(const Zc *self, int p) {
  const float v = *self->port[p];
#ifndef ZC_TRUST_PORTS
  if (v != v) return ZC_TTL_DEFAULT[p];
  if (v < ZC_TTL_MIN[p]) return ZC_TTL_MIN[p];
  if (v > ZC_TTL_MAX[p]) return ZC_TTL_MAX[p];
#endif
  return v;
}

/* Every control port is dereferenced WITHOUT a null check, and that is deliberate: LV2 requires the
 * host to call connect_port() for every port before it may call run(), so a null port pointer is a
 * host bug rather than a state this plugin has to survive. Guarding some ports and not others would
 * suggest the case is handled when it is not. */
static void read_controls(Zc *self, ZcControls *ct) {
  memset(ct, 0, sizeof *ct);
  /* HP/LP: the filter engages by knob POSITION (off at the extreme):
     HP off at the full-CCW end (norm 0), LP off at the full-CW end (norm 1). The *_ON port is an
     extra optional enable (default 1), in case a host wants to disable it. */
  ct->hp_freq = zc_port(self, P_HP_FREQ);
  ct->hp_on = (zc_port(self, P_HP_ON) > 0.5f) && (ct->hp_freq > ZC_HP_OFF_NORM);
  ct->hp_bump = zc_port(self, P_HP_BUMP) > 0.5f;
  ct->lp_freq = zc_port(self, P_LP_FREQ);
  ct->lp_on = (zc_port(self, P_LP_ON) > 0.5f) && (ct->lp_freq < ZC_LP_OFF_NORM);
  ct->lp_resonant = zc_port(self, P_LP_MODE) > 0.5f;
  /* bands: 0=low 1=lomid 2=himid 3=hi. EQ engage (P_EQ_ON) activates them all at once;
     HP/LP don't depend on it. Flat (gain 0.5) = transparent. */
  int eq_on = zc_port(self, P_EQ_ON) > 0.5f;
  ct->band[0].on = eq_on; ct->band[0].shelf = zc_port(self, P_LF_SHELF) > 0.5f;
  ct->band[0].freq = zc_port(self, P_LO_FREQ);    ct->band[0].gain = zc_port(self, P_LO_GAIN);
  ct->band[1].on = eq_on; ct->band[1].freq = zc_port(self, P_LOMID_FREQ); ct->band[1].gain = zc_port(self, P_LOMID_GAIN);
  ct->band[2].on = eq_on; ct->band[2].freq = zc_port(self, P_HIMID_FREQ); ct->band[2].gain = zc_port(self, P_HIMID_GAIN);
  ct->band[3].on = eq_on; ct->band[3].shelf = zc_port(self, P_HF_SHELF) > 0.5f;
  ct->band[3].freq = zc_port(self, P_HI_FREQ);    ct->band[3].gain = zc_port(self, P_HI_GAIN);
}

static LV2_Handle
instantiate(const LV2_Descriptor *d, double rate, const char *bundle,
            const LV2_Feature *const *features)
{
  (void)d; (void)bundle; (void)features;
#ifndef ZC_ANY_RATE
  /* An impossible rate is refused, which LV2 allows (instantiate returns NULL and the host reports it). Accepted, a
   * negative rate made every design fall back to pass-through while the drive's DC blocker, built from the same rate,
   * turned unstable: at -48 kHz and 4x a -6 dBFS input came out at +10 dBFS. The top is 768 kHz: above about 1.16 MHz
   * the low-pass at its lowest corner no longer designs and would pass through unfiltered. ZC_ANY_RATE accepts
   * anything: the negative arm of make abuse. */
  if (!(rate >= 1000.0 && rate <= 768000.0)) return NULL;
#endif
  Zc *self = (Zc *)calloc(1, sizeof(Zc));
  if (!self) return NULL;
  self->base_fs = rate;
  self->have = 0;
  self->last_sig = -1;
  zc_grid_request(rate);            /* fills the shared coefficient grid in the background */
  /* off the audio thread, once: the FIR round trip plus 0.5/os of ADAA */
  self->lat_drive = zc_driveos_latency_total(ZC_DRIVE_OS);
  self->dly_off = (int)lrintf(self->lat_drive);
  if (self->dly_off > ZC_HIST) self->dly_off = ZC_HIST;      /* the history's size bounds the read */
#ifndef ZC_WARM_FIXED_SAMPLES   /* the negative arm of make abuse's engage leg: a history 4096 SAMPLES long, too short
                                * above 48 kHz (21 ms at 192 kHz against the blocker's 10.9 ms time constant) */
  self->warm_dec = (int)ceil(rate / 48000.0);
#else
  self->warm_dec = 1;
#endif
  return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void *data) {
  Zc *self = (Zc *)instance;
  if (port < ZC_N_PORTS) self->port[port] = (float *)data;
}

static void activate(LV2_Handle instance) {
  Zc *self = (Zc *)instance;
  zc_chain_reset_state(&self->chL);
  zc_chain_reset_state(&self->chR);
  zc_driveos_reset(&self->drvL); zc_driveos_reset(&self->drvR);
  self->xfade = 0;          /* no transition pending */
  self->cfg = -1;           /* force (re)config on the first run, without triggering a crossfade */
  self->have = 0;
  self->designed = 0;
  memset(self->histL, 0, sizeof self->histL); memset(self->histR, 0, sizeof self->histR);
  self->warm_w = self->warm_n = self->warm_ph = 0; self->warm_accL = self->warm_accR = 0.0f; self->warm_pending = 0;
#ifndef ZC_STALE_RAMP
  /* The first run() after this rebuilds the chains from scratch with exact designs, so nothing
   * pending from before may survive: a ramp set by a run(0) would otherwise walk the fresh chain
   * from and to the OLD designs (a different filter, measured at -10 dB), an interpolated-section
   * mask would point at sections that are now exact, and the round-robin cursor at slots of the
   * old structure. ZC_STALE_RAMP keeps them: the negative arm of make abuse. */
  self->ramp_pending = 0;
  self->approxL = 0;
  self->rr_cursor = 0;
#endif
#ifndef ZC_DESIGN_ON_FIRST_RUN   /* the negative arm of make abuse's firstrun leg */
  /* The chain's exact design costs up to ~0.75 ms (six sections). activate() is not the audio thread, so when the host
   * has already connected the control ports the chain is designed here, for the values they hold, and the first run()
   * only checks that its structure is still the one designed. A host that connects them later gets the design in the
   * first run() instead, as before. */
  int connected = 1;
  for (int p = P_POWER; p < ZC_N_PORTS; p++) if (p != P_LATENCY && !self->port[p]) connected = 0;
  if (connected) {
    ZcControls ct; read_controls(self, &ct);
    zc_chain_build(&self->chL, &ct, self->base_fs);
    self->chR = self->chL;
    self->last_sig = structure_sig(&ct);
    self->cur_ct = ct;
    self->designed = 1;
  }
#endif
}

/* Warms a freshly reset drive (see ZC_WARM), its output discarded. (1) The DC blocker removes from the shaped signal s
 * a component m that is a one-pole low-pass of s, m[n] = (1 - r) s[n-1] + r m[n-1]: it is estimated over the decimated
 * history, with the shaper at that rate. (2) The drive runs over the last ZC_HIST input samples (the history in front
 * of the current chunk), which fills the FIRs and the ADAA; then the blocker is handed m as its state (y1 = x1 - m).
 * The drive's path reads the input with no extra delay, so both steps end on the sample before the chunk. */
#ifndef ZC_COLD_DRIVE
static void warm_drive(Zc *self, ZcDriveOS *d, const float *ring, const float *hist) {
  ZcDrive *v = &d->drv;
  const double g = v->g;
  if (g <= 1e-6) return;
  const double rd = 1.0 - 2.0 * M_PI * 14.62 * self->warm_dec / self->base_fs;   /* the blocker at the history's rate */
  /* The blocker sees the input through the upsampling FIRs, half their round trip later; an entry of the history is
   * the mean of warm_dec samples, (warm_dec - 1) / 2 older than its last one; and the recurrence below, which feeds m
   * with s one entry late, puts it half an entry late on average. So m is taken
   * (0.5 lat_drive - (warm_dec - 1) / 2) / warm_dec - 0.5 entries back, its fractional part interpolated between the
   * last two values. (A sweep of that delay put the residual's minimum there at 44.1, 48, 96 and 192 kHz.) */
  const double dl = (0.5 * self->lat_drive - 0.5 * (self->warm_dec - 1)) / self->warm_dec - 0.5;
  const int lag = dl > 0.0 ? (int)dl : 0;
  const double fr = dl > 0.0 ? dl - lag : 0.0;
  double m = 0.0, mp = 0.0, s1 = 0.0;
  int r = (self->warm_w - self->warm_n + ZC_WARM) % ZC_WARM;   /* the oldest entry held */
  for (int k = 0; k < self->warm_n - lag; k++) {
    const double sk = zc_drive_shape(g * ring[r]) / g;
    mp = m; m = (1.0 - rd) * s1 + rd * m; s1 = sk;
    r = (r + 1) % ZC_WARM;
  }
  m -= fr * (m - mp);
  zc_driveos_process(d, hist, self->pbufL, ZC_HIST, self->sa, self->sb);
  v->y1 = v->x1 - m;      /* after the FIRs' own start-up, which would otherwise stay in the blocker for ~11 ms */
}
#endif

/* ISA dispatch resolved at load time. The plugin is compiled for SSE3, so the EQ recurrence
   vectorises to 128 bits; these clones let a CPU that has them use 256-bit vectors and FMA
   instead, worth about 23 % at the factory default, for a binary
   45 % larger. The two paths agree to -180 dB.
   run() is called once per BLOCK, which is the right boundary for the dispatch: doing it per
   sample costs more than it buys, because the indirection blocks inlining.
   Two clones, not three: an "avx2" clone without FMA would only be chosen by a CPU with AVX2 and no
   FMA, which no x86 CPU is, so it never ran anywhere and was dropped.
   The `default` clone cannot execute on a machine that has AVX2, so it has to be exercised
   elsewhere: `make isa` runs it under qemu-user on CPU models without AVX2, each against a canary
   build that must fault, and checks that no AVX instruction sits outside the x86-64-v3 clones. */
__attribute__((target_clones("default","arch=x86-64-v3")))
static void run(LV2_Handle instance, uint32_t n_samples) {
  Zc *self = (Zc *)instance;
  const unsigned fp_saved = denormal_off();
  const float *inL = self->port[P_IN_L], *inR = self->port[P_IN_R];
  float *outL = self->port[P_OUT_L], *outR = self->port[P_OUT_R];

  const int power = zc_port(self, P_POWER) > 0.5f;

  /* --- configure the CURRENT path (chL/chR + drive) and decide the config token --- */
  ZcControls ct; read_controls(self, &ct);
  int sig = 0, drive_on = 0;
  double drive_knob = 0.0;
  if (power) {
    sig = structure_sig(&ct);
    drive_knob = (double)zc_port(self, P_DRIVE);
    drive_on = drive_knob > 1e-4;
  }

  /* Config token: POWER off, structure, and drive on/off. A CHANGE here triggers a
   * declick crossfade (the old path fades out as the new one comes in), so that toggling
   * POWER / EQ / filters / shelf / bump / drive produces no audible step. Moving continuous knobs
   * (EQ freq/gain, drive intensity) does NOT change the token -> hot refresh, no fade. */
  int newcfg = power ? (sig | (drive_on ? ZC_CFG_DRIVE : 0)) : ZC_CFG_BYPASS;

  /* A config change triggers a declick crossfade. While one is still running, DEFER the new config
   * (don't mutate the live path mid-fade): transitions serialize, so rapid toggles never click.
   * The deferred config is picked up on the block after the current fade ends. */
  int changed = self->have && newcfg != self->cfg;
  int defer   = changed && self->xfade > 0;

  if (changed && self->xfade == 0) {
    /* snapshot the current path -> previous; start the crossfade */
    self->chL_p = self->chL; self->chR_p = self->chR;
    self->drvL_p = self->drvL; self->drvR_p = self->drvR;
    self->prev_bypass = self->cur_bypass; self->prev_drive_on = self->cur_drive_on;
    self->prev_delay = self->cur_delay;
    /* Re-engaging after a bypass: the chains still hold the state from BEFORE the bypass, because
     * while bypassed process_path returns early and the filters are never stepped at all -- so the
     * state does not decay, it FREEZES. Measured: with digital silence at the input, re-engaging
     * after half a second of bypass fires a -3.1 dBFS transient from the very first sample. Start
     * the re-engaged path from rest instead; the fade in from the dry path covers the discontinuity
     * that resetting would otherwise create. */
    if (self->cur_bypass && power) {
      zc_chain_reset_state(&self->chL); zc_chain_reset_state(&self->chR);
      zc_driveos_reset(&self->drvL);    zc_driveos_reset(&self->drvR);
      self->warm_pending = 1;
    }
    self->xfade = ZC_XFADE;
  }

  /* apply the new config to the current path (skipped while a fade is deferring it) */
  if (!defer) {
    if (power) {
      /* structure -> rebuild (state zeroed). A first run whose structure activate() already designed takes the hot
       * refresh below instead, which redesigns only what moved since. */
      if (self->have ? sig != self->last_sig : !(self->designed && sig == self->last_sig)) {
        if (self->have)
          /* A toggle shifts the section indices, but only one section's controls actually moved.
           * Reuse the designs of the survivors instead of recomputing all six: bit-identical, and
           * it is what turns a knob flip from a DSP spike into nothing. */
        {
          unsigned fresh = 0;                      /* bands taken from the grid, to refine (see zc_chain_rebuild_reuse) */
#ifdef ZC_REBUILD_EXACT   /* every band designed exactly in this block: the negative arm of make abuse's eqengage leg */
          const ZcGrid *rg = NULL;
#else
          const ZcGrid *rg = zc_grid_get(self->base_fs);
#endif
#ifndef ZC_KEEP_APPROX_ON_REBUILD
          /* the interpolated sections are redesigned, not carried over (see zc_chain_rebuild_reuse);
           * ZC_KEEP_APPROX_ON_REBUILD copies them as before: the negative arm of make abuse */
          zc_chain_rebuild_reuse(&self->chL, &self->chL, self->base_fs, &ct, &self->cur_ct, self->approxL, rg, &fresh);
#else
          zc_chain_rebuild_reuse(&self->chL, &self->chL, self->base_fs, &ct, &self->cur_ct, 0u, rg, &fresh);
#endif
#ifndef ZC_KEEP_APPROX_ON_REBUILD
          self->approxL = fresh;                   /* the copies are exact; the grid's bands get refined */
#endif
        }
        else
          zc_chain_build(&self->chL, &ct, self->base_fs);
#ifndef ZC_KEEP_APPROX_ON_REBUILD
        if (!self->have) self->approxL = 0;        /* a first build: every section is an exact design */
#endif
        /* Both channels get the SAME ct, and the designer is a pure function of (ct, fs), so the
         * right channel's design is the left one's, word for word. Copying the whole struct is
         * correct HERE (and only here) because zc_chain_build has just zeroed the state, so the
         * z[] that comes along is all zeros. */
        self->chR = self->chL;
        self->last_sig = sig;
        self->cur_ct = ct;
#ifndef ZC_STALE_RAMP
        /* A ramp still pending (set by a run(0), which processes nothing) holds designs of the OLD
         * structure, indexed by the old section order: walking the rebuilt chain through them would
         * put the wrong filter in every section. The rebuild is a structure change and is covered
         * by the crossfade; there is nothing to ramp. */
        self->ramp_pending = 0;
#endif
      } else {
        /* memcmp over a struct is normally a trap -- padding bytes are unspecified and two
         * semantically equal structs can compare different. It is safe HERE, and only here, because
         * read_controls memsets the whole ZcControls to zero before filling it, so the padding is
         * deterministically zero in both operands: ct is freshly memset each block, and cur_ct is a
         * plain copy of a previously memset ct. Anything that stops doing that memset breaks this. */
        const int moved = memcmp(&ct, &self->cur_ct, sizeof ct) != 0;   /* continuous knobs moved */
        /* The refinement runs only on blocks where nothing moved, on purpose: while a knob keeps
         * moving, every block must take the cheap interpolating refresh. Tested first, it grabbed
         * every other block of a gesture -- one full design (~110 us) to make a section exact, which
         * the next block interpolated again -- so the designer ran on half the blocks of a gesture,
         * the very peak the grid exists to remove. ZC_REFINE_FIRST restores that order: the negative
         * arm of make abuse. */
#ifdef ZC_REFINE_FIRST
        const int refine = self->approxL != 0;
#else
        const int refine = !moved && self->approxL != 0;
#endif
        if (refine) {
          /* Nothing moved this block, so there is room to replace ONE interpolated section with its
           * exact design. The grid exists to remove the peak WHILE a control moves; once it stops,
           * what is heard has to be the exact filter, not the interpolated one. Refining a single
           * section per block keeps this from becoming a peak itself. */
          zc_chain_copy_design(&self->chL_from, &self->chL);
          zc_chain_copy_design(&self->chR_from, &self->chR);
          if (zc_chain_refine_one(&self->chL, &ct, &self->approxL)) {
            zc_chain_copy_design(&self->chR, &self->chL);
            zc_chain_copy_design(&self->chL_to, &self->chL);
            zc_chain_copy_design(&self->chR_to, &self->chR);
            self->ramp_pending = 1;
          }
        } else if (moved) {                         /* hot refresh */
          /* Only the sections whose own controls moved are redesigned, and only once: the right
           * channel takes a copy of the DESIGN (never the state). A section that is skipped would
           * have been handed the same inputs and produced the same coefficients, so this is
           * bit-identical, not an approximation.
           * The saving is what makes a moving control affordable: a full two-channel redesign costs
           * 1.37 ms, which is over a whole 64-sample block at 48 kHz, while the selective path with
           * a design copy to the right channel costs 0.11 ms, about 8 % of that block. */
          /* keep the design we are coming FROM, then move to the new one */
          zc_chain_copy_design(&self->chL_from, &self->chL);
          zc_chain_copy_design(&self->chR_from, &self->chR);
          /* Redesign at most `budget` sections in THIS block; the rest drain over the following
           * ones. The budget scales with the block size so the peak stays a roughly constant
           * FRACTION of the block rather than a constant time: at 64 samples one section is about
           * 8 % of the block and all four at once would be 32 %. zc_chain_update_rr marks only what
           * it actually redesigned, so the memcmp above brings control straight back here on the
           * next block. */
          int budget = 1 + (int)(n_samples / 256);
          const ZcGrid *grid = zc_grid_get(self->base_fs);
          zc_chain_update_rr(&self->chL, &ct, &self->cur_ct, budget, &self->rr_cursor,
                             grid, &self->approxL);
          zc_chain_copy_design(&self->chR, &self->chL);
          zc_chain_copy_design(&self->chL_to, &self->chL);
          zc_chain_copy_design(&self->chR_to, &self->chR);
          self->ramp_pending = 1;
        }
      }
      if (drive_on) {
        /* fresh drive state on engage (a token change already forced a declick crossfade); otherwise just
         * hot-refresh the gain (continuous knob move, no click). */
        zc_driveos_set(&self->drvL, drive_knob, self->base_fs, ZC_DRIVE_OS);
        zc_driveos_set(&self->drvR, drive_knob, self->base_fs, ZC_DRIVE_OS);
        if (!self->cur_drive_on) {
          zc_driveos_reset(&self->drvL); zc_driveos_reset(&self->drvR);
          self->warm_pending = 1;
        }
#ifndef ZC_COLD_DRIVE
        if (self->warm_pending) {
          warm_drive(self, &self->drvL, self->warmL, self->histL);
          warm_drive(self, &self->drvR, self->warmR, self->histR);
        }
#endif
      }
      self->warm_pending = 0;
    }
    self->cur_bypass = !power; self->cur_drive_on = drive_on;
    self->cur_delay = (power && drive_on) ? 0 : self->dly_off;
    self->have = 1; self->cfg = newcfg;
#ifndef ZC_LATENCY_PER_MODE
    self->latency_samples = (float)self->dly_off;  /* constant: see ZC_HIST above. The paths without the drive are
                                                    * delayed by exactly this; the drive's own path is lat_drive (16.93),
                                                    * 0.07 earlier. A whole number survives a host that truncates. */
#else
    self->latency_samples = (power && drive_on) ? self->lat_drive : 0.0f;
#endif
  }
  /* the one port the TTL declares lv2:connectionOptional, hence the one null check */
#ifndef ZC_LATENCY_PER_MODE
  if (self->port[P_LATENCY]) *self->port[P_LATENCY] = self->latency_samples;
#else
  if (self->port[P_LATENCY]) *self->port[P_LATENCY] = power ? self->latency_samples : 0.0f;
#endif

  /* --- block processing: current path; if crossfading, the previous one too, then mixed --- */
  for (uint32_t off = 0; off < n_samples; off += ZC_MAXBLK) {
    int m = (int)((off + ZC_MAXBLK <= n_samples) ? ZC_MAXBLK : (n_samples - off));
#ifndef ZC_LATENCY_PER_MODE
    memcpy(self->histL + ZC_HIST, inL + off, (size_t)m * sizeof(float));
    memcpy(self->histR + ZC_HIST, inR + off, (size_t)m * sizeof(float));
    const float *xl = self->histL + ZC_HIST - self->cur_delay,  *xr = self->histR + ZC_HIST - self->cur_delay;
    const float *pl = self->histL + ZC_HIST - self->prev_delay, *pr = self->histR + ZC_HIST - self->prev_delay;
#else
    const float *xl = inL + off, *xr = inR + off, *pl = xl, *pr = xr;
#endif
    float *yl = outL + off, *yr = outR + off;

    /* IN PLACE. LV2 lets a host connect the same buffer to an input and an output port unless the plugin declares
     * lv2:inPlaceBroken -- Ardour does exactly that, and this plugin does not declare it. Both paths read the input
     * from the history above, never from the host's buffer, so the current path writing yl cannot change what the
     * previous one reads. Without the history (ZC_LATENCY_PER_MODE) xl and yl may be the SAME memory, and then the
     * previous path has to read the input BEFORE the current path writes over it.
     * Measured with the old order: 10 of 12 transitions differed under in-place processing, and
     * inside the 256-sample crossfade the error was -4 to -10 dB -- the fade was substantially
     * wrong, not slightly. With separate buffers the two orders are bit-identical.
     * The sa/sb scratch is still shared: each process_path call consumes it entirely before
     * returning, so no call can see another's leftovers. */
#ifndef ZC_PREV_PATH_LAST          /* ZC_PREV_PATH_LAST restores the old order: make inplace's negative arm */
    if (self->xfade > 0) {
      process_path(self->prev_bypass, &self->drvL_p, self->prev_drive_on, &self->chL_p, pl, self->pbufL, m, self->sa, self->sb);
      process_path(self->prev_bypass, &self->drvR_p, self->prev_drive_on, &self->chR_p, pr, self->pbufR, m, self->sa, self->sb);
    }
#endif

    if (self->ramp_pending) {
      /* Deliver this block's coefficient change in a few sub-steps instead of one jump, so the
       * size of the step does not depend on the host's buffer size. Sub-steps are at least 8
       * samples long, so a tiny block does not pay for interpolation it cannot use. This is applied
       * on the FIRST chunk of the call, which for n_samples <= ZC_MAXBLK is the whole block. */
      int steps = m / 8; if (steps > 8) steps = 8; if (steps < 1) steps = 1;
      process_path_ramp(self->cur_bypass, &self->drvL, self->cur_drive_on, &self->chL,
                        &self->chL_from, &self->chL_to, xl, yl, m, self->sa, self->sb, steps);
      process_path_ramp(self->cur_bypass, &self->drvR, self->cur_drive_on, &self->chR,
                        &self->chR_from, &self->chR_to, xr, yr, m, self->sa, self->sb, steps);
      self->ramp_pending = 0;
    } else {
      if (!self->cur_bypass) {
        /* The two EQ chains are independent and each is a serial recurrence, so they are run
         * INTERLEAVED (see chain.h). The drive still runs one channel at a time: both calls share the
         * sa/sb scratch, so interleaving it would need a second pair of buffers. */
        if (self->cur_drive_on) {
          zc_driveos_process(&self->drvL, xl, yl, m, self->sa, self->sb);
          zc_driveos_process(&self->drvR, xr, yr, m, self->sa, self->sb);
          zc_chain_process2(&self->chL, &self->chR, yl, yr, yl, yr, m);
        } else {
          zc_chain_process2(&self->chL, &self->chR, xl, xr, yl, yr, m);
        }
      } else {
        process_path(self->cur_bypass, &self->drvL, self->cur_drive_on, &self->chL, xl, yl, m, self->sa, self->sb);
        process_path(self->cur_bypass, &self->drvR, self->cur_drive_on, &self->chR, xr, yr, m, self->sa, self->sb);
      }
    }

#ifdef ZC_PREV_PATH_LAST
    if (self->xfade > 0) {
      process_path(self->prev_bypass, &self->drvL_p, self->prev_drive_on, &self->chL_p, pl, self->pbufL, m, self->sa, self->sb);
      process_path(self->prev_bypass, &self->drvR_p, self->prev_drive_on, &self->chR_p, pr, self->pbufR, m, self->sa, self->sb);
    }
#endif
    if (self->xfade > 0) {
      int xf = self->xfade;
      for (int i = 0; i < m; i++) {
        double t = 1.0 - (double)xf / ZC_XFADE;      /* 0 -> 1: weight of the new path */
        yl[i] = (float)(self->pbufL[i] * (1.0 - t) + yl[i] * t);
        yr[i] = (float)(self->pbufR[i] * (1.0 - t) + yr[i] * t);
        if (xf > 0) xf--;
      }
      self->xfade = xf;
    }
#ifndef ZC_NO_SELF_HEAL
    /* ONE non-finite sample -- from the input, or made inside -- stays in every recursive state it
     * reaches (the IIR sections, the DC blocker, the FIR histories) and the channel outputs NaN until
     * the host calls activate(), which it may never do. So a non-finite output is silenced and both
     * paths restart from rest: the host hears one silent block instead of a dead channel. A bypassed
     * plugin is left alone: there it is the input, copied, and not ours to change.
     * ZC_NO_SELF_HEAL removes it: the negative arm of make abuse. */
    if (!(self->cur_bypass && self->xfade == 0)
        && (zc_any_nonfinite(yl, m) || zc_any_nonfinite(yr, m))) {
      memset(yl, 0, (size_t)m * sizeof(float)); memset(yr, 0, (size_t)m * sizeof(float));
      zc_chain_reset_state(&self->chL);   zc_chain_reset_state(&self->chR);
      zc_chain_reset_state(&self->chL_p); zc_chain_reset_state(&self->chR_p);
      zc_driveos_reset(&self->drvL);   zc_driveos_reset(&self->drvR);
      zc_driveos_reset(&self->drvL_p); zc_driveos_reset(&self->drvR_p);
      memset(self->histL, 0, sizeof self->histL); memset(self->histR, 0, sizeof self->histR);
      self->warm_w = self->warm_n = self->warm_ph = 0; self->warm_accL = self->warm_accR = 0.0f;
    }
#endif
    /* the warm-up history (ZC_WARM), up to the ring's end and then from its start. It copies the chunk's INPUT from
     * the history above, not from inL/inR: in place, those now hold this chunk's output. */
#ifndef ZC_LATENCY_PER_MODE
    const float *rawL = self->histL + ZC_HIST, *rawR = self->histR + ZC_HIST;
#else
    const float *rawL = inL + off, *rawR = inR + off;
#endif
    if (self->warm_dec == 1) {                        /* up to 48 kHz: the samples themselves, by block copies */
      const int a = m < ZC_WARM - self->warm_w ? m : ZC_WARM - self->warm_w;
      memcpy(self->warmL + self->warm_w, rawL, (size_t)a * sizeof(float));
      memcpy(self->warmR + self->warm_w, rawR, (size_t)a * sizeof(float));
      memcpy(self->warmL, rawL + a, (size_t)(m - a) * sizeof(float));
      memcpy(self->warmR, rawR + a, (size_t)(m - a) * sizeof(float));
      self->warm_w = (self->warm_w + m) % ZC_WARM;
      self->warm_n = self->warm_n + m < ZC_WARM ? self->warm_n + m : ZC_WARM;
    } else {                                          /* above: the mean of each warm_dec samples */
      const float inv = 1.0f / (float)self->warm_dec;
      for (int i = 0; i < m; i++) {
        self->warm_accL += rawL[i]; self->warm_accR += rawR[i];
        if (++self->warm_ph == self->warm_dec) {
          self->warmL[self->warm_w] = self->warm_accL * inv; self->warmR[self->warm_w] = self->warm_accR * inv;
          self->warm_w = (self->warm_w + 1) % ZC_WARM;
          if (self->warm_n < ZC_WARM) self->warm_n++;
          self->warm_ph = 0; self->warm_accL = self->warm_accR = 0.0f;
        }
      }
    }
#ifndef ZC_LATENCY_PER_MODE
    memmove(self->histL, self->histL + m, ZC_HIST * sizeof(float));   /* keep the last ZC_HIST input samples */
    memmove(self->histR, self->histR + m, ZC_HIST * sizeof(float));
#endif
  }
  denormal_restore(fp_saved);
}

static void cleanup(LV2_Handle instance) {
  zc_grid_release();          /* the last instance out joins the grid filler; see zc_grid_request */
  free(instance);
}

static const LV2_Descriptor descriptor = {
  ZC_URI, instantiate, connect_port, activate, run, NULL /*deactivate*/, cleanup, NULL /*extension_data*/
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
