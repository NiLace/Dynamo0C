/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* ui_photo.h -- the photoreal panel: drawing only, pure cairo.
 *
 * The panel is a render of the approved 500-series module (a 3D scene of the module, kept with the sources' history)
 * with every button released and NO knob pointers. Everything that follows a port is drawn here, on top, in the
 * render's own pixels (PH_W x PH_H; positions from ui_photo_layout.h, which the scene wrote by projecting through its
 * camera -- nothing measured off the image):
 *   - each knob's pointer, at the port's angle on the knob's white head;
 *   - a pressed button: its crop from buttons.png, lit while the plugin runs, dark while bypassed;
 *   - LF / HF: the curve symbol of the selected mode lit amber (pressed = shelf, released = bell);
 *   - EQ out: the four bands' caps dark (their knobs do not turn); bypassed: the whole metal dimmed but the title,
 *     which is the bypass switch;
 *   - a knob being moved: its value.
 * ui_panel.h still provides the controls' tables (KNOBS, SWITCHES, scales), the read-out format and the text helpers. */
#ifndef ZC_UI_PHOTO_H
#define ZC_UI_PHOTO_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <cairo/cairo.h>

#include "ports.h"
#include "ui_photo_layout.h"

#include "ui_panel.h"

#define PH_NLABEL ((int)(sizeof PH_LABEL / sizeof PH_LABEL[0]))
#define PH_DEFAULT_SCALE 0.55   /* the window's default size, as a fraction of the render (~223 x 747 px) */
#define PH_MIN_SCALE     0.42

typedef struct {
  cairo_surface_t *panel;     /* PH_W x PH_H */
  cairo_surface_t *buttons;   /* 8 cells of PH_BTN_W x PH_BTN_H: 4 pressed+lit, 4 pressed+dark */
  cairo_surface_t *title;     /* A8 mask of the title's letters (their amber ink, read off the panel), title box sized */
  cairo_surface_t *label[PH_NLABEL];  /* the same for PH_LABEL[] (band names, DRIVE) */
} PhImages;

/* The title's letters as a mask: where the panel's pixel is amber (red well above blue) inside the title box. Built
 * once at load, so a bypassed panel can dim EVERYTHING evenly and repaint only the letters, without a box round them. */
static cairo_surface_t *ph_ink_mask(cairo_surface_t *panel, double bx0, double by0, double bx1, double by1) {
  int x0 = (int)bx0, y0 = (int)by0, w = (int)(bx1 - bx0) + 1, h = (int)(by1 - by0) + 1;
  cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
  cairo_surface_flush(panel); cairo_surface_flush(m);
  const unsigned char *src = cairo_image_surface_get_data(panel); int ss = cairo_image_surface_get_stride(panel);
  unsigned char *dst = cairo_image_surface_get_data(m); int ds = cairo_image_surface_get_stride(m);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int X = x0 + x, Y = y0 + y; double a = 0.0;
      if (X >= 0 && X < PH_W && Y >= 0 && Y < PH_H) {
        uint32_t px = *(const uint32_t *)(src + Y * ss + 4 * X);      /* cairo RGB24: 0x00RRGGBB */
        int r = (px >> 16) & 255, b = px & 255;
        a = (r - b - 25) / 70.0; if (a < 0) a = 0; if (a > 1) a = 1;
      }
      dst[y * ds + x] = (unsigned char)(a * 255.0 + 0.5);
    }
  cairo_surface_mark_dirty(m);
  return m;
}

/* Loads <dir>/panel.png and <dir>/buttons.png. Returns 1 if both loaded with the expected size, else 0 (the panel
 * then draws a plain dark plate with the live parts, so a broken install still shows working controls). */
static int ph_load(PhImages *im, const char *dir) {
  char p[4200];
  im->panel = im->buttons = im->title = NULL;
  for (int i = 0; i < PH_NLABEL; i++) im->label[i] = NULL;
  snprintf(p, sizeof p, "%s/panel.png", dir);
  cairo_surface_t *a = cairo_image_surface_create_from_png(p);
  snprintf(p, sizeof p, "%s/buttons.png", dir);
  cairo_surface_t *b = cairo_image_surface_create_from_png(p);
  int ok = cairo_surface_status(a) == CAIRO_STATUS_SUCCESS && cairo_surface_status(b) == CAIRO_STATUS_SUCCESS
        && cairo_image_surface_get_width(a) == PH_W && cairo_image_surface_get_height(a) == PH_H
        && cairo_image_surface_get_width(b) == 8 * PH_BTN_W && cairo_image_surface_get_height(b) == PH_BTN_H;
  if (!ok) {
    fprintf(stderr, "Dynamo 0C: panel images missing or of the wrong size in %s (plain panel)\n", dir);
    cairo_surface_destroy(a); cairo_surface_destroy(b);
    return 0;
  }
  im->panel = a; im->buttons = b; im->title = ph_ink_mask(a, PH_TITLE[0], PH_TITLE[1], PH_TITLE[2], PH_TITLE[3]);
  for (int i = 0; i < PH_NLABEL; i++)
    im->label[i] = ph_ink_mask(a, PH_LABEL[i].x0, PH_LABEL[i].y0, PH_LABEL[i].x1, PH_LABEL[i].y1);
  return 1;
}
static void ph_free(PhImages *im) {
  if (im->panel) cairo_surface_destroy(im->panel);
  if (im->buttons) cairo_surface_destroy(im->buttons);
  if (im->title) cairo_surface_destroy(im->title);
  for (int i = 0; i < PH_NLABEL; i++) { if (im->label[i]) cairo_surface_destroy(im->label[i]); im->label[i] = NULL; }
  im->panel = im->buttons = im->title = NULL;
}

/* ---------------- hit tests, in the render's pixels ---------------- */
static inline int ph_in(const double b[4], double x, double y) { return x >= b[0] && x <= b[2] && y >= b[1] && y <= b[3]; }
static inline int ph_knob_at(double x, double y) {
  for (int i = 0; i < ZC_NKNOB; i++) {
    double dx = x - PH_KNOB[i].cx, dy = y - PH_KNOB[i].cy, R = PH_KNOB[i].body_r * 1.35;
    if (dx * dx + dy * dy <= R * R) return i;
  }
  return -1;
}
static inline int ph_switch_at(double x, double y) {
  for (int i = 0; i < ZC_NSW; i++) {
    double b[4] = { PH_BTN[i].x0, PH_BTN[i].y0, PH_BTN[i].x1, PH_BTN[i].y1 };
    if (ph_in(b, x, y)) return i;
  }
  return -1;
}
/* a curve symbol: returns its index in PH_ICON (its switch and mode are there), or -1 */
static inline int ph_icon_at(double x, double y) {
  for (int i = 0; i < (int)(sizeof PH_ICON / sizeof PH_ICON[0]); i++) {
    double b[4] = { PH_ICON[i].x0, PH_ICON[i].y0, PH_ICON[i].x1, PH_ICON[i].y1 };
    if (ph_in(b, x, y)) return i;
  }
  return -1;
}

/* ---------------- the live parts ---------------- */
/* a curve symbol's centre line, the same shape the scene printed (module500_scene.py curve_icon) */
static void ph_icon_path(cairo_t *cr, int kind, double cx, double cy, double w, double h) {
  const int n = 24;
  for (int i = 0; i <= n; i++) {
    double u = (double)i / n, X = cx - w / 2 + w * u, Z;
    if (kind == 0) Z = h * exp(-pow((u - 0.5) / 0.17, 2.0));
    else { double sg = 1.0 / (1.0 + exp(-(u - 0.5) * 12.0)); Z = h * (kind == 1 ? 1.0 - sg : sg); }
    double Y = cy + h / 2 - Z;                /* the image's y runs down */
    if (i == 0) cairo_move_to(cr, X, Y); else cairo_line_to(cr, X, Y);
  }
}
static void ph_pointer(cairo_t *cr, int i, double norm, double alpha) {
  double a = zc_angle(norm), dx = sin(a), dy = -cos(a), r = PH_KNOB[i].cap_r;
  double cx = PH_KNOB[i].cx, cy = PH_KNOB[i].cy;
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
  cairo_set_line_width(cr, 0.80 * PH_PX_MM);
  cairo_set_source_rgba(cr, 0.043, 0.043, 0.043, 0.94 * alpha);
  cairo_move_to(cr, cx + dx * r * 0.10, cy + dy * r * 0.10);
  cairo_line_to(cr, cx + dx * r * 0.92, cy + dy * r * 0.92);
  cairo_stroke(cr);
}
/* a knob that does not respond (its band out with the EQ): its white cap goes dark, pointer included, and nothing
 * round it changes. The cap's edge is soft over a few pixels so it does not read as a drawn circle. */
static void ph_cap_off(cairo_t *cr, int i, double a) {
  double cx = PH_KNOB[i].cx, cy = PH_KNOB[i].cy, r = PH_KNOB[i].cap_r;
  cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r * 1.06);
  cairo_pattern_add_color_stop_rgba(g, 0.0, 0.02, 0.02, 0.02, a);
  cairo_pattern_add_color_stop_rgba(g, 0.92, 0.02, 0.02, 0.02, a);
  cairo_pattern_add_color_stop_rgba(g, 1.0, 0.02, 0.02, 0.02, 0);
  cairo_set_source(cr, g); cairo_arc(cr, cx, cy, r * 1.06, 0, 2 * M_PI); cairo_fill(cr);
  cairo_pattern_destroy(g);
}

/* The whole panel, in the render's pixels. anim[0] = engaged-ness of POWER, anim[1] of EQ (0..1, faded by the UI).
 * showk = knob whose value is shown (-1 none). */
static void ph_draw(cairo_t *cr, const PhImages *im, const float *v, int showk, const double *anim) {
  const double run = anim[0], eq = anim[1];
  /* 1) the plate */
  if (im->panel) { cairo_set_source_surface(cr, im->panel, 0, 0); cairo_paint(cr); }   /* NULL: the caller drew it */
  /* 2) pressed buttons: lit while the plugin runs, dark while bypassed (cross-faded with POWER) */
  for (int i = 0; i < ZC_NSW; i++) {
    const int pressed = v[SWITCHES[i].port] > 0.5f || (SWITCHES[i].port2 >= 0 && v[SWITCHES[i].port2] > 0.5f);
    if (!pressed || !im->buttons) continue;   /* BUMP lights if either filter is resonant (a host can set them apart) */
    double x = PH_BTN[i].cx0, y = PH_BTN[i].cy0;
    for (int lit = 0; lit < 2; lit++) {
      double a = lit ? run : 1.0;
      if (a <= 0.0) continue;
      cairo_save(cr);
      cairo_rectangle(cr, x, y, PH_BTN_W, PH_BTN_H); cairo_clip(cr);
      cairo_set_source_surface(cr, im->buttons, x - (lit ? i : 4 + i) * PH_BTN_W, y);
      cairo_paint_with_alpha(cr, a);
      cairo_restore(cr);
    }
  }
  /* 3b) a section's name is AMBER while it is in use and WHITE otherwise: a band once EQ is in and its gain is off
   *     0 dB, DRIVE while it is on. The printed ink is amber; a white one is repainted through the mask of its ink. */
  for (int i = 0; i < PH_NLABEL; i++) {
    if (!im->label[i]) continue;
    float g = v[PH_LABEL[i].port];
    int on = PH_LABEL[i].kind == 0 ? (v[P_EQ_ON] > 0.5f && fabsf(g - 0.5f) > 1e-4f) : (g > 1e-4f);
    if (on) continue;
    cairo_set_source_rgb(cr, 0.925, 0.918, 0.898);
    cairo_mask_surface(cr, im->label[i], (int)PH_LABEL[i].x0, (int)PH_LABEL[i].y0);
  }
  /* 4) pointers; EQ out: the four bands' caps (the first 8 knobs) go dark over theirs */
  for (int i = 0; i < ZC_NKNOB; i++) ph_pointer(cr, i, v[KNOBS[i].port], 1.0);
  if (eq < 1.0) for (int i = 0; i < 8; i++) ph_cap_off(cr, i, 0.72 * (1.0 - eq));
  /* 5) LF / HF: the selected mode's symbol lit amber (a soft halo, then the line) */
  for (int i = 0; i < (int)(sizeof PH_ICON / sizeof PH_ICON[0]); i++) {
    int shelf = v[SWITCHES[PH_ICON[i].sw].port] > 0.5f;
    if (shelf != PH_ICON[i].shelf || run <= 0.0) continue;
    const double cx = PH_ICON_SHAPE[i].cx, cy = PH_ICON_SHAPE[i].cy, w = PH_ICON_SHAPE[i].w, h = PH_ICON_SHAPE[i].h;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    ph_icon_path(cr, PH_ICON_SHAPE[i].kind, cx, cy, w, h);
    cairo_set_line_width(cr, PH_ICON_SHAPE[i].lw * 3.2); cola(cr, 1.0, 0.60, 0.23, 0.22 * run); cairo_stroke(cr);
    ph_icon_path(cr, PH_ICON_SHAPE[i].kind, cx, cy, w, h);
    cairo_set_line_width(cr, PH_ICON_SHAPE[i].lw * 1.1); cola(cr, ACC_R, ACC_G, ACC_B, run); cairo_stroke(cr);
  }
  /* 6) bypassed: the whole metal dims evenly; the title is the switch, so its letters are repainted on top, from
   *    amber to a pale grey (the family's bypassed brand), through the mask of their own ink */
  if (run < 1.0) {
    cairo_save(cr);
    cairo_rectangle(cr, PH_METAL[0], PH_METAL[1], PH_METAL[2] - PH_METAL[0], PH_METAL[3] - PH_METAL[1]); cairo_clip(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * (1.0 - run)); cairo_paint(cr);
    cairo_restore(cr);
    if (im->title) {
      cairo_set_source_rgba(cr, 0.80, 0.79, 0.76, 1.0 - run);
      cairo_mask_surface(cr, im->title, (int)PH_TITLE[0], (int)PH_TITLE[1]);
    }
  }
  /* 7) the value of the knob being moved: a dark tag under its head */
  if (showk >= 0 && showk < ZC_NKNOB) {
    const Knob *k = &KNOBS[showk]; char b[32];
    fmt_value(k, v, b, sizeof b);
    zc_setface(cr, ZF_VALUE, 1); cairo_set_font_size(cr, 2.6 * PH_PX_MM);
    cairo_text_extents_t e; cairo_text_extents(cr, b, &e);
    double pw = e.width + 2.4 * PH_PX_MM, ph = 4.0 * PH_PX_MM;
    double x = PH_KNOB[showk].cx - pw / 2, y = PH_KNOB[showk].cy + PH_KNOB[showk].body_r + 0.8 * PH_PX_MM;
    if (x < PH_METAL[0] + 2) x = PH_METAL[0] + 2;
    if (x + pw > PH_METAL[2] - 2) x = PH_METAL[2] - 2 - pw;
    rrect(cr, x, y, pw, ph, ph / 2); cola(cr, 0.05, 0.05, 0.05, 0.88); cairo_fill_preserve(cr);
    cola(cr, ACC_R, ACC_G, ACC_B, 0.7); cairo_set_line_width(cr, 1.0); cairo_stroke(cr);
    cola(cr, 0.95, 0.94, 0.91, 1.0);
    cairo_move_to(cr, x + (pw - e.width) / 2 - e.x_bearing, y + ph / 2 - e.y_bearing - e.height / 2);
    cairo_show_text(cr, b);
  }
}

#endif /* ZC_UI_PHOTO_H */
