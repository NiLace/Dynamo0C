/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* dsp.h -- DSP core of the equalizer: the umbrella over section.h, drive.h and chain.h (header-only; C11 with GCC
 * extensions: vector types, target_clones).
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
 *      Nyquist WITHOUT cramping. Faithful to the analogue within 0.6 dB over the whole control range, and
 *      always stable (the poles come from the matched-Z denominator of a stable analogue
 *      prototype). The EQ is LINEAR, so it runs at the base rate: one biquad-style section per
 *      band, no oversampling, zero latency.
 *   3. The chain runs sample by sample (Direct Form II transposed); the numerator can reach degree
 *      7 (nz<=2 plus the FIR correction of degree ZC_MZ_CORR=5), the denominator degree 3.
 *
 *   Comments that name a test, a probe or a generator (ramp_stability.c, gen_drive.py, ...) refer to the project's
 *   development tools, which are not part of the published source.
 *
 *   The coefficients are only recomputed when a control changes (not per sample) and can be
 *   refreshed "live" without resetting the state, avoiding clicks when moving the controls.
 */
#ifndef ZC_DSP_H
#define ZC_DSP_H

#include "section.h"   /* one IIR section and its designers (analog.h prototypes, matched.h designer) */
#include "drive.h"     /* the channel drive and its oversampling */
#include "chain.h"     /* the chain, the controls and the real-time refresh (grid.h) */

#endif /* ZC_DSP_H */
