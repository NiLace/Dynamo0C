/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* ui_panel.h -- the panel's controls as tables, the knobs' read-out, the text helpers and the OVERSAMPLING card's
 * geometry. PURE module: depends on neither Pugl nor LV2; used by the LV2 UI (through ui_photo.h), the PNG previewer
 * and the scene's dump of the controls (tools/dump_panel.c).
 * All control values are norm 0..1 (gain 0.5 = the centre detent). The scales printed around each knob are the
 * console's legend, cosmetic and uniformly spaced; the read-out is computed from the prototypes (see fmt_value). */
#ifndef ZC_UI_PANEL_H
#define ZC_UI_PANEL_H

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <cairo/cairo.h>
#include "ports.h"
#include "analog.h"
#include "ui_fonts.h"

/* knob angle: norm 0..1 -> -135..+135 degrees */
static inline double zc_angle(double norm) { return (-135.0 + norm * 270.0) * M_PI / 180.0; }

/* ---------------- scales (cosmetic) ---------------- */
typedef struct { int n; double v[8]; int label[8]; } Scale;   /* label=1 prints the number */
static const Scale SC_HI    = {6, {1.8,3,5,7,10,13},        {1,1,1,1,1,1}};
static const Scale SC_HIMID = {6, {.45,.8,1.2,1.8,2.8,4},   {1,1,1,1,1,1}};
static const Scale SC_LOMID = {6, {.2,.4,.7,1.2,2,3.1},     {1,1,1,1,1,1}};
static const Scale SC_LOW   = {6, {.04,.07,.1,.16,.28,.45}, {1,1,1,1,1,1}};
static const Scale SC_GAIN  = {7, {-10,-6.5,-3,0,3,6.5,10}, {1,0,1,1,1,0,1}};
static const Scale SC_HP    = {6, {.025,.05,.1,.25,.5,1},   {1,1,1,1,1,1}};
static const Scale SC_LP    = {7, {.16,.4,1,2.5,6,13,20},   {1,1,1,1,1,1,1}};

/* ---------------- knob table ----------------
 * The order is the panel's: the scene that renders the panel reads it (tools/dump_panel.c), and the generated layout
 * (ui_photo_layout.h) lists the knobs' positions in the same order. compact = no printed scale (DRIVE). */
typedef enum { SK_FREQ, SK_GAIN } SkKind;
typedef struct { int port; const Scale *sc; const char *label; SkKind kind; int compact; } Knob;   /* defaults: port_defaults.h (the TTL) */
enum { K_HI_F, K_HI_G, K_HM_F, K_HM_G, K_LM_F, K_LM_G, K_LO_F, K_LO_G, K_HP, K_LP, K_DRIVE, ZC_NKNOB };
static const Knob KNOBS[ZC_NKNOB] = {
  { P_HI_FREQ,    &SC_HI,    "FREQ",  SK_FREQ, 0 },
  { P_HI_GAIN,    &SC_GAIN,  "GAIN",  SK_GAIN, 0 },
  { P_HIMID_FREQ, &SC_HIMID, "FREQ",  SK_FREQ, 0 },
  { P_HIMID_GAIN, &SC_GAIN,  "GAIN",  SK_GAIN, 0 },
  { P_LOMID_FREQ, &SC_LOMID, "FREQ",  SK_FREQ, 0 },
  { P_LOMID_GAIN, &SC_GAIN,  "GAIN",  SK_GAIN, 0 },
  { P_LO_FREQ,    &SC_LOW,   "FREQ",  SK_FREQ, 0 },
  { P_LO_GAIN,    &SC_GAIN,  "GAIN",  SK_GAIN, 0 },
  { P_HP_FREQ,    &SC_HP,    "HP",    SK_FREQ, 0 },
  { P_LP_FREQ,    &SC_LP,    "LP",    SK_FREQ, 0 },
  { P_DRIVE,      NULL,      "DRIVE", SK_GAIN, 1 },
};

/* ---------------- the buttons ----------------
 * port2 = a second port toggled in lockstep (-1 = none): BUMP sets the HP bump and the LP resonance together. */
typedef struct { int port; const char *label; int port2; } Switch;
enum { SW_LF, SW_HF, SW_EQ, SW_BUMP, ZC_NSW };
static const Switch SWITCHES[ZC_NSW] = {
  { P_LF_SHELF, "LF",   -1 },
  { P_HF_SHELF, "HF",   -1 },
  { P_EQ_ON,    "EQ",   -1 },
  { P_HP_BUMP,  "BUMP", P_LP_MODE },
};
#define SET_NOPT 3            /* drive oversampling: 1x/2x/4x -> P_DRIVE_OS values 0/1/2 */
static const char *SET_OS_LBL[SET_NOPT] = { "1x", "2x", "4x" };

/* ---------------- fonts and colours ---------------- */
#define ZC_FONT "Roboto Condensed"   /* fallback if the bundled fonts did not load (zc_face[] NULL) */

/* selects the bundled face for the role; if it was not loaded, falls back to the toy API with the requested weight. */
static inline void zc_setface(cairo_t *cr, ZcFontId f, int bold) {
  if ((unsigned)f < ZF_COUNT && zc_face[f]) cairo_set_font_face(cr, zc_face[f]);
  else cairo_select_font_face(cr, ZC_FONT, CAIRO_FONT_SLANT_NORMAL,
                              bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  /* HINT_METRICS OFF fixes the letter spacing: by default cairo rounds every x_advance to a whole pixel, which at
   * small sizes inflates some letters and shrinks others (measured on "DRIVE" at 11 px: 5.19/5.07/2.46/5.18/4.79
   * became 6/6/3/5/5, and it read "D RIVE"); the panel also scales with the window, so integer metrics in the
   * logical space are wrong whenever the scale is not 1:1. */
  cairo_font_options_t *fo = cairo_font_options_create();
  cairo_get_font_options(cr, fo);
  cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
  cairo_set_font_options(cr, fo);
  cairo_font_options_destroy(fo);
}
#define ACC_R 0.851
#define ACC_G 0.541
#define ACC_B 0.227   /* #d98a3a, the 0C family's amber */
static inline void col(cairo_t*c,double r,double g,double b){ cairo_set_source_rgb(c,r,g,b);}
static inline void cola(cairo_t*c,double r,double g,double b,double a){ cairo_set_source_rgba(c,r,g,b,a);}

/* ---------------- text helpers ----------------
 * manual letter-spacing, advancing by UTF-8 code point (not by byte: '·' = 2 bytes). */
static inline int utf8_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}
/* copies the next UTF-8 code point from *p into cl (NUL-terminated, up to 4 bytes) and advances *p */
static inline void next_glyph(const char **p, char cl[5]) {
  int n = utf8_len((unsigned char)**p), i = 0;
  for (; i < n && (*p)[i]; i++) cl[i] = (*p)[i];
  cl[i] = 0; *p += i;
}
static inline double txt_width(cairo_t *cr, const char *s, double ls) {
  double total = 0;
  for (const char *p = s; *p; ) {
    char cl[5]; next_glyph(&p, cl);
    cairo_text_extents_t e; cairo_text_extents(cr, cl, &e);
    total += e.x_advance + ls;
  }
  return total > 0 ? total - ls : 0;
}
static inline void txt(cairo_t *cr, double x, double y, const char *s, double size,
                       ZcFontId font, int bold, double r, double g, double b, double ls, int center) {
  zc_setface(cr, font, bold);
  cairo_set_font_size(cr, size);
  double cx = center ? x - txt_width(cr, s, ls) / 2 : x;
  col(cr, r, g, b);
  for (const char *p = s; *p; ) {
    char cl[5]; next_glyph(&p, cl);
    cairo_text_extents_t e; cairo_text_extents(cr, cl, &e);
    cairo_move_to(cr, cx, y); cairo_show_text(cr, cl);
    cx += e.x_advance + ls;
  }
}
static inline void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x+w-r, y+r,   r, -M_PI/2, 0);
  cairo_arc(cr, x+w-r, y+h-r, r, 0, M_PI/2);
  cairo_arc(cr, x+r,   y+h-r, r, M_PI/2, M_PI);
  cairo_arc(cr, x+r,   y+r,   r, M_PI, 3*M_PI/2);
  cairo_close_path(cr);
}

/* ---------------- read-out ----------------
 * The number shown while a knob moves is what the plugin really does, computed from the same analogue prototypes the
 * DSP designs from (analog.h): a band's peak or plateau gain at its current frequency and shape, where the band acts,
 * and the HP/LP corner from the DSP's own table. The scale printed around each knob stays the console's legend; it is
 * cosmetic and the read-out does not interpolate it. v[] is the port array; a non-finite or out-of-range value reads
 * as the nearest end, so no port value can index outside a table. */
static inline double zc_unit(float x) { return (x > 0.0f) ? (x < 1.0f ? (double)x : 1.0) : 0.0; }   /* NaN -> 0 */
static inline void fmt_hz(double hz, char *out, int n) {
  if (hz < 1000.0) snprintf(out, n, "%d Hz", (int)(hz + 0.5));
  else snprintf(out, n, "%.2f kHz", hz / 1000.0);
}
static inline void fmt_value(const Knob *k, const float *v, char *out, int n) {
#ifdef ZC_READOUT_FROM_SCALE
  /* the old read-out, kept only as make readout's negative arm: it interpolated the printed legend */
  if (k->sc) {
    const Scale *s = k->sc; double pos = zc_unit(v[k->port]) * (s->n - 1); int i = (int)pos; if (i > s->n - 2) i = s->n - 2;
    double val = s->v[i] + (pos - i) * (s->v[i + 1] - s->v[i]);
    if (k->kind == SK_GAIN) snprintf(out, n, "%+.1f dB", val); else fmt_hz(val * 1000.0, out, n);
    return;
  }
#endif
  int band = -1, shelf_port = -1;
  switch (k->port) {
    case P_LO_FREQ:    case P_LO_GAIN:    band = 0; shelf_port = P_LF_SHELF; break;
    case P_LOMID_FREQ: case P_LOMID_GAIN: band = 1; break;
    case P_HIMID_FREQ: case P_HIMID_GAIN: band = 2; break;
    case P_HI_FREQ:    case P_HI_GAIN:    band = 3; shelf_port = P_HF_SHELF; break;
    default: break;
  }
  if (band >= 0) {
    const int fport = (band == 0) ? P_LO_FREQ : (band == 1) ? P_LOMID_FREQ : (band == 2) ? P_HIMID_FREQ : P_HI_FREQ;
    const int gport = (band == 0) ? P_LO_GAIN : (band == 1) ? P_LOMID_GAIN : (band == 2) ? P_HIMID_GAIN : P_HI_GAIN;
    const int shelf = shelf_port >= 0 && v[shelf_port] > 0.5f;
    if (k->kind == SK_GAIN) {
      double g; zc_band_readout(band, shelf, zc_unit(v[fport]), zc_unit(v[gport]), &g, NULL);
      snprintf(out, n, "%+.1f dB", g);
    } else {
      double f; zc_band_readout(band, shelf, zc_unit(v[fport]), 1.0, NULL, &f);
      fmt_hz(f, out, n);
    }
    return;
  }
  const double x = zc_unit(v[k->port]);
  if (k->port == P_HP_FREQ) {
    if (x <= 0.0005) snprintf(out, n, "off");                     /* the DSP's own engage threshold */
    else fmt_hz(zc_interp(x, ZC_HP_NORM, ZC_HP_HZ, ZC_ARRLEN(ZC_HP_NORM)), out, n);
  } else if (k->port == P_LP_FREQ) {
    if (x >= 0.9995) snprintf(out, n, "off");
    else fmt_hz(zc_interp(x, ZC_LP_NORM, ZC_LP_HZ, ZC_ARRLEN(ZC_LP_NORM)), out, n);
  } else {
    snprintf(out, n, "%d %%", (int)(x * 100.0 + 0.5));
  }
}

/* ---------------- the OVERSAMPLING card ----------------
 * Opened by clicking the DRIVE label; closed by the X, a segment, or a click outside. Its own units, origin at its
 * top-left corner: ui_photo.h maps them onto the panel image (and a click back). */
#define SET_CARD_W 210.0
#define SET_CARD_H 108.0
#define SET_CARD_X 0.0
#define SET_CARD_Y 0.0
#define SET_CLOSE_CX (SET_CARD_X + SET_CARD_W - 17.0)
#define SET_CLOSE_CY (SET_CARD_Y + 17.0)
#define SET_CLOSE_R  10.0     /* click hotspot radius (bigger than the drawn glyph, on purpose) */
static inline void set_seg_rect(int i, double *x, double *y, double *w, double *h) {
  const double m = 16.0, gap = 8.0;
  double tw = SET_CARD_W - 2*m;
  *w = (tw - (SET_NOPT-1)*gap) / (double)SET_NOPT;
  *x = SET_CARD_X + m + i*(*w + gap);
  *y = SET_CARD_Y + 58.0; *h = 30.0;
}
static inline int set_in_card(double x, double y) {
  return x >= SET_CARD_X && x <= SET_CARD_X+SET_CARD_W && y >= SET_CARD_Y && y <= SET_CARD_Y+SET_CARD_H;
}

#endif /* ZC_UI_PANEL_H */
