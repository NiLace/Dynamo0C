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
 *   - memory     0.58 MB per sample rate, STATIC AND SHARED -- about seven instances' worth, so a
 *                per-instance copy would be unaffordable in a session with dozens of them
 *   - fill time  0.72 s, which is why it is not filled in instantiate()
 *   - accuracy   p95 -37.2 dB against the exact design, which is deliberate and is only ever heard
 *                DURING a gesture: the section is redesigned exactly as soon as the knob stops
 *   - stability  every interpolated pole stays inside the unit circle
 *
 * At twice the density (65 nodes per axis) the same probe measures median -71.4 dB, p95 -52.0 dB
 * and worst -35.3 dB, for 2.32 MB and 2.87 s. Both sets come from compare/grid_interp.c over 13824
 * points chosen strictly between nodes, never on one.
 *
 * THE ACCURACY FIGURES ARE A PROPERTY OF THE DENSITY, not of interpolation in general. Interpolating
 * the denominator between two arbitrary designs is not safe as a technique; it is safe here because
 * the nodes are close together. Changing ZC_GRID_N invalidates every number above.
 */
#ifndef ZC_GRID_H
#define ZC_GRID_H
#include <string.h>

/* 33 nodes per axis, not the 65 the probe recommended, and the reason is architectural.
 * The probe judged the grid as a REPLACEMENT for the designer, where the interpolation error is
 * what you listen to permanently, and at that standard 33 is too coarse. Here the interpolated
 * filter only exists WHILE THE KNOB IS MOVING: the moment it stops, the section is redesigned
 * exactly. What matters during a gesture is that it does not step or crunch rather than that it
 * lands within a fraction of a dB, and that is what compare/knob_gate.c measures.
 *
 * The saving is not cosmetic: 2.32 MB -> 0.58 MB and 2.87 s -> 0.72 s of background fill, a
 * quarter of the memory and a quarter of the time that thread exists at all. */
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

/* --- filling (NOT on the audio thread: 0.72 s) ------------------------------------------------ */
static inline void zc_grid_fill_one(ZcGridBand *g, int which, double fs) {
  ZcSection s;
  for (int i = 0; i < ZC_GRID_N; i++) {
    double nf = (double)i / (ZC_GRID_N - 1);
    for (int j = 0; j < ZC_GRID_N; j++) {
      double ng = (double)j / (ZC_GRID_N - 1);
      if (which <= ZC_GB_HI)
        zc_section_build_bellshelf(&s, ZC_BELL, zc_ftap_band(which), zc_wtap_band(which),
                                   zc_cap_band(which), nf, ng, fs);
      else if (which == ZC_GB_LOSHELF)
        zc_section_build_bellshelf(&s, ZC_LOSHELF, ZC_FTAP_LO_SHELF, ZC_WTAP_LO_SHELF,
                                   ZC_CAP_LO_SHELF, nf, ng, fs);
      else
        zc_section_build_bellshelf(&s, ZC_HISHELF, ZC_FTAP_HI_SHELF, ZC_WTAP_HI_SHELF,
                                   ZC_CAP_HI_SHELF, nf, ng, fs);
      memcpy(g->n[i][j].b, s.b, sizeof g->n[i][j].b);
      memcpy(g->n[i][j].a, s.a, sizeof g->n[i][j].a);
    }
  }
}
static inline void zc_grid_fill(ZcGrid *g, double fs) {
  g->fs = fs;
  for (int w = 0; w < ZC_GB_N; w++) zc_grid_fill_one(&g->band[w], w, fs);
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
