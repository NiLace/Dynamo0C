/* ui_panel.h -- Cairo drawing + layout of the Dynamo 0C EQ panel.
 * PURE module: depends on neither Pugl nor LV2; used by the PNG previewer and the LV2 UI.
 * All control values are norm 0..1 (gain 0.5 = 0 dB). The scales printed around
 * each knob are cosmetic (uniform angular spacing; the read-out interpolates between them). */
#ifndef ZC_UI_PANEL_H
#define ZC_UI_PANEL_H

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <cairo/cairo.h>
#include "ports.h"
#include "ui_fonts.h"

/* ---------------- geometry ---------------- */
#define PANEL_W   242
#define COL_L     67      /* center of left column  (FREQ/HP) */
#define COL_R     175     /* center of right column (GAIN/LP) */
#define HEADER_H  54
#define BAND_H    118
#define KNOB_R    25.0
#define TICK_R    39.0
#define PANEL_H   (HEADER_H + 5*BAND_H + 50)   /* 4 bands + filters + switches/footer */

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

/* ---------------- knob table ---------------- */
typedef enum { SK_FREQ, SK_GAIN } SkKind;
typedef struct {
  int port; double cx, cy; const Scale *sc; const char *label; SkKind kind; double def;
  double r; int compact;   /* r=0 -> KNOB_R; compact=1 -> no tick ring (header knob) */
} Knob;

/* DRIVE knob ONLY in the right header (occupies the spot of the old logo slot -> there is no
 * longer a logo). The plugin name goes in the left BRAND. The "DRIVE" label is drawn to the
 * LEFT of the knob (not below), which frees up the height and allows a larger knob. */
#define DRV_CX 210.0
#define DRV_CY 30.0        /* header strip, at the height of the name (raised 2px: optical) */
#define DRV_R  18.0

/* per-band positions (header_y of each block) */
#define BAND_Y(i)   (HEADER_H + (i) * BAND_H)
#define KNOB_CY(i)  (BAND_Y(i) + 62)

enum { K_HI_F, K_HI_G, K_HM_F, K_HM_G, K_LM_F, K_LM_G, K_LO_F, K_LO_G, K_HP, K_LP, K_DRIVE, ZC_NKNOB };
static const Knob KNOBS[ZC_NKNOB] = {
  { P_HI_FREQ,   COL_L, KNOB_CY(0), &SC_HI,    "FREQ", SK_FREQ, 0.5 },
  { P_HI_GAIN,   COL_R, KNOB_CY(0), &SC_GAIN,  "GAIN", SK_GAIN, 0.5 },
  { P_HIMID_FREQ,COL_L, KNOB_CY(1), &SC_HIMID, "FREQ", SK_FREQ, 0.5 },
  { P_HIMID_GAIN,COL_R, KNOB_CY(1), &SC_GAIN,  "GAIN", SK_GAIN, 0.5 },
  { P_LOMID_FREQ,COL_L, KNOB_CY(2), &SC_LOMID, "FREQ", SK_FREQ, 0.4 },
  { P_LOMID_GAIN,COL_R, KNOB_CY(2), &SC_GAIN,  "GAIN", SK_GAIN, 0.5 },
  { P_LO_FREQ,   COL_L, KNOB_CY(3), &SC_LOW,   "FREQ", SK_FREQ, 0.4 },
  { P_LO_GAIN,   COL_R, KNOB_CY(3), &SC_GAIN,  "GAIN", SK_GAIN, 0.5 },
  { P_HP_FREQ,   COL_L, KNOB_CY(4), &SC_HP,    "HP",   SK_FREQ, 0.0 },
  { P_LP_FREQ,   COL_R, KNOB_CY(4), &SC_LP,    "LP",   SK_FREQ, 1.0 },
  { P_DRIVE,     DRV_CX, DRV_CY,    NULL,      "DRIVE",SK_GAIN, 0.0, DRV_R, 1 },
};

/* switch bank (POWER red, rest amber). port2 = a second port toggled in lockstep (-1 = none). */
typedef struct { int port; const char *label; int red; int port2; } Switch;
enum { SW_POWER, SW_LF, SW_HF, SW_EQ, SW_BUMP, ZC_NSW };
static const Switch SWITCHES[ZC_NSW] = {
  { P_POWER,    "POWER", 1, -1 },
  { P_LF_SHELF, "LF",    0, -1 },
  { P_HF_SHELF, "HF",    0, -1 },
  { P_EQ_ON,    "EQ",    0, -1 },
  { P_HP_BUMP,  "BUMP",  0, P_LP_MODE },   /* one switch = resonant knee on BOTH filters (HP bump + LP resonant) */
};
#define SWITCH_Y   (BAND_Y(5) + 4)
#define SWITCH_H   24
#define FOOTER_Y   (SWITCH_Y + SWITCH_H + 18)

/* ---------------- colors ---------------- */
#define ZC_FONT "Roboto Condensed"   /* fallback if the TTFs were not bundled (zc_face[]=NULL) */

/* selects the bundled face for the role; if it was not loaded, falls back to the toy API with the requested weight. */
static inline void zc_setface(cairo_t *cr, ZcFontId f, int bold) {
  if ((unsigned)f < ZF_COUNT && zc_face[f]) cairo_set_font_face(cr, zc_face[f]);
  else cairo_select_font_face(cr, ZC_FONT, CAIRO_FONT_SLANT_NORMAL,
                              bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
}
#define ACC_R 0.851
#define ACC_G 0.541
#define ACC_B 0.227   /* #d98a3a */
static inline void col(cairo_t*c,double r,double g,double b){ cairo_set_source_rgb(c,r,g,b);}
static inline void cola(cairo_t*c,double r,double g,double b,double a){ cairo_set_source_rgba(c,r,g,b,a);}

/* ---------------- text helpers ----------------
 * manual letter-spacing advancing by UTF-8 code-point (not by byte: '·' = 2 bytes). */
static int utf8_len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}
/* copies the next UTF-8 code-point from *p into cl (NUL-terminated, up to 4 bytes) and advances *p */
static void next_glyph(const char **p, char cl[5]) {
  int n = utf8_len((unsigned char)**p), i = 0;
  for (; i < n && (*p)[i]; i++) cl[i] = (*p)[i];
  cl[i] = 0; *p += n;
}
static double txt_width(cairo_t *cr, const char *s, double ls) {
  double total = 0;
  for (const char *p = s; *p; ) {
    char cl[5]; next_glyph(&p, cl);
    cairo_text_extents_t e; cairo_text_extents(cr, cl, &e);
    total += e.x_advance + ls;
  }
  return total > 0 ? total - ls : 0;
}
static void txt(cairo_t *cr, double x, double y, const char *s, double size,
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

/* like txt(), but draws a DOT centered inside each '0' (dotted zero) to distinguish it
 * from the letter O in fonts without a slashed zero (Oswald). The dot inherits the text color. */
static void txt_dot0(cairo_t *cr, double x, double y, const char *s, double size,
                     ZcFontId font, int bold, double r, double g, double b, double ls, int center) {
  zc_setface(cr, font, bold);
  cairo_set_font_size(cr, size);
  double cx = center ? x - txt_width(cr, s, ls) / 2 : x;
  double dr = size * 0.085; if (dr < 0.9) dr = 0.9;   /* dot radius, with a legible minimum */
  for (const char *p = s; *p; ) {
    char cl[5]; next_glyph(&p, cl);
    cairo_text_extents_t e; cairo_text_extents(cr, cl, &e);
    col(cr, r, g, b);
    cairo_move_to(cr, cx, y); cairo_show_text(cr, cl);
    if (cl[0] == '0' && cl[1] == 0) {   /* single-byte ASCII '0' */
      double gx = cx + e.x_bearing + e.width / 2.0;
      double gy = y + e.y_bearing + e.height / 2.0;
      cairo_new_sub_path(cr);
      cairo_arc(cr, gx, gy, dr, 0, 2*M_PI);
      cairo_fill(cr);
    }
    cx += e.x_advance + ls;
  }
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x+w-r, y+r,   r, -M_PI/2, 0);
  cairo_arc(cr, x+w-r, y+h-r, r, 0, M_PI/2);
  cairo_arc(cr, x+r,   y+h-r, r, M_PI/2, M_PI);
  cairo_arc(cr, x+r,   y+r,   r, M_PI, 3*M_PI/2);
  cairo_close_path(cr);
}

/* ---------------- read-out format ---------------- */
static void fmt_value(const Knob *k, double norm, char *out, int n) {
  const Scale *s = k->sc;
  /* linear interp between printed numbers (uniformly spaced on the arc) */
  double pos = norm * (s->n - 1); int i = (int)pos; if (i >= s->n-1) i = s->n-2;
  double fr = pos - i; double val = s->v[i] * (1-fr) + s->v[i+1] * fr;  /* in kHz or dB */
  if (k->kind == SK_GAIN) snprintf(out, n, "%+.1f dB", val);
  else {
    double hz = val * 1000.0;
    if (hz < 1000.0) snprintf(out, n, "%d Hz", (int)(hz + 0.5));
    else snprintf(out, n, "%.2f kHz", val);
  }
}

/* ---------------- knob ---------------- */
static void draw_knob(cairo_t *cr, const Knob *k, double norm, int show_val, double dim) {
  double cx = k->cx, cy = k->cy;
  double a = 1.0 - 0.6 * dim;   /* attenuation if section dimmed */
  double R = (k->r > 0.0) ? k->r : KNOB_R;

  /* ticks + numbers (band knobs only; the compact header one has no ring) */
  if (!k->compact && k->sc) {
    zc_setface(cr, ZF_TICK, 1);
    cairo_set_font_size(cr, 8.0);
    for (int i = 0; i < k->sc->n; i++) {
      double t = (k->sc->n == 1) ? 0.5 : (double)i / (k->sc->n - 1);
      double ang = zc_angle(t);
      double dx = sin(ang), dy = -cos(ang);
      cairo_set_line_width(cr, 1.0);
      cola(cr, 0.83, 0.80, 0.73, a);
      cairo_move_to(cr, cx + dx*(R+3), cy + dy*(R+3));
      cairo_line_to(cr, cx + dx*(R+5), cy + dy*(R+5));
      cairo_stroke(cr);
      if (k->sc->label[i]) {
        char buf[16];
        /* FREQ: scale values are in kHz. Below 1 kHz they are printed in Hz as
         * an integer (no decimals: 40, 70, 100, 160...); at 1 kHz or more, in kHz (1.8, 3, 13). GAIN = dB. */
        if (k->kind == SK_FREQ) {
          double v = k->sc->v[i];
          if (v < 1.0) snprintf(buf, sizeof buf, "%d", (int)(v * 1000.0 + 0.5));
          else         snprintf(buf, sizeof buf, "%g", v);
        } else {
          snprintf(buf, sizeof buf, "%g", k->sc->v[i]);
        }
        cairo_text_extents_t e; cairo_text_extents(cr, buf, &e);
        double tx = cx + dx*TICK_R, ty = cy + dy*TICK_R;
        cola(cr, 0.77, 0.74, 0.67, a);
        cairo_move_to(cr, tx - e.width/2 - e.x_bearing, ty + 3.2);
        cairo_show_text(cr, buf);
      }
    }
  }

  /* fluted skirt (dark) */
  cairo_save(cr);
  cairo_new_sub_path(cr);   /* cuts off any pending current point (caption of the previous knob):
                             * without this, cairo_arc draws a line from that point to the arc. */
  cairo_arc(cr, cx, cy, R, 0, 2*M_PI);
  cola(cr, 0.07, 0.066, 0.063, a); cairo_fill_preserve(cr);
  /* rim lighter than the faceplate (~0.07) so the ring contrasts and the knob edge reads */
  cola(cr, 0.30, 0.285, 0.255, a); cairo_set_line_width(cr, k->compact ? 1.4 : 2); cairo_stroke(cr);
  if (!k->compact) {   /* fluting only on large knobs */
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i < 60; i += 2) {
      double ang = i/60.0*2*M_PI;
      cola(cr, 0.17, 0.165, 0.155, a*0.8);
      cairo_move_to(cr, cx + sin(ang)*(R-2), cy - cos(ang)*(R-2));
      cairo_line_to(cr, cx + sin(ang)*R, cy - cos(ang)*R);
      cairo_stroke(cr);
    }
  }
  cairo_restore(cr);

  /* drive activity ring (compact): amber arc = amount, marks on/off */
  if (k->compact && norm > 0.0001) {
    cola(cr, ACC_R, ACC_G, ACC_B, 0.7*a); cairo_set_line_width(cr, 1.4);
    cairo_new_sub_path(cr); cairo_arc(cr, cx, cy, R+2.5, zc_angle(0)-M_PI/2, zc_angle(norm)-M_PI/2);
    cairo_stroke(cr);
  }

  /* cream cap (radial) */
  double capR = R * 0.74;
  cairo_pattern_t *pat = cairo_pattern_create_radial(cx, cy - capR*0.4, capR*0.1, cx, cy, capR);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.937,0.914,0.847, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.42,0.867,0.839,0.757, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.74,0.761,0.733,0.647, a);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.639,0.612,0.525, a);
  cairo_arc(cr, cx, cy, capR, 0, 2*M_PI);
  cairo_set_source(cr, pat); cairo_fill(cr); cairo_pattern_destroy(pat);

  /* pointer */
  double ang = zc_angle(norm);
  cairo_set_line_width(cr, k->compact ? 1.6 : 2.2); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cola(cr, 0.149, 0.141, 0.122, a);
  cairo_move_to(cr, cx + sin(ang)*(k->compact?2:4), cy - cos(ang)*(k->compact?2:4));
  cairo_line_to(cr, cx + sin(ang)*(capR-2), cy - cos(ang)*(capR-2));
  cairo_stroke(cr);

  /* caption / read-out */
  if (k->compact) {
    /* header knob: label (or readout %) to the LEFT of the knob, vertically centered. */
    char buf[24]; const char *s;
    double fsz = 11.0, ls = 1.2;
    if (show_val) { snprintf(buf, sizeof buf, "%d%%", (int)(norm*100 + 0.5)); s = buf; }
    else s = k->label;
    zc_setface(cr, show_val ? ZF_VALUE : ZF_LABEL, 1);
    cairo_set_font_size(cr, fsz);
    double tw = txt_width(cr, s, ls);
    double lx = cx - R - 12.0 - tw;       /* the text ENDS 12 px to the left of the knob */
    double ty = cy + fsz * 0.35;          /* baseline ~ vertical center of the knob */
    if (show_val)
      txt(cr, lx, ty, s, fsz, ZF_VALUE, 1, ACC_R, ACC_G, ACC_B, ls, 0);
    else {
      double br = (norm < 0.0001) ? 0.5 : 1.0;   /* DRIVE dimmed if at 0 (off) */
      txt(cr, lx, ty, s, fsz, ZF_LABEL, 1,
          (0.718*a+0.1)*br, (0.694*a+0.1)*br, (0.627*a+0.1)*br, ls, 0);
    }
  } else {
    double capy = cy + KNOB_R + 22;
    if (show_val) {
      char buf[24]; fmt_value(k, norm, buf, sizeof buf);
      txt(cr, cx, capy, buf, 13, ZF_VALUE, 1, ACC_R, ACC_G, ACC_B, 0.4, 1);
    } else {
      txt(cr, cx, capy, k->label, 11, ZF_LABEL, 1,
          0.718*a+0.1, 0.694*a+0.1, 0.627*a+0.1, 1.0, 1);
    }
  }
}

/* ---------------- bell/shelf icon ---------------- */
static void draw_shape_icon(cairo_t *cr, double x, double y, int shelf, int high, double alpha) {
  cairo_set_line_width(cr, 1.4); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cola(cr, ACC_R, ACC_G, ACC_B, alpha);
  if (!shelf) {                         /* bell */
    cairo_move_to(cr, x, y+5);
    cairo_curve_to(cr, x+5, y+5, x+5, y-5, x+8, y-5);
    cairo_curve_to(cr, x+11, y-5, x+11, y+5, x+16, y+5);
  } else if (high) {                    /* high-shelf: step that rises to the right */
    cairo_move_to(cr, x, y+5); cairo_line_to(cr, x+8, y+5);
    cairo_curve_to(cr, x+11, y+5, x+11, y-5, x+16, y-5);
  } else {                              /* low-shelf: step that falls to the right */
    cairo_move_to(cr, x, y-5);
    cairo_curve_to(cr, x+5, y-5, x+5, y+5, x+8, y+5);
    cairo_line_to(cr, x+16, y+5);
  }
  cairo_stroke(cr);
}

/* ---------------- hit-boxes of shelf/bell icons (captured at draw time) ----------------
 * The icon's X depends on the label width (measured with cairo), so the UI's hit-testing cannot
 * recompute it without context: zc_draw_panel fills a CALLER-OWNED array and on_press queries it.
 * The array is per instance (passed in), so multiple plugin windows never share state. */
typedef struct { double x0, y0, x1, y1; int port; } ZcHit;

/* ---------------- section header ----------------
 * toggle_port >= 0 -> the band toggles bell<->shelf and its icon is clickable (registers a hit-box). */
static void draw_section(cairo_t *cr, double y, const char *label, int icon, int shelf, int high,
                         double dim, int toggle_port, ZcHit *hits, int *nhits) {
  double a = 1.0 - 0.6*dim;
  zc_setface(cr, ZF_HEAD, 1);
  cairo_set_font_size(cr, 11);
  double lw = txt_width(cr, label, 3.0);    /* width with letter-spacing 3 */
  double cx = PANEL_W/2.0;
  double half = lw/2 + 8 + (icon?12:0);
  /* rules on the sides */
  for (int s = 0; s < 2; s++) {
    double x0 = s ? cx+half : 12;
    double x1 = s ? PANEL_W-12 : cx-half;
    cairo_pattern_t *p = cairo_pattern_create_linear(x0, 0, x1, 0);
    cairo_pattern_add_color_stop_rgba(p, 0, ACC_R,ACC_G,ACC_B, 0.0);
    cairo_pattern_add_color_stop_rgba(p, 0.5, ACC_R,ACC_G,ACC_B, 0.33*a);
    cairo_pattern_add_color_stop_rgba(p, 1, ACC_R,ACC_G,ACC_B, 0.0);
    cairo_set_source(cr, p); cairo_set_line_width(cr,1);
    cairo_move_to(cr, x0, y); cairo_line_to(cr, x1, y); cairo_stroke(cr);
    cairo_pattern_destroy(p);
  }
  txt(cr, cx - (icon?10:0), y+4, label, 11, ZF_HEAD, 1, ACC_R*a+0.05, ACC_G*a+0.05, ACC_B*a+0.05, 3.0, 1);
  if (icon) {
    double ix = cx + lw/2 - 2;
    draw_shape_icon(cr, ix, y, shelf, high, (toggle_port >= 0) ? a : a*0.45);
    if (toggle_port >= 0 && *nhits < 2)
      hits[(*nhits)++] = (ZcHit){ ix - 5, y - 9, ix + 18, y + 9, toggle_port };
  }
}

/* ---------------- switch ---------------- */
/* geometry of switch i (shared by draw_switch and the UI hit-test, so they can't desync) */
static inline void switch_rect(int i, double *x, double *y, double *w, double *h) {
  const double gap = 6, x0 = 12;
  *w = (PANEL_W - 24 - (ZC_NSW-1)*gap) / (double)ZC_NSW;
  *x = x0 + i*(*w + gap);
  *y = SWITCH_Y; *h = SWITCH_H;
}
static void draw_switch(cairo_t *cr, int i, int lit) {
  double x, y, w, h; switch_rect(i, &x, &y, &w, &h);
  rrect(cr, x, y, w, h, 4);
  cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y+h);
  cairo_pattern_add_color_stop_rgb(p, 0, 0.11,0.105,0.10);
  cairo_pattern_add_color_stop_rgb(p, 1, 0.06,0.057,0.054);
  cairo_set_source(cr, p); cairo_fill_preserve(cr); cairo_pattern_destroy(p);
  cola(cr, 0,0,0,0.6); cairo_set_line_width(cr,1); cairo_stroke(cr);
  const Switch *s = &SWITCHES[i];
  double r,g,b;
  if (lit) { if (s->red){r=1.0;g=0.29;b=0.227;} else {r=ACC_R;g=ACC_G;b=ACC_B;} }
  else { r=0.812;g=0.788;b=0.722; }
  txt(cr, x+w/2, y+h/2+3.5, s->label, 9.5, ZF_LABEL, 1, r,g,b, 1.2, 1);
}

/* ---------------- full panel ----------------
 * v   = control values (indexed by port)
 * showk = index of the knob with active read-out (-1 = none) */
static void zc_draw_panel(cairo_t *cr, const float *v, int showk, ZcHit *hits, int *nhits) {
  int power = v[P_POWER] > 0.5f;
  int eq_on = v[P_EQ_ON] > 0.5f;
  double dim_all = power ? 0.0 : 1.0;        /* POWER off -> everything */
  double dim_eq  = (power && eq_on) ? 0.0 : 1.0;  /* EQ off -> bands only */

  /* chassis background */
  cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, PANEL_W*0.5, PANEL_H);
  cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.110,0.106,0.102);
  cairo_pattern_add_color_stop_rgb(bg, 0.55,0.071,0.067,0.063);
  cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.039,0.039,0.035);
  cairo_set_source(cr, bg); cairo_paint(cr); cairo_pattern_destroy(bg);
  rrect(cr, 0.5, 0.5, PANEL_W-1, PANEL_H-1, 11);
  cola(cr, 1,1,1,0.04); cairo_set_line_width(cr,1); cairo_stroke(cr);

  /* header: plugin name "DYNAMO 0C" as brand (left) with DOTTED ZERO; the DRIVE knob
   * goes alone on the right (drawn in the knob loop). There is NO logo anymore. */
  txt_dot0(cr, 14, 32, "DYNAMO 0C", 18, ZF_BRAND, 1, 0.910,0.886,0.824, 0.6, 0);
  txt(cr, 15, 44, "PARAMETRIC EQ", 7.5, ZF_LABEL, 1, 0.561,0.537,0.478, 2.0, 0);

  /* band sections (HIGH/LOW toggle bell<->shelf -> clickable icon; mids always bell) */
  *nhits = 0;
  draw_section(cr, BAND_Y(0)+10, "HIGH",   1, v[P_HF_SHELF]>0.5f, 1, dim_eq, P_HF_SHELF, hits, nhits);
  draw_section(cr, BAND_Y(1)+10, "HI\xC2\xB7MID", 1, 0, 1, dim_eq, -1, hits, nhits);   /* mid: fixed bell, dimmed */
  draw_section(cr, BAND_Y(2)+10, "LO\xC2\xB7MID", 1, 0, 0, dim_eq, -1, hits, nhits);
  draw_section(cr, BAND_Y(3)+10, "LOW",    1, v[P_LF_SHELF]>0.5f, 0, dim_eq, P_LF_SHELF, hits, nhits);
  draw_section(cr, BAND_Y(4)+10, "FILTERS",0, 0, 0, dim_all, -1, hits, nhits);

  /* knobs */
  for (int i = 0; i < ZC_NKNOB; i++) {
    double dim = (i < 8) ? dim_eq : dim_all;   /* HP/LP (8,9) depend only on POWER */
    draw_knob(cr, &KNOBS[i], v[KNOBS[i].port], showk == i, dim);
  }

  /* switches */
  for (int i = 0; i < ZC_NSW; i++) {
    int lit;
    if (i == SW_POWER) lit = power;
    else if (i == SW_EQ) lit = eq_on;
    else lit = v[SWITCHES[i].port] > 0.5f;     /* LF/HF: on = shelf */
    draw_switch(cr, i, lit);
  }

}

#endif /* ZC_UI_PANEL_H */
