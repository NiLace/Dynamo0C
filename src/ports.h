/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
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

  /* output */
  P_LATENCY   = 21,  /* output: latency in samples, the same in every mode (the 4x drive's; the other paths are delayed to match) */

  P_HP_BUMP   = 22,  /* HP toggle: 0=clean (Butterworth) 1=resonant (the console's own: +1.25 dB about half an octave above the corner) */

  /* channel drive (console-style input stage). norm 0..1, 0=bypass (auto-on when the knob moves).
   * 0.5 = the shaper's nominal intensity (pre-gain 1); >0.5 pushes harder. Bounded asym. shaper + DC-block. */
  P_DRIVE     = 23,

  /* kept only so that sessions saved with earlier versions load: the drive is always 4x oversampled and this port is
   * not read (lv2.c, ZC_DRIVE_OS) */
  P_DRIVE_OS  = 24,

  ZC_N_PORTS  = 25
} PortIndex;

#endif /* ZC_PORTS_H */
