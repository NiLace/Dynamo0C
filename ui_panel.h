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

/* ---- the DRIVE label is a CONTROL: clicking it opens the Oversampling card ----------------
 * Family pattern ("labels can BE controls"), same as DynaColor's "COLOUR DRIVE"/"BUS EQ" and the
 * title=bypass here. Geometry SHARED by the draw code and ui.c's hit-test so they cannot desync.
 * The label is drawn RIGHT-ALIGNED, ending DRV_LBL_GAP px left of the knob -> its right edge is
 * fixed and only the left edge moves with the text.
 * The box is deliberately the UNION of every label width ("DRIVE" = 29.8 px at 1x, "DRIVE 4x" =
 * 46.4 px) instead of tracking the current text: the click target must not jump around when the
 * OS changes. Measured at 11px/ZF_LABEL/ls=1.2 -> 50 covers the widest with room to spare.
 * ⚠ It ends where title_rect() begins: see the note there, the two boxes are neighbours. */
#define DRV_LBL_GAP 12.0
#define DRV_LBL_R   (DRV_CX - DRV_R - DRV_LBL_GAP)   /* right edge of the label = 180 */
#define DRV_LBL_W   50.0
static inline void drive_label_rect(double *x, double *y, double *w, double *h) {
  *x = DRV_LBL_R - DRV_LBL_W; *w = DRV_LBL_W;   /* 130 .. 180 */
  *y = DRV_CY - 11.0;         *h = 22.0;        /* 19 .. 41, centred on the knob */
}

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

/* switch bank. port2 = a second port toggled in lockstep (-1 = none).
 * The old POWER button is gone: bypass moved to the TITLE (family pattern). The DRIVE
 * OVERSAMPLING selector briefly occupied that freed slot as a dropdown cell; it now lives back
 * in its settings card (zc_draw_settings), opened from the DRIVE label -- the family pattern is
 * "labels can BE controls". So the bank is four plain toggles again, and switch_rect() shares the
 * width between ZC_NSW cells, which is what makes the four survivors wider. */
typedef struct { int port; const char *label; int red; int port2; } Switch;
enum { SW_LF, SW_HF, SW_EQ, SW_BUMP, ZC_NSW };
static const Switch SWITCHES[ZC_NSW] = {
  { P_LF_SHELF, "LF",    0, -1 },
  { P_HF_SHELF, "HF",    0, -1 },
  { P_EQ_ON,    "EQ",    0, -1 },
  { P_HP_BUMP,  "BUMP",  0, P_LP_MODE },   /* one switch = resonant knee on BOTH filters (HP bump + LP resonant) */
};
#define SET_NOPT 3            /* drive oversampling: 1x(off)/2x/4x -> P_DRIVE_OS values 0/1/2 */
static const char *SET_OS_LBL[SET_NOPT] = { "1x", "2x", "4x" };
#define SWITCH_Y   (BAND_Y(5) + 4)
#define SWITCH_H   24

/* ---------------- colors ---------------- */
#define ZC_FONT "Roboto Condensed"   /* fallback if the TTFs were not bundled (zc_face[]=NULL) */

/* selects the bundled face for the role; if it was not loaded, falls back to the toy API with the requested weight. */
static inline void zc_setface(cairo_t *cr, ZcFontId f, int bold) {
  if ((unsigned)f < ZF_COUNT && zc_face[f]) cairo_set_font_face(cr, zc_face[f]);
  else cairo_select_font_face(cr, ZC_FONT, CAIRO_FONT_SLANT_NORMAL,
                              bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
  /* ⚠ HINT_METRICS OFF — no es cosmetica, arregla el espaciado de letras.
   * Por defecto cairo REDONDEA cada x_advance a un ENTERO de pixel. A 11px eso destroza el
   * espaciado con letter-spacing: medido en "DRIVE", los avances reales son
   *   D=5.192 R=5.071 I=2.464 V=5.181 E=4.785
   * y el hinting los aplasta a 6,6,3,5,5 => infla la D +0.81 px y la R +0.93, pero ENCOGE la V
   * -0.18. El resultado se lee "D RIVE": el hueco D-R medido era 2.33 px contra 2.00 del I-V.
   * Con las metricas sin hintear los avances son fraccionarios y el espaciado sale parejo.
   * Ademas el panel ESCALA con la ventana: hintear metricas a enteros del espacio LOGICO es
   * incorrecto de todas formas cuando la escala no es 1:1. */
  cairo_font_options_t *fo = cairo_font_options_create();
  cairo_get_font_options(cr, fo);
  cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
  cairo_set_font_options(cr, fo);
  cairo_font_options_destroy(fo);
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

/* text with alpha (glow layers build on this) */
static void txt_a(cairo_t *cr, double x, double y, const char *s, double size,
                  ZcFontId font, int bold, double r, double g, double b, double al, double ls, int center) {
  zc_setface(cr, font, bold); cairo_set_font_size(cr, size);
  double cx = center ? x - txt_width(cr, s, ls)/2 : x; cola(cr, r, g, b, al);
  for (const char *p = s; *p;) { char cl[5]; next_glyph(&p, cl); cairo_text_extents_t e;
    cairo_text_extents(cr, cl, &e); cairo_move_to(cr, cx, y); cairo_show_text(cr, cl); cx += e.x_advance + ls; }
}
/* fake text-shadow: Cairo has no blur, so stack faint offset copies that HUG the glyphs (~0.9px)
 * -- a wide halo just smears legibility -- then the crisp text on top. */
static const double GLOW_OFF[8][2] = {{-0.9,0},{0.9,0},{0,-0.9},{0,0.9},{-0.7,-0.7},{0.7,-0.7},{-0.7,0.7},{0.7,0.7}};
static void txt_glow(cairo_t *cr, double x, double y, const char *s, double size,
                     ZcFontId font, int bold, double r, double g, double b, double ls, int center) {
  for (int k = 0; k < 8; k++) txt_a(cr, x+GLOW_OFF[k][0], y+GLOW_OFF[k][1], s, size, font, bold, r, g, b, 0.09, ls, center);
  txt(cr, x, y, s, size, font, bold, r, g, b, ls, center);
}
/* same, for the dotted-zero brand renderer (the title needs the dotted 0 AND the halo) */
static void txt_dot0_a(cairo_t *cr, double x, double y, const char *s, double size,
                       ZcFontId font, int bold, double r, double g, double b, double al, double ls, int center) {
  zc_setface(cr, font, bold);
  cairo_set_font_size(cr, size);
  double cx = center ? x - txt_width(cr, s, ls) / 2 : x;
  double dr = size * 0.085; if (dr < 0.9) dr = 0.9;
  for (const char *p = s; *p; ) {
    char cl[5]; next_glyph(&p, cl);
    cairo_text_extents_t e; cairo_text_extents(cr, cl, &e);
    cola(cr, r, g, b, al);
    cairo_move_to(cr, cx, y); cairo_show_text(cr, cl);
    if (cl[0] == '0' && cl[1] == 0) {
      double gx = cx + e.x_bearing + e.width/2.0, gy = y + e.y_bearing + e.height/2.0;
      cairo_new_sub_path(cr); cairo_arc(cr, gx, gy, dr, 0, 2*M_PI); cairo_fill(cr);
    }
    cx += e.x_advance + ls;
  }
}
static void txt_dot0_glow(cairo_t *cr, double x, double y, const char *s, double size,
                          ZcFontId font, int bold, double r, double g, double b, double ls, int center) {
  for (int k = 0; k < 8; k++) txt_dot0_a(cr, x+GLOW_OFF[k][0], y+GLOW_OFF[k][1], s, size, font, bold, r, g, b, 0.09, ls, center);
  txt_dot0(cr, x, y, s, size, font, bold, r, g, b, ls, center);
}
static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x+w-r, y+r,   r, -M_PI/2, 0);
  cairo_arc(cr, x+w-r, y+h-r, r, 0, M_PI/2);
  cairo_arc(cr, x+r,   y+h-r, r, M_PI/2, M_PI);
  cairo_arc(cr, x+r,   y+r,   r, M_PI, 3*M_PI/2);
  cairo_close_path(cr);
}

/* ---------------- backdrop + drop shadow (window background, INDEPENDENT of the chassis) --------
 * Dynamo 0C has no handoff bundle of its own, but it IS the "0C Mixer EQ" that BOTH sibling
 * handoffs (DynaComp 0C, Dynacolor 0C) name as the family anchor: "Match the 0C Mixer EQ exactly
 * on: chassis material/shadows, accent #d98a3a, Oswald/Barlow type, the shared Knob component,
 * the section-header rule pattern, and the switch-bank style." So the tokens below are theirs,
 * verbatim -- not invented by analogy. */
#define UI_MARGIN 20.0
static void zc_backdrop(cairo_t *cr, double w, double h) {
  (void)h;
  /* radial-gradient(120% 80% at 50% 0%, #1b1b1d, #0c0c0d 60%, #060606) -- NEUTRAL; the amber is
   * the ACCENT only, never the room. */
  cairo_pattern_t *bd = cairo_pattern_create_radial(w*0.5, 0, w*0.05, w*0.5, 0, w*1.15);
  cairo_pattern_add_color_stop_rgb(bd, 0.0, 0.106,0.106,0.114);   /* #1b1b1d */
  cairo_pattern_add_color_stop_rgb(bd, 0.6, 0.047,0.047,0.051);   /* #0c0c0d */
  cairo_pattern_add_color_stop_rgb(bd, 1.0, 0.024,0.024,0.024);   /* #060606 */
  cairo_set_source(cr, bd); cairo_paint(cr); cairo_pattern_destroy(bd);
}
/* drop shadow around the chassis (chassis-local coords): 0 26px 60px rgba(0,0,0,.6) */
static void zc_shadow(cairo_t *cr) {
  for (int i = 12; i >= 1; i--) { double g = i*2.6;
    rrect(cr, -g, -g+13, PANEL_W+2*g, PANEL_H+2*g, 11+g);
    cairo_set_source_rgba(cr, 0,0,0, 0.055); cairo_fill(cr); }
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

/* ---------------- knob skirt cache ------------------------------------------------------------
 * The fluted skirt is identical for every knob of a given radius and never depends on a port value.
 * This panel has ELEVEN knobs = ~660 fills per repaint, by far its biggest cost. Cache it per
 * radius (R is a unique key here: KNOB_R=25 fluted vs DRV_R=18 compact/unfluted) and blit.
 *
 * RENDERED AT DEVICE RESOLUTION, and keyed on the scale as well as the radius. The panel is drawn
 * through a scale transform (the window is resizable), so a surface sized in USER units gets
 * upscaled at blit time and the flutes go visibly soft -- exactly what happened the first time.
 * Deliberately process-lifetime and SHARED BY ALL INSTANCES of this .so (a session can hold ~30;
 * freeing it on one window's cleanup would be a use-after-free for the others). */
#define SK_CACHE_N 8
#define SK_MARGIN  8.0
static cairo_surface_t *sk_surf[SK_CACHE_N];
static double           sk_rad[SK_CACHE_N], sk_scl[SK_CACHE_N];
static int              sk_n = 0;
static void skirt_draw(cairo_t *c, double cx, double cy, double R, double a, int compact){
  for (int i = 6; i >= 1; i--) { double g = i*1.35;
    cairo_new_sub_path(c); cairo_arc(c, cx, cy+2.5, R+g*0.55, 0, 2*M_PI); cola(c, 0,0,0, 0.10*a); cairo_fill(c); }
  cairo_new_sub_path(c); cairo_arc(c, cx, cy, R+2.0, 0, 2*M_PI); cola(c, 0.024,0.024,0.020, a); cairo_fill(c);
  cairo_new_sub_path(c); cairo_arc(c, cx, cy, R, 0, 2*M_PI); cola(c, 0.071,0.067,0.063, a); cairo_fill(c);
  if (!compact) {
    for (int i = 0; i < 60; i++) {
      double a0 = -M_PI/2.0 + i*(6.0*M_PI/180.0), a1 = a0 + 2.6*M_PI/180.0;
      cairo_new_sub_path(c); cairo_move_to(c, cx, cy); cairo_arc(c, cx, cy, R, a0, a1); cairo_close_path(c);
      cola(c, 0.169,0.165,0.153, a); cairo_fill(c);
    }
  }
  cairo_save(c); cairo_new_sub_path(c); cairo_arc(c, cx, cy, R, 0, 2*M_PI); cairo_clip(c);
  cola(c, 1,1,1, 0.14*a); cairo_set_line_width(c, 1.0);
  cairo_new_sub_path(c); cairo_arc(c, cx, cy, R-0.5, -M_PI*0.92, -M_PI*0.08); cairo_stroke(c);
  cairo_pattern_t *bs = cairo_pattern_create_linear(0, cy+R*0.25, 0, cy+R);
  cairo_pattern_add_color_stop_rgba(bs, 0, 0,0,0, 0.0);
  cairo_pattern_add_color_stop_rgba(bs, 1, 0,0,0, 0.55*a);
  cairo_new_sub_path(c); cairo_arc(c, cx, cy, R, 0, 2*M_PI); cairo_set_source(c, bs); cairo_fill(c);
  cairo_pattern_destroy(bs); cairo_restore(c);
}
static void skirt_blit(cairo_t *cr, double cx, double cy, double R, double a, int compact){
  cairo_matrix_t m; cairo_get_matrix(cr, &m);
  double sc = hypot(m.xx, m.yx); if (sc < 0.05) sc = 1.0;
  cairo_surface_t *sk = NULL;
  for (int i = 0; i < sk_n; i++) if (sk_rad[i]==R && sk_scl[i]==sc) { sk = sk_surf[i]; break; }
  if (!sk) {
    if (sk_n >= SK_CACHE_N) { skirt_draw(cr, cx, cy, R, a, compact); return; }
    int D = (int)(2.0*(R+SK_MARGIN)*sc + 0.5);
    sk = cairo_surface_create_similar(cairo_get_target(cr), CAIRO_CONTENT_COLOR_ALPHA, D, D);
    cairo_t *c = cairo_create(sk);
    cairo_scale(c, sc, sc);
    skirt_draw(c, R+SK_MARGIN, R+SK_MARGIN, R, 1.0, compact);
    cairo_destroy(c);
    sk_surf[sk_n]=sk; sk_rad[sk_n]=R; sk_scl[sk_n]=sc; sk_n++;
  }
  cairo_save(cr);
  cairo_translate(cr, cx-(R+SK_MARGIN), cy-(R+SK_MARGIN));
  cairo_scale(cr, 1.0/sc, 1.0/sc);
  cairo_set_source_surface(cr, sk, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint_with_alpha(cr, a);
  cairo_restore(cr);
}

/* ---------------- knob ---------------- */
/* The knob splits in two so the panel can cache what never moves:
 *   draw_knob_static -- ticks, labels, skirt (itself cached) and the domed cap. The tick positions
 *                       come from the knob's scale, not from its value, so none of this moves.
 *   draw_knob_live   -- the indicator line and the caption/read-out: all a value change moves. */
static void draw_knob_static(cairo_t *cr, const Knob *k, double dim) {
  double cx = k->cx, cy = k->cy;
  double a = 1.0 - 0.6 * dim;   /* attenuation if section dimmed */
  double R = (k->r > 0.0) ? k->r : KNOB_R;

  /* ticks + numbers (band knobs only; the compact header one has no ring) */
  if (!k->compact && k->sc) {
    /* ticks: 2x8px marks (#d3ccba, opacity .8) centred at size/2+5 -- shared Knob spec */
    zc_setface(cr, ZF_TICK, 1);
    cairo_set_font_size(cr, 9.5);
    for (int i = 0; i < k->sc->n; i++) {
      double t = (k->sc->n == 1) ? 0.5 : (double)i / (k->sc->n - 1);
      double ang = zc_angle(t);
      double dx = sin(ang), dy = -cos(ang);
      double markR = R + 5.0;
      cairo_set_line_width(cr, 2.0); cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
      cola(cr, 0.827, 0.800, 0.729, 0.8*a);
      cairo_move_to(cr, cx + dx*(markR-4.0), cy + dy*(markR-4.0));
      cairo_line_to(cr, cx + dx*(markR+4.0), cy + dy*(markR+4.0));
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
        double tx = cx + dx*TICK_R, ty = cy + dy*TICK_R;   /* labels: 9.5px #c4beac */
        cola(cr, 0.769, 0.745, 0.675, a);
        cairo_move_to(cr, tx - e.width/2 - e.x_bearing, ty + 3.4);
        cairo_show_text(cr, buf);
      }
    }
  }

  cairo_new_sub_path(cr);   /* cuts off any pending current point (previous caption) */
  skirt_blit(cr, cx, cy, R, a, k->compact);   /* fluted skirt, device-resolution cache */


  /* cream domed cap (radial) + 0 1px 2px rgba(0,0,0,.45) */
  double capR = R * 0.74;
  cairo_new_sub_path(cr); cairo_arc(cr, cx, cy+1.0, capR, 0, 2*M_PI); cola(cr, 0,0,0, 0.45*a); cairo_fill(cr);
  cairo_pattern_t *pat = cairo_pattern_create_radial(cx, cy - capR*0.4, capR*0.1, cx, cy, capR);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.937,0.914,0.847, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.42,0.867,0.839,0.757, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.74,0.761,0.733,0.647, a);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.639,0.612,0.525, a);
  cairo_arc(cr, cx, cy, capR, 0, 2*M_PI);
  cairo_set_source(cr, pat); cairo_fill(cr); cairo_pattern_destroy(pat);
  /* cap insets: inset 0 2px 3px rgba(255,255,255,.6) top, inset 0 -4px 8px rgba(0,0,0,.28) bottom */
  cairo_save(cr); cairo_new_sub_path(cr); cairo_arc(cr, cx, cy, capR, 0, 2*M_PI); cairo_clip(cr);
  cola(cr, 1,1,1, 0.6*a); cairo_set_line_width(cr, 1.4);
  cairo_new_sub_path(cr); cairo_arc(cr, cx, cy, capR-0.7, -M_PI*0.88, -M_PI*0.12); cairo_stroke(cr);
  cairo_pattern_t *cs = cairo_pattern_create_linear(0, cy+capR*0.1, 0, cy+capR);
  cairo_pattern_add_color_stop_rgba(cs, 0, 0,0,0, 0.0);
  cairo_pattern_add_color_stop_rgba(cs, 1, 0,0,0, 0.28*a);
  cairo_new_sub_path(cr); cairo_arc(cr, cx, cy, capR, 0, 2*M_PI); cairo_set_source(cr, cs); cairo_fill(cr); cairo_pattern_destroy(cs);
  cairo_restore(cr);
}

/* drive_os is only read for the compact header knob: its label doubles as the OS read-out
 * ("DRIVE" at 1x -- the default -- then "DRIVE 2x" / "DRIVE 4x"). Pass 0 for every other knob.
 * NOTE this lives in the LIVE pass, so the OS does NOT belong in the static cache key. */
static void draw_knob_live(cairo_t *cr, const Knob *k, double norm, int show_val, double dim,
                           int drive_os, int card_open) {
  double cx = k->cx, cy = k->cy, a = 1.0 - 0.6*dim;
  double R = (k->r > 0.0) ? k->r : KNOB_R;
  double capR = R * 0.74;

  /* drive activity ring (compact): amber arc = amount. FOLLOWS THE VALUE, so it is live, not
   * static -- it sits outside the cap, on top of the cached skirt. */
  if (k->compact && norm > 0.0001) {
    cola(cr, ACC_R, ACC_G, ACC_B, 0.7*a); cairo_set_line_width(cr, 1.4);
    cairo_new_sub_path(cr); cairo_arc(cr, cx, cy, R+2.5, zc_angle(0)-M_PI/2, zc_angle(norm)-M_PI/2);
    cairo_stroke(cr);
  }
  /* indicator line: from cap centre outward, width 3.5, length cap*0.40 (cap = DIAMETER), #26241f */
  double ang = zc_angle(norm), plen = (2.0*capR)*0.40;
  cairo_set_line_width(cr, k->compact ? 2.4 : 3.5); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cola(cr, 0.149, 0.141, 0.122, a);
  cairo_move_to(cr, cx, cy);
  cairo_line_to(cr, cx + sin(ang)*plen, cy - cos(ang)*plen);
  cairo_stroke(cr);

  /* caption / read-out */
  if (k->compact) {
    /* header knob: label (or readout %) to the LEFT of the knob, vertically centered. */
    char buf[24]; const char *s;
    double fsz = 11.0, ls = 1.2;
    if (show_val) { snprintf(buf, sizeof buf, "%d%%", (int)(norm*100 + 0.5)); s = buf; }
    else if (drive_os > 0 && drive_os < SET_NOPT) {   /* 1x is the default -> label stays clean */
      snprintf(buf, sizeof buf, "%s %s", k->label, SET_OS_LBL[drive_os]); s = buf;
    }
    else s = k->label;
    zc_setface(cr, show_val ? ZF_VALUE : ZF_LABEL, 1);
    cairo_set_font_size(cr, fsz);
    double tw = txt_width(cr, s, ls);
    double lx = cx - R - 12.0 - tw;       /* the text ENDS 12 px to the left of the knob */
    double ty = cy + fsz * 0.35;          /* baseline ~ vertical center of the knob */
    if (show_val)   /* accent readout honours dim too, or it stays lit in bypass and CANTA */
      txt_glow(cr, lx, ty, s, fsz, ZF_VALUE, 1, ACC_R*a, ACC_G*a, ACC_B*a, ls, 0);
    else {
      /* The DRIVE label is a control, so it is coloured like one -- same grammar as the title:
       * accent = idle/armed, flat white = active. Here "active" is card_open: while the card is up
       * the label goes white, which is what tells you WHICH label opened it.
       * Dimmed to 50% when the drive is at 0 (off), as before. */
      double br = (norm < 0.0001) ? 0.5 : 1.0;
      double r, g, b;
      if (card_open) { r = 0.949; g = 0.937; b = 0.914; }        /* #f2efe9 flat white: pressed */
      else           { r = ACC_R; g = ACC_G; b = ACC_B; }        /* #d98a3a copper: idle */
      txt(cr, lx, ty, s, fsz, ZF_LABEL, 1, (r*a+0.05)*br, (g*a+0.05)*br, (b*a+0.05)*br, ls, 0);
    }
  } else {
    double capy = cy + KNOB_R + 22;
    if (show_val) {   /* live read-out: Barlow 600 15px accent + 0 0 8px accent66; honours dim */
      char buf[24]; fmt_value(k, norm, buf, sizeof buf);
      txt_glow(cr, cx, capy, buf, 15, ZF_VALUE, 1, ACC_R*a, ACC_G*a, ACC_B*a, 0.4, 1);
    } else {          /* caption: Oswald 600 12px #b7b1a0 */
      txt(cr, cx, capy, k->label, 12, ZF_LABEL, 1,
          0.718*a+0.1, 0.694*a+0.1, 0.627*a+0.1, 1.2, 1);
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
  /* label: accent + text-shadow 0 0 9px #d98a3a55 (family section-header pattern) */
  txt_glow(cr, cx - (icon?10:0), y+4, label, 11, ZF_HEAD, 1, ACC_R*a+0.05, ACC_G*a+0.05, ACC_B*a+0.05, 3.0, 1);
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
  /* switch-bank style shared with the family (see the sibling handoffs): ON = dark face
   * #2b2825->#161412 + INSET shadow (sunk); OFF = raised #3c3c39->#232321 + top highlight.
   * Lit text carries text-shadow 0 0 6px. */
  /* delicate accent halo on lit cells: many faint layers, graded falloff (~1.9px) -> blur, not ring */
  if (lit) { for (int g = 8; g >= 1; g--) { double e = g*0.24, al = 0.05*(1.0-(g-1)/8.0);
    rrect(cr, x-e, y-e, w+2*e, h+2*e, 4+e); cola(cr, ACC_R,ACC_G,ACC_B, al); cairo_fill(cr); } }
  if (!lit) { rrect(cr, x, y+1, w, h, 4); cola(cr, 0,0,0,0.45); cairo_fill(cr); }  /* grounds the bevel */
  rrect(cr, x, y, w, h, 4);
  cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y+h);
  if (lit) { cairo_pattern_add_color_stop_rgb(p, 0, 0.169,0.157,0.145);   /* #2b2825 */
             cairo_pattern_add_color_stop_rgb(p, 1, 0.086,0.078,0.071); } /* #161412 */
  else     { cairo_pattern_add_color_stop_rgb(p, 0, 0.235,0.235,0.224);   /* #3c3c39 */
             cairo_pattern_add_color_stop_rgb(p, 1, 0.137,0.137,0.129); } /* #232321 */
  cairo_set_source(cr, p); cairo_fill(cr); cairo_pattern_destroy(p);
  if (lit) {   /* inset shadow -> pressed-in depth (light from above = deepest at the top edge) */
    cairo_save(cr); rrect(cr, x, y, w, h, 4); cairo_clip(cr);
    for (int sN = 1; sN <= 4; sN++) { rrect(cr, x, y, w, h, 4); cola(cr, 0,0,0,0.13);
      cairo_set_line_width(cr, sN*1.5); cairo_stroke(cr); }
    cairo_pattern_t *ts = cairo_pattern_create_linear(0, y, 0, y+h*0.6);
    cairo_pattern_add_color_stop_rgba(ts, 0, 0,0,0, 0.30);
    cairo_pattern_add_color_stop_rgba(ts, 1, 0,0,0, 0.0);
    rrect(cr, x, y, w, h, 4); cairo_set_source(cr, ts); cairo_fill(cr); cairo_pattern_destroy(ts);
    cairo_restore(cr);
  } else {     /* raised: lit top rim + shadowed bottom rim */
    cola(cr, 1,0.95,0.89, 0.16); cairo_set_line_width(cr, 1);
    cairo_move_to(cr, x+3, y+1.2); cairo_line_to(cr, x+w-3, y+1.2); cairo_stroke(cr);
    cola(cr, 0,0,0,0.5); cairo_move_to(cr, x+3, y+h-1.4); cairo_line_to(cr, x+w-3, y+h-1.4); cairo_stroke(cr);
  }
  rrect(cr, x, y, w, h, 4); cola(cr, 0,0,0,0.6); cairo_set_line_width(cr,1); cairo_stroke(cr);
  const Switch *s = &SWITCHES[i];
  double r,g,b;
  if (lit) { if (s->red){r=1.0;g=0.29;b=0.227;} else {r=ACC_R;g=ACC_G;b=ACC_B;} }
  else { r=0.812;g=0.788;b=0.722; }
  if (lit) txt_glow(cr, x+w/2, y+h/2+3.5, s->label, 9.5, ZF_LABEL, 1, r,g,b, 1.2, 1);
  else     txt(cr, x+w/2, y+h/2+3.5, s->label, 9.5, ZF_LABEL, 1, r,g,b, 1.2, 1);
}

/* ---------------- the OVERSAMPLING card (popup window) ---------------------------------------
 * Opened by clicking the DRIVE label; closed by the X, or by clicking anywhere outside the card.
 * This is the ORIGINAL settings card (it shipped that way, opened from the title) brought back and
 * re-anchored to the DRIVE label -- same protocol, current 0C family tokens. It carries THREE
 * segments now: the old fourth, AUTO, went away with the port (drive_os is 1x/2x/4x = 0/1/2).
 * Geometry SHARED by the draw code and ui.c's hit-test so they cannot desync. */
#define SET_CARD_W 210.0
#define SET_CARD_H 108.0
#define SET_CARD_X ((PANEL_W - SET_CARD_W) / 2.0)
#define SET_CARD_Y ((PANEL_H - SET_CARD_H) / 2.0)
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
/* dim the panel, keep the brand lit (it would vanish under the dim), then the card on top.
 * hover_seg / hover_close = -1 / 0 when nothing is hovered. */
static void zc_draw_settings(cairo_t *cr, const float *v, int hover_seg, int hover_close) {
  cola(cr, 0, 0, 0, 0.62); cairo_rectangle(cr, 0, 0, PANEL_W, PANEL_H); cairo_fill(cr);
  txt_dot0(cr, 14, 32, "DYNAMO 0C", 18, ZF_BRAND, 1, ACC_R, ACC_G, ACC_B, 0.6, 0);
  txt(cr, 15, 44, "PARAMETRIC EQ", 7.5, ZF_LABEL, 1, ACC_R*0.7, ACC_G*0.7, ACC_B*0.7, 2.0, 0);
  /* Redraw the DRIVE label ON TOP of the dim, flat white: it is the control that opened this card,
   * so it must read as pressed. Under the dim it washes out to grey and the link back to what you
   * clicked is lost. Same reason the brand is redrawn above. Right-aligned exactly as the live pass
   * draws it (ends at DRV_LBL_R) -- if that changes, change it here too. */
  { char buf[24]; const char *s = "DRIVE";
    int os = (int)(v[P_DRIVE_OS] + 0.5f);
    if (os > 0 && os < SET_NOPT) { snprintf(buf, sizeof buf, "DRIVE %s", SET_OS_LBL[os]); s = buf; }
    zc_setface(cr, ZF_LABEL, 1); cairo_set_font_size(cr, 11.0);
    double tw = txt_width(cr, s, 1.2);
    txt(cr, DRV_LBL_R - tw, DRV_CY + 11.0*0.35, s, 11.0, ZF_LABEL, 1, 0.949, 0.937, 0.914, 1.2, 0); }

  /* card: family tokens (#1c1a18 -> #100f0d, like the rest of the recessed surfaces) */
  rrect(cr, SET_CARD_X, SET_CARD_Y, SET_CARD_W, SET_CARD_H, 10);
  cairo_pattern_t *p = cairo_pattern_create_linear(0, SET_CARD_Y, 0, SET_CARD_Y+SET_CARD_H);
  cairo_pattern_add_color_stop_rgb(p, 0, 0.110, 0.102, 0.094);
  cairo_pattern_add_color_stop_rgb(p, 1, 0.063, 0.059, 0.051);
  cairo_set_source(cr, p); cairo_fill_preserve(cr); cairo_pattern_destroy(p);
  cola(cr, ACC_R, ACC_G, ACC_B, 0.55); cairo_set_line_width(cr, 1.2); cairo_stroke(cr);

  txt(cr, SET_CARD_X+16, SET_CARD_Y+27, "OVERSAMPLING", 11, ZF_HEAD, 1, ACC_R, ACC_G, ACC_B, 2.0, 0);
  /* honest subtitle: 1x is the shipping default AND the bit-identical one; 2x/4x add latency */
  txt(cr, SET_CARD_X+16, SET_CARD_Y+43, "DRIVE quality \xC2\xB7 1x = no latency", 7.0, ZF_LABEL, 1,
      0.60, 0.58, 0.52, 0.4, 0);

  int cur = (int)(v[P_DRIVE_OS] + 0.5f); if (cur < 0) cur = 0; if (cur > SET_NOPT-1) cur = SET_NOPT-1;
  for (int i = 0; i < SET_NOPT; i++) {
    double x, y, w, h; set_seg_rect(i, &x, &y, &w, &h);
    rrect(cr, x, y, w, h, 4);
    if (i == cur) {
      cairo_pattern_t *ap = cairo_pattern_create_linear(0, y, 0, y+h);
      cairo_pattern_add_color_stop_rgb(ap, 0, ACC_R, ACC_G, ACC_B);
      cairo_pattern_add_color_stop_rgb(ap, 1, ACC_R*0.8, ACC_G*0.8, ACC_B*0.8);
      cairo_set_source(cr, ap); cairo_fill_preserve(cr); cairo_pattern_destroy(ap);
    }
    else if (i == hover_seg) { cola(cr, 1, 1, 1, 0.11); cairo_fill_preserve(cr); }
    else                     { cola(cr, 1, 1, 1, 0.05); cairo_fill_preserve(cr); }
    cola(cr, 0, 0, 0, 0.5); cairo_set_line_width(cr, 1); cairo_stroke(cr);
    int on = (i == cur);
    txt(cr, x+w/2, y+h/2+3.5, SET_OS_LBL[i], 9.5, ZF_LABEL, 1,
        on?0.102:0.812, on?0.078:0.788, on?0.063:0.722, 1.2, 1);
  }

  /* close X (top-right of the card) */
  double a = hover_close ? 1.0 : 0.72;
  cola(cr, ACC_R, ACC_G, ACC_B, a);
  cairo_set_line_width(cr, 1.6); cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  double r = 5.0;
  cairo_move_to(cr, SET_CLOSE_CX-r, SET_CLOSE_CY-r); cairo_line_to(cr, SET_CLOSE_CX+r, SET_CLOSE_CY+r);
  cairo_move_to(cr, SET_CLOSE_CX+r, SET_CLOSE_CY-r); cairo_line_to(cr, SET_CLOSE_CX-r, SET_CLOSE_CY+r);
  cairo_stroke(cr);
}

/* ---------------- bypass: the TITLE is the switch (no POWER button) ---------------------------
 * Family pattern (from DynaSolidComp): clicking "DYNAMO 0C" toggles engage/bypass. Engaged =
 * accent + glow, bypassed = flat white and the whole panel dims.
 * The hit box is SHARED by the draw code and ui.c's hit-test so they can never desync.
 * ⚠ WIDTH: this box and drive_label_rect() are NEIGHBOURS in the header, and on_press tests the
 * title FIRST -- so any overlap silently steals the label's clicks and toggles bypass instead of
 * opening the card. Measured: the brand text "DYNAMO 0C" is 92.8 px at 18px/ZF_BRAND/ls=0.6 drawn
 * from x=14, so it ENDS at ~107. The old w=138 (x1=150) was ~43 px of dead padding that was never
 * title, and it left the label a 0.2 px clearance -- luck, not headroom. w=100 (x1=112) keeps every
 * pixel of the text plus ~5 px of pad, and hands the slack to the label, which needs it for the
 * "DRIVE 4x" form. Do not widen it past drive_label_rect()'s left edge (130). */
static inline void title_rect(double *x, double *y, double *w, double *h) {
  *x = 12.0; *y = 15.0; *w = 100.0; *h = 23.0;
}
/* ---------------- full panel ----------------
 * v   = control values (indexed by port)
 * showk = index of the knob with active read-out (-1 = none) */
/* ---------------- chassis (STATIC) ----------------
 * Depends on NOTHING: not on a port value, not on dim. That is what lets the UI render it ONCE
 * into a cached surface and blit it every frame (see ui.c). It is also the expensive half --
 * ~120 hairline strokes plus gradients -- so caching it is most of the win. Chassis-local
 * coords (0,0 .. PANEL_W,PANEL_H), already clipped by the caller. */
static void zc_draw_chassis(cairo_t *cr){
  /* chassis: dark brushed metal -- family tokens (already the right colours):
   * linear-gradient(160deg, #1c1b1a 0%, #121110 55%, #0a0a09 100%). Missing were the brushed
   * overlay and the two inner edges. */
  cairo_pattern_t *bg = cairo_pattern_create_linear(PANEL_W*0.30, 0, PANEL_W*0.62, PANEL_H);
  cairo_pattern_add_color_stop_rgb(bg, 0.0, 0.110,0.106,0.102);  /* #1c1b1a */
  cairo_pattern_add_color_stop_rgb(bg, 0.55,0.071,0.067,0.063);  /* #121110 */
  cairo_pattern_add_color_stop_rgb(bg, 1.0, 0.039,0.039,0.035);  /* #0a0a09 */
  cairo_set_source(cr, bg); cairo_paint(cr); cairo_pattern_destroy(bg);
  /* faint vertical brushed overlay: 1px repeating lines. ~0.8%, NOT a straight 2% alpha -- the
   * spec's 2% is mix-blend-mode:overlay; at straight 2% it reads as stripes (same finding as
   * DynaComp 0C, checked there against the reference render). */
  cairo_set_line_width(cr, 1.0);
  for (double x = 1.5; x < PANEL_W-1; x += 2) {
    cola(cr, 1,1,1, 0.008); cairo_move_to(cr, x, 4); cairo_line_to(cr, x, PANEL_H-4); cairo_stroke(cr);
  }
  /* 1px inner dark + 1px inner light edge */
  rrect(cr, 0.5, 0.5, PANEL_W-1, PANEL_H-1, 11);
  cola(cr, 0,0,0,0.6); cairo_set_line_width(cr,1); cairo_stroke(cr);
  rrect(cr, 1.5, 1.5, PANEL_W-3, PANEL_H-3, 10);
  cola(cr, 1,1,1,0.05); cairo_set_line_width(cr,1); cairo_stroke(cr);
}

/* anim[0..1] = ANIMATED engaged-ness (0..1) of POWER / EQ, driven by ui_idle at 0.2s. The dims are
 * products of those, so fading either parent fades the block. Discrete states (title accent vs
 * white) flip at the midpoint. Pass NULL for a static render. */
/* ---------------- STATIC pass (cached) ----------------
 * Everything that does NOT follow a live port value -- only dim, the shelf/bell icon states and
 * the OS selection. The UI renders it ONCE into a surface and blits it. With ELEVEN knobs this
 * pass is the overwhelming majority of the panel (~77 tick labels alone). */
static void zc_draw_static(cairo_t *cr, const float *v, ZcHit *hits, int *nhits, const double *anim) {
  double a_pw = anim ? anim[0] : (v[P_POWER] > 0.5f ? 1.0 : 0.0);
  double a_eq = anim ? anim[1] : (v[P_EQ_ON] > 0.5f ? 1.0 : 0.0);
  int power = a_pw > 0.5, eq_on = a_eq > 0.5;
  double dim_all = 1.0 - a_pw;
  double dim_eq  = 1.0 - a_pw*a_eq;
  *nhits = 0;
  /* header: plugin name "DYNAMO 0C" as brand (left) with DOTTED ZERO; the DRIVE knob
   * goes alone on the right (drawn in the knob loop). There is NO logo anymore. */
  /* the TITLE is the bypass switch: accent + glow when engaged, flat white when bypassed */
  if (power) txt_dot0_glow(cr, 14, 32, "DYNAMO 0C", 18, ZF_BRAND, 1, ACC_R,ACC_G,ACC_B, 0.6, 0);
  else       txt_dot0(cr, 14, 32, "DYNAMO 0C", 18, ZF_BRAND, 1, 0.910,0.886,0.824, 0.6, 0);
  txt(cr, 15, 44, "PARAMETRIC EQ", 7.5, ZF_LABEL, 1, 0.561,0.537,0.478, 2.0, 0);

  /* band sections (HIGH/LOW toggle bell<->shelf -> clickable icon; mids always bell) */
  *nhits = 0;
  draw_section(cr, BAND_Y(0)+10, "HIGH",   1, v[P_HF_SHELF]>0.5f, 1, dim_eq, P_HF_SHELF, hits, nhits);
  draw_section(cr, BAND_Y(1)+10, "HI\xC2\xB7MID", 1, 0, 1, dim_eq, -1, hits, nhits);   /* mid: fixed bell, dimmed */
  draw_section(cr, BAND_Y(2)+10, "LO\xC2\xB7MID", 1, 0, 0, dim_eq, -1, hits, nhits);
  draw_section(cr, BAND_Y(3)+10, "LOW",    1, v[P_LF_SHELF]>0.5f, 0, dim_eq, P_LF_SHELF, hits, nhits);
  draw_section(cr, BAND_Y(4)+10, "FILTERS",0, 0, 0, dim_all, -1, hits, nhits);
  for (int i = 0; i < ZC_NKNOB; i++) {
    double dim = (i < 8) ? dim_eq : dim_all;
    draw_knob_static(cr, &KNOBS[i], dim);
  }
  for (int i = 0; i < ZC_NSW; i++) {
    int lit = (i == SW_EQ) ? eq_on : (v[SWITCHES[i].port] > 0.5f);
    draw_switch(cr, i, lit);
  }
  (void)power;
}

/* ---------------- LIVE pass ----------------
 * ONLY what a port value moves: each knob's pointer, its caption/read-out and the DRIVE ring. */
static void zc_draw_live(cairo_t *cr, const float *v, int showk, const double *anim, int card_open) {
  double a_pw = anim ? anim[0] : (v[P_POWER] > 0.5f ? 1.0 : 0.0);
  double a_eq = anim ? anim[1] : (v[P_EQ_ON] > 0.5f ? 1.0 : 0.0);
  double dim_all = 1.0 - a_pw;
  double dim_eq  = 1.0 - a_pw*a_eq;
  for (int i = 0; i < ZC_NKNOB; i++) {
    double dim = (i < 8) ? dim_eq : dim_all;
    draw_knob_live(cr, &KNOBS[i], v[KNOBS[i].port], showk == i, dim,
                   KNOBS[i].compact ? (int)(v[P_DRIVE_OS] + 0.5f) : 0,
                   KNOBS[i].compact ? card_open : 0);
  }
}

#endif /* ZC_UI_PANEL_H */
