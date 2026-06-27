/* ports.h -- port indices for the Dynamo 0C EQ. Single source of truth (included by lv2.c and the UI). */
#ifndef ZC_PORTS_H
#define ZC_PORTS_H

typedef enum {
  /* audio (stereo) */
  P_IN_L      = 0,
  P_IN_R      = 1,
  P_OUT_L     = 2,
  P_OUT_R     = 3,

  /* global (panel switch bank: POWER, EQ) */
  P_POWER     = 4,   /* toggle: master. 0=everything bypassed (audio passes through) */
  P_EQ_ON     = 5,   /* toggle: engages the 4 bands. 0=bands out, HP/LP stay active */

  /* HIGH (internal band idx 3) */
  P_HI_FREQ   = 6,   /* norm 0..1 */
  P_HI_GAIN   = 7,   /* norm 0..1 (0.5 = flat) */
  P_HF_SHELF  = 8,   /* toggle: 0=bell 1=shelf */

  /* HI MID (idx 2, always bell) */
  P_HIMID_FREQ= 9,
  P_HIMID_GAIN= 10,

  /* LO MID (idx 1, always bell) */
  P_LOMID_FREQ= 11,
  P_LOMID_GAIN= 12,

  /* LOW (idx 0) */
  P_LO_FREQ   = 13,
  P_LO_GAIN   = 14,
  P_LF_SHELF  = 15,  /* toggle: 0=bell 1=shelf */

  /* filters */
  P_HP_FREQ   = 16,  /* norm 0..1 */
  P_HP_ON     = 17,  /* toggle */
  P_LP_FREQ   = 18,  /* norm 0..1 */
  P_LP_ON     = 19,  /* toggle */
  P_LP_MODE   = 20,  /* toggle: 0=clean (Q=0.707, no peak) 1=resonant (Q=0.99, with boost) */

  /* quality / output */
  P_OVERSAMPLE= 21,  /* enum: 0=1x 1=2x 2=4x 3=Auto (adaptive to the material) */
  P_LATENCY   = 22,  /* output: latency in samples */

  P_HP_BUMP   = 23,  /* HP toggle: 0=clean (Butterworth) 1=resonant (boost ~+1.5 dB at the cutoff) */

  /* channel drive (console-style input stage). norm 0..1, 0=bypass (auto-on when the knob moves).
   * 0.5 = nominal intensity of the reference console; >0.5 pushes harder. Bounded asym. shaper + DC-block. */
  P_DRIVE     = 24,

  ZC_N_PORTS  = 25
} PortIndex;

#endif /* ZC_PORTS_H */
