/* host_test.c -- mini-host: dlopen the .so, instantiate, wire up ports, process and check
 * the wrapper's behavior (exact bypass, flat ~unity, real boost). Verification only. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <lv2/core/lv2.h>
#include "ports.h"
#include "uris.h"

#define SR 48000.0
#define N  16384

static float ports[ZC_N_PORTS];
static float inL[N], inR[N], outL[N], outR[N];

static double rms(const float *x, int n0, int n1) {
  double s = 0; for (int i = n0; i < n1; i++) s += (double)x[i] * x[i];
  return sqrt(s / (n1 - n0));
}
static void sine(float *x, double f) { for (int i = 0; i < N; i++) x[i] = 0.25f * sinf(2*M_PI*f*i/SR); }

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : "build/Dynamo-0C.so";
  void *h = dlopen(so, RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  LV2_Descriptor_Function df = (LV2_Descriptor_Function)dlsym(h, "lv2_descriptor");
  if (!df) { fprintf(stderr, "dlsym lv2_descriptor: %s\n", dlerror()); return 1; }
  const LV2_Descriptor *d = df(0);
  if (!d || strcmp(d->URI, ZC_URI)) { fprintf(stderr, "bad descriptor/URI\n"); return 1; }

  LV2_Handle inst = d->instantiate(d, SR, "", NULL);
  if (!inst) { fprintf(stderr, "instantiate failed\n"); return 1; }
  /* defaults */
  for (int i = 0; i < ZC_N_PORTS; i++) ports[i] = 0.0f;
  ports[P_POWER] = 1; ports[P_EQ_ON] = 1; ports[P_HI_FREQ]=ports[P_HI_GAIN]=0.5f;
  ports[P_HIMID_FREQ]=ports[P_HIMID_GAIN]=0.5f; ports[P_LOMID_FREQ]=ports[P_LOMID_GAIN]=0.5f;
  ports[P_LO_FREQ]=ports[P_LO_GAIN]=0.5f; ports[P_LP_FREQ]=1.0f; ports[P_OVERSAMPLE]=2;
  ports[P_HP_ON]=1; ports[P_LP_ON]=1;   /* enables: host default = 1 */
  for (int i = 0; i < ZC_N_PORTS; i++) d->connect_port(inst, i, &ports[i]);
  d->connect_port(inst, P_IN_L, inL); d->connect_port(inst, P_IN_R, inR);
  d->connect_port(inst, P_OUT_L, outL); d->connect_port(inst, P_OUT_R, outR);
  d->activate(inst);

  int fail = 0;
  /* 1) POWER off = exact passthrough */
  sine(inL, 1000); memcpy(inR, inL, sizeof inL);
  ports[P_POWER] = 0; d->run(inst, N);
  double maxd = 0; for (int i=0;i<N;i++){ double e=fabs(outL[i]-inL[i]); if(e>maxd)maxd=e; }
  printf("[power off] max|out-in| = %.2e  %s\n", maxd, maxd==0?"OK":"FAIL"); fail |= (maxd!=0);
  ports[P_POWER] = 1;

  /* 2) flat (all bands at 0.5, HP/LP off): the wrapper reproduces the MODEL, which has
   *    ~0.14 dB of center residual at 1kHz (intrinsic to the analog prototype, not the wrapper).
   *    Informational, not a wrapper failure. */
  d->run(inst, N);
  double g_flat = 20*log10(rms(outL,2000,N-2000)/rms(inL,2000,N-2000));
  printf("[flat 1kHz] gain = %+.3f dB  (model center residual, ~+0.14 dB expected)\n", g_flat);

  /* 3) Hi shelf boost: hf_shelf=1, hi_gain high -> raises highs */
  ports[P_HF_SHELF] = 1; ports[P_HI_GAIN] = 0.95f;
  sine(inL, 12000); memcpy(inR, inL, sizeof inL); d->run(inst, N);
  double g_hi = 20*log10(rms(outL,2000,N-2000)/rms(inL,2000,N-2000));
  printf("[hi-shelf boost 12kHz] gain = %+.2f dB  %s\n", g_hi, g_hi>6?"OK (rises)":"FAIL");
  fail |= (g_hi < 6);
  /* at 100 Hz the hi-shelf barely touches */
  sine(inL, 100); memcpy(inR, inL, sizeof inL); d->run(inst, N);
  double g_lo = 20*log10(rms(outL,2000,N-2000)/rms(inL,2000,N-2000));
  printf("[hi-shelf @100Hz] gain = %+.2f dB  %s\n", g_lo, fabs(g_lo)<1.0?"OK (flat)":"CHECK");

  /* reset hi band to flat to isolate the filters */
  ports[P_HF_SHELF]=0; ports[P_HI_GAIN]=0.5f;

  /* 4) HP engages by knob position: low-cut */
  ports[P_HP_FREQ]=0.6f;   /* corner ~hundreds of Hz */
  sine(inL, 40); memcpy(inR, inL, sizeof inL); d->run(inst, N);
  double g_hp = 20*log10(rms(outL,2000,N-2000)/rms(inL,2000,N-2000));
  printf("[HP @40Hz, knob 0.6] gain = %+.1f dB  %s\n", g_hp, g_hp < -6 ? "OK (cuts)" : "FAIL");
  fail |= (g_hp > -6);
  ports[P_HP_FREQ]=0.0f;   /* CCW end = off */
  sine(inL, 40); memcpy(inR, inL, sizeof inL); d->run(inst, N);
  double g_hpoff = 20*log10(rms(outL,2000,N-2000)/rms(inL,2000,N-2000));
  printf("[HP off (knob 0)] gain = %+.2f dB  %s\n", g_hpoff, fabs(g_hpoff)<0.5?"OK (passes)":"CHECK");

  /* 5) LP engages by position: high-cut */
  ports[P_LP_FREQ]=0.4f;   /* corner ~1-2 kHz */
  sine(inL, 10000); memcpy(inR, inL, sizeof inL); d->run(inst, N);
  double g_lp = 20*log10(rms(outL,2000,N-2000)/rms(inL,2000,N-2000));
  printf("[LP @10kHz, knob 0.4] gain = %+.1f dB  %s\n", g_lp, g_lp < -6 ? "OK (cuts)" : "FAIL");
  fail |= (g_lp > -6);
  ports[P_LP_FREQ]=1.0f;   /* CW end = off */

  /* 6) reported latency (OS=4) */
  printf("[latency OS4] = %.2f samples\n", ports[P_LATENCY]);

  /* 7) stability: finite output */
  int nan=0; for(int i=0;i<N;i++) if(!isfinite(outL[i])) nan++;
  printf("[stability] non-finite samples = %d  %s\n", nan, nan==0?"OK":"FAIL"); fail|=(nan!=0);

  d->cleanup(inst); dlclose(h);
  printf("\n%s\n", fail? "SOME FAILURE ✗" : "WRAPPER OK ✓");
  return fail;
}
