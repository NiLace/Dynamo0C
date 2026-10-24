/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* grid.h -- precomputed coefficient grid + bilinear interpolation.
 *
 * WHAT PROBLEM THIS SOLVES
 *   Moving an EQ knob does not interpolate coefficients: it DESIGNS the filter -- polynomial roots
 *   by Durand-Kerner, a least-squares fit, a factorisation -- and it does so on the AUDIO THREAD,
 *   at about 0.113 ms per section. The round-robin refresh caps how many sections pay that per
 *   block, but the work is still unbounded in principle, because Durand-Kerner is iterative with a
 *   cap of 8000 iterations and no hard bound. A real-time thread needs a bound, not an average.
 *
 *   With the grid, moving a knob costs a bilinear interpolation of 12 doubles: O(1), deterministic,
 *   no iteration, no allocation.
 *
 * WHAT IT COSTS at the 33 nodes per axis this compiles (see ZC_GRID_N below for why 33):
 *   - memory     0.63 MB per sample rate, STATIC AND SHARED -- about seven instances' worth, so a
 *                per-instance copy would be unaffordable in a session with dozens of them
 *   - fill time  0.71 s on an i7-8700K at 48 kHz, which is why it is not filled in instantiate()
 *   - accuracy   p95 -37.2 dB, worst -27.8 dB against the exact design, deliberate and only ever heard
 *                DURING a gesture: the section is redesigned exactly as soon as the knob stops
 *   - stability  every interpolated pole stays inside the unit circle
 *
 * At twice the density (65 nodes per axis) the same probe measures median -71.4 dB, p95 -52.0 dB
 * and worst -35.3 dB, for about four times the memory and the fill time. Both sets come from a development probe (grid_interp.c) over 13824
 * points chosen strictly between nodes, never on one.
 *
 * THE ACCURACY FIGURES ARE A PROPERTY OF THE DENSITY, not of interpolation in general. Interpolating
 * the denominator between two arbitrary designs is not safe as a technique; it is safe here because
 * the nodes are close together. Changing ZC_GRID_N invalidates every number above.
 */
#ifndef ZC_GRID_H
#define ZC_GRID_H
#include <string.h>

#include "section.h"

/* 33 nodes per axis, not the 65 the probe recommended, and the reason is architectural.
 * The probe judged the grid as a REPLACEMENT for the designer, where the interpolation error is
 * what you listen to permanently, and at that standard 33 is too coarse. Here the interpolated
 * filter only exists WHILE THE KNOB IS MOVING: the moment it stops, the section is redesigned
 * exactly. What matters during a gesture is that it does not step or crunch rather than that it
 * lands within a fraction of a dB, and that is what a development test (knob_gate.c) measures.
 *
 * The saving is not cosmetic: (65/33)^2 = 3.9 times fewer nodes, so about a quarter of the memory
 * (0.63 MB instead of ~2.4 MB) and a quarter of the time the filling thread exists at all. */
#define ZC_GRID_N 33

/* One designed filter, without its state: that belongs to the instance, never to the table. */
typedef struct { double b[ZC_MAX_N + 1], a[ZC_MAX_NA + 1]; } ZcGridNode;
typedef struct { ZcGridNode n[ZC_GRID_N][ZC_GRID_N]; } ZcGridBand;

/* The six two-dimensional sections: the four bells and the two shelves. HP and LP are ONE
 * dimensional and cost one design each, so they keep using the designer directly. */
enum { ZC_GB_LO = 0, ZC_GB_LOMID, ZC_GB_HIMID, ZC_GB_HI, ZC_GB_LOSHELF, ZC_GB_HISHELF, ZC_GB_N };

typedef struct {
  ZcGridBand band[ZC_GB_N];
  double fs;
} ZcGrid;

/* --- filling (NOT on the audio thread: 0.71 s on an i7-8700K) -----------------------------------
 * Every node starts from a ZEROED section: a builder whose design fails keeps what the section held
 * (zc_section_keep_or_bypass reads it), so an uninitialised one would hand garbage to the table. A
 * failed node is counted, and the filler does not publish a table that has one: the plugin keeps
 * designing exactly instead of interpolating towards a pass-through node.
 * ZC_GRID_FAIL_NODE=k makes node k of every band report a failure: the negative arm of make gridfill. */
static inline int zc_grid_fill_one(ZcGridBand *g, int which, double fs) {
  int failed = 0;
  for (int i = 0; i < ZC_GRID_N; i++) {
    double nf = (double)i / (ZC_GRID_N - 1);
    for (int j = 0; j < ZC_GRID_N; j++) {
      double ng = (double)j / (ZC_GRID_N - 1);
      ZcSection s;
      memset(&s, 0, sizeof s);
      int ok = (which <= ZC_GB_HI) ? zc_section_build_band(&s, which, 0, nf, ng, fs)
                                   : zc_section_build_band(&s, which == ZC_GB_HISHELF ? 3 : 0, 1, nf, ng, fs);
#ifdef ZC_GRID_FAIL_NODE
      if (i * ZC_GRID_N + j == ZC_GRID_FAIL_NODE) ok = 0;
#endif
      if (!ok) failed++;
      memcpy(g->n[i][j].b, s.b, sizeof g->n[i][j].b);
      memcpy(g->n[i][j].a, s.a, sizeof g->n[i][j].a);
    }
  }
  return failed;
}

/* --- lookup (audio thread; this is the whole point) -------------------------------------------
 * Bilinear over the four surrounding nodes. Writes ONLY the coefficients: the section's state z[]
 * is untouched, exactly like the designer, so a knob move never resets the filter. */
static inline void zc_grid_lookup(const ZcGridBand *g, double nf, double ng, ZcSection *out) {
  if (nf < 0.0) nf = 0.0; else if (nf > 1.0) nf = 1.0;
  if (ng < 0.0) ng = 0.0; else if (ng > 1.0) ng = 1.0;
  double x = nf * (ZC_GRID_N - 1), y = ng * (ZC_GRID_N - 1);
  int i = (int)x, j = (int)y;
  if (i >= ZC_GRID_N - 1) i = ZC_GRID_N - 2;
  if (j >= ZC_GRID_N - 1) j = ZC_GRID_N - 2;
  double fx = x - i, fy = y - j;
  double w00 = (1.0 - fx) * (1.0 - fy), w10 = fx * (1.0 - fy);
  double w01 = (1.0 - fx) * fy,         w11 = fx * fy;
  const ZcGridNode *n00 = &g->n[i][j],     *n10 = &g->n[i + 1][j];
  const ZcGridNode *n01 = &g->n[i][j + 1], *n11 = &g->n[i + 1][j + 1];
  for (int k = 0; k <= ZC_MAX_N; k++)
    out->b[k] = w00 * n00->b[k] + w10 * n10->b[k] + w01 * n01->b[k] + w11 * n11->b[k];
  for (int k = 0; k <= ZC_MAX_NA; k++)
    out->a[k] = w00 * n00->a[k] + w10 * n10->a[k] + w01 * n01->a[k] + w11 * n11->a[k];
  for (int k = ZC_MAX_NA + 1; k <= ZC_MAX_N; k++) out->a[k] = 0.0;
  out->nb = ZC_MAX_NB; out->na = ZC_MAX_NA;
}
#endif /* ZC_GRID_H */
