/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* ui.c -- Dynamo 0C EQ : LV2 UI (Pugl + Cairo). Single translation unit.
 * Drawing in ui_photo.h (pure module: the photoreal panel and its live parts; the controls' tables come from
 * ui_panel.h, their defaults from port_defaults.h, i.e. the TTL). Here: Pugl window, events, control-port writes. Every
 * coordinate below is in the panel render's pixels (PH_W x PH_H): the window is that image, scaled and centred. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pugl/pugl.h>
#include <pugl/cairo.h>
#include <cairo/cairo.h>
#include <X11/Xlib.h>

#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>
#include <lv2/options/options.h>
#include <lv2/atom/atom.h>

#include "uris.h"
#include "ports.h"
#include "ui_fonts.h"
#include "ui_photo.h"
#include "port_defaults.h"

#define DRAG_RANGE 200.0   /* WINDOW px of drag = full travel (converted to panel px with the scale) */
#define DCLICK_S   0.35    /* double-click window (s) */
#define ZC_FINE    6.0     /* hold SHIFT while dragging -> this many times finer */

typedef struct {
  LV2UI_Write_Function write;
  LV2UI_Controller     controller;
  LV2UI_Resize        *resize;
  LV2UI_Touch         *touch;      /* ui:touch, optional: NULL if the host does not offer it */
  PuglWorld           *world;
  PuglView            *view;

  float  v[ZC_N_PORTS];

  int    drag_knob;        /* -1 or index */
  double drag_yprev, drag_val;   /* incremental drag: last pointer y + accumulated 0..1 value */
  int    show_knob; double show_time;
  int    last_knob; double last_press;
  int    needs_redraw;

  int    set_open;         /* the OVERSAMPLING card is open (opened from the DRIVE label) */
  int    set_hover_seg;    /* segment under the pointer while it is open (-1 = none) */
  int    set_hover_close;  /* pointer is over the close X */

  PhImages img;            /* panel.png + buttons.png + the ink masks, per instance: ~2.2 MB decoded */

  double anim[2];          /* animated engaged-ness of POWER / EQ (0..1), 0.2s fades */
  double last_t;           /* last idle timestamp -> framerate-independent animation */
  double last_paint;       /* timestamp of the last real repaint (frame-rate cap, see ui_idle) */
  cairo_surface_t *bg;     /* the panel image resampled to the window's size (rebuilt when the size changes): the
                            * per-frame cost is then one blit, not a 400x1360 resample */
  int    bg_w, bg_h;       /* window size the cache was built for */

  int    win_w, win_h;     /* current window size */
  double scale, ox, oy;    /* logical->window transform (uniform scale + centering) */
  int    fonts_loaded;     /* nonzero if this instance holds a font reference (balances the free) */
} ZcUI;

/* uniform scale + centring of the panel image in the window */
#define CANVAS_W (PH_W * PH_DEFAULT_SCALE)
#define CANVAS_H (PH_H * PH_DEFAULT_SCALE)
static void recompute_scale(ZcUI *ui) {
  double sx = (double)ui->win_w / PH_W, sy = (double)ui->win_h / PH_H;
  ui->scale = sx < sy ? sx : sy;
  ui->ox = (ui->win_w - PH_W * ui->scale) / 2.0;
  ui->oy = (ui->win_h - PH_H * ui->scale) / 2.0;
}

/* ---------- port writes ----------
 * ui:touch tells the host that the USER has taken hold of a control and then let go of it. Without
 * it the host cannot tell an automation write apart from a user gesture, so automation in touch
 * mode and MIDI learn misbehave. The VST3 equivalent is beginEdit / performEdit / endEdit, in that
 * order.
 *
 * The feature is OPTIONAL: `ui->touch` is NULL when the host does not offer it, and every call goes
 * through zc_touch, which checks. A host without it behaves as if these calls were absent.
 * Called from the UI thread only, like everything else here, as the specification requires. */
static void zc_touch(ZcUI *ui, int port, bool grabbed) {
  if (ui->touch && ui->touch->touch) ui->touch->touch(ui->touch->handle, (uint32_t)port, grabbed);
}
static void write_port(ZcUI *ui, int port, float val) {
  ui->v[port] = val;
  ui->write(ui->controller, (uint32_t)port, sizeof(float), 0, &val);
}
/* One-shot change (a switch, the title, a card segment, a double-click reset): the gesture starts
 * and ends around the single write, which is what the host needs to record it as a user edit. */
static void write_port_gesture(ZcUI *ui, int port, float val) {
  zc_touch(ui, port, true);
  write_port(ui, port, val);
  zc_touch(ui, port, false);
}
static void redraw(ZcUI *ui) { ui->needs_redraw = 1; }

/* ---------- hit-testing: ph_knob_at / ph_switch_at / ph_icon_at (ui_photo.h), in the panel image's pixels.
 *   on_event converts window coords to panel px (subtract the centring, divide by the scale) first. */

/* ---------- events ---------- */
static void on_press(ZcUI *ui, double x, double y) {
  double now = puglGetTime(ui->world);

  /* OVERSAMPLING card open: it is MODAL. The X closes, a segment selects (and closes), and a click
   * anywhere outside the card closes without changing anything. Nothing under the card can be hit
   * -- that is the point of the dim. */
  if (ui->set_open) {
    double cx, cy; ph_to_card(x, y, &cx, &cy);            /* the card keeps its own units (SET_*) */
    double dxc = cx - SET_CLOSE_CX, dyc = cy - SET_CLOSE_CY;
    if (dxc*dxc + dyc*dyc <= SET_CLOSE_R*SET_CLOSE_R) {   /* the X */
      ui->set_open = 0; redraw(ui); ui->last_knob = -1; return;
    }
    for (int i = 0; i < SET_NOPT; i++) {
      double sx, sy, sw, sh; set_seg_rect(i, &sx, &sy, &sw, &sh);
      if (cx >= sx && cx <= sx+sw && cy >= sy && cy <= sy+sh) {
        write_port_gesture(ui, P_DRIVE_OS, (float)i);
        ui->set_open = 0; redraw(ui); ui->last_knob = -1; return;
      }
    }
    if (!set_in_card(cx, cy)) { ui->set_open = 0; redraw(ui); }   /* click outside -> close */
    ui->last_knob = -1; return;
  }
  /* clicking the TITLE toggles engage/bypass -- works even while bypassed (it is the only thing
   * that does). Replaces the old POWER switch. */
  { if (ph_in(PH_TITLE, x, y)) {
      write_port_gesture(ui, P_POWER, ui->v[P_POWER] > 0.5f ? 0.0f : 1.0f);
      redraw(ui); ui->last_knob = -1; return; } }
  /* while bypassed, only the title responds */
  if (ui->v[P_POWER] <= 0.5f) { ui->last_knob = -1; return; }

  /* the DRIVE LABEL is a control: it opens the OVERSAMPLING card. Tested BEFORE the knobs: the boxes are disjoint
   * (checked by make ui-geometry), but the label must win if that ever changes. */
  { if (ph_in(PH_DRIVE_LABEL, x, y)) {
      ui->set_open = 1; ui->set_hover_seg = -1; ui->set_hover_close = 0;
      redraw(ui); ui->last_knob = -1; return; } }

  int k = ph_knob_at(x, y);
  if (k >= 0) {
    if (ui->last_knob == k && (now - ui->last_press) < DCLICK_S) {   /* double-click = reset */
      write_port_gesture(ui, KNOBS[k].port, ZC_TTL_DEFAULT[KNOBS[k].port]);
      ui->show_knob = k; ui->show_time = now; ui->drag_knob = -1; redraw(ui);
    } else {
      ui->drag_knob = k; ui->drag_yprev = y; ui->drag_val = ui->v[KNOBS[k].port];   /* port_event keeps it in [0,1] */
      zc_touch(ui, KNOBS[k].port, true);          /* the gesture starts HERE and ends in on_release */
      ui->show_knob = k; ui->show_time = now; redraw(ui);
    }
    ui->last_knob = k; ui->last_press = now;
    return;
  }
  /* a curve symbol above LF / HF: SELECTS its mode (bell = released, shelf = pressed) */
  { int ic = ph_icon_at(x, y);
    if (ic >= 0) {
      int port = SWITCHES[PH_ICON[ic].sw].port;
      write_port_gesture(ui, port, PH_ICON[ic].shelf ? 1.0f : 0.0f);
      redraw(ui); ui->last_knob = -1; return;
    } }
  int s = ph_switch_at(x, y);
  if (s >= 0) {
    float nv = ui->v[SWITCHES[s].port] > 0.5f ? 0.0f : 1.0f;
    write_port_gesture(ui, SWITCHES[s].port, nv);
    if (SWITCHES[s].port2 >= 0) write_port_gesture(ui, SWITCHES[s].port2, nv);   /* BUMP: HP bump + LP resonant together */
    redraw(ui);
  }
  ui->last_knob = -1;
}
/* hover feedback while the card is open: the X and the segments light under the pointer.
 * Only redraws when the hovered thing actually CHANGES -- a motion event per pixel that repainted
 * the panel would cost a core for nothing (see the family's UI perf notes). */
static void on_hover_settings(ZcUI *ui, double px, double py) {
  int seg = -1; double x, y; ph_to_card(px, py, &x, &y);
  for (int i = 0; i < SET_NOPT; i++) {
    double sx, sy, sw, sh; set_seg_rect(i, &sx, &sy, &sw, &sh);
    if (x >= sx && x <= sx+sw && y >= sy && y <= sy+sh) { seg = i; break; }
  }
  double dxc = x - SET_CLOSE_CX, dyc = y - SET_CLOSE_CY;
  int close = (dxc*dxc + dyc*dyc <= SET_CLOSE_R*SET_CLOSE_R);
  if (seg != ui->set_hover_seg || close != ui->set_hover_close) {
    ui->set_hover_seg = seg; ui->set_hover_close = close; redraw(ui);
  }
}
/* Incremental drag: accumulate the delta each motion (not absolute from the press point), so that
 * pressing/releasing SHIFT mid-drag switches coarse<->fine with no jump. SHIFT -> ZC_FINE x finer. */
static void on_motion(ZcUI *ui, double y, unsigned mods) {
  if (ui->drag_knob < 0) return;
  int k = ui->drag_knob;
  double range = ((mods & PUGL_MOD_SHIFT) ? DRAG_RANGE * ZC_FINE : DRAG_RANGE) / (ui->scale > 0 ? ui->scale : 1.0);
  ui->drag_val += (ui->drag_yprev - y) / range;
  ui->drag_yprev = y;
  if (ui->drag_val < 0) ui->drag_val = 0;
  if (ui->drag_val > 1) ui->drag_val = 1;
  write_port(ui, KNOBS[k].port, (float)ui->drag_val);
  ui->show_knob = k; ui->show_time = puglGetTime(ui->world);
  redraw(ui);
}
static void on_release(ZcUI *ui) {
  if (ui->drag_knob >= 0) zc_touch(ui, KNOBS[ui->drag_knob].port, false);
  ui->drag_knob = -1;
}

/* The panel image at the window's size: resampled once per size (GOOD filter), blitted every frame. The backdrop
 * round it (letterboxing) is the render's own dark edge colour. */
static void ensure_bg(ZcUI *ui, cairo_t *target) {
  if (ui->bg && ui->bg_w == ui->win_w && ui->bg_h == ui->win_h) return;
  if (ui->bg) cairo_surface_destroy(ui->bg);
  ui->bg = cairo_surface_create_similar(cairo_get_target(target), CAIRO_CONTENT_COLOR, ui->win_w, ui->win_h);
  cairo_t *c = cairo_create(ui->bg);
  cairo_set_source_rgb(c, 0.055, 0.055, 0.059); cairo_paint(c);
  if (ui->img.panel) {
    cairo_translate(c, ui->ox, ui->oy); cairo_scale(c, ui->scale, ui->scale);
    cairo_set_source_surface(c, ui->img.panel, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
    cairo_paint(c);
  }
  cairo_destroy(c);
  ui->bg_w = ui->win_w; ui->bg_h = ui->win_h;
}

static void on_expose(ZcUI *ui, PuglView *view) {
  cairo_t *cr = (cairo_t*)puglGetContext(view);
  if (!cr) return;
  ensure_bg(ui, cr);
  cairo_set_source_surface(cr, ui->bg, 0, 0); cairo_paint(cr);
  cairo_save(cr);
  cairo_translate(cr, ui->ox, ui->oy);
  cairo_scale(cr, ui->scale, ui->scale);
  cairo_rectangle(cr, 0, 0, PH_W, PH_H); cairo_clip(cr);
  /* the plate is already in the cached layer: draw everything else (ph_draw without its plate) */
  PhImages live = ui->img; live.panel = NULL;   /* the plate is in the cached layer */
  ph_draw(cr, &live, ui->v, ui->show_knob, ui->anim, ui->set_open, ui->set_hover_seg, ui->set_hover_close);
  cairo_restore(cr);
}

static PuglStatus on_event(PuglView *view, const PuglEvent *e) {
  ZcUI *ui = (ZcUI*)puglGetHandle(view);
  if (!ui) return PUGL_SUCCESS;
  double s = ui->scale > 0 ? ui->scale : 1.0;
  switch (e->type) {
    case PUGL_CONFIGURE:
      ui->win_w = (int)e->configure.width; ui->win_h = (int)e->configure.height;
      recompute_scale(ui); redraw(ui); break;
    case PUGL_EXPOSE:         on_expose(ui, view); break;
    /* the left button only (pugl numbers from 0): another button must neither start a drag nor re-target one */
    case PUGL_BUTTON_PRESS:   if (e->button.button == 0) on_press(ui, (e->button.x - ui->ox)/s, (e->button.y - ui->oy)/s); break;
    case PUGL_BUTTON_RELEASE: if (e->button.button == 0) on_release(ui); break;
    /* a release can be lost (another client grabs the pointer mid-drag, the window loses focus): end the drag there,
     * or the gesture stays open and the knob follows the pointer with the button up */
    case PUGL_POINTER_OUT:    if (e->crossing.mode == PUGL_CROSSING_GRAB) on_release(ui); break;
    case PUGL_FOCUS_OUT:      on_release(ui); break;
    case PUGL_MOTION:
      if (ui->set_open)   /* the card is modal: hover feeds it, never a knob drag underneath */
        on_hover_settings(ui, (e->motion.x - ui->ox)/s, (e->motion.y - ui->oy)/s);
      else
        on_motion(ui, (e->motion.y - ui->oy)/s, e->motion.state);
      break;
    default: break;
  }
  return PUGL_SUCCESS;
}

/* ---------- LV2 UI ---------- */
/* X errors while the editor's window is created or torn down. Xlib's default handler exit()s the whole HOST, so
 * those two moments run under a TEMPORARY handler that records instead, and the previous one is put back (a plugin
 * must never keep a process-wide handler). zc_x_error holds the last error seen inside a guard; UI thread only. */
static int zc_x_error;
static int zc_x_error_record(Display *d, XErrorEvent *e) { (void)d; zc_x_error = e->error_code ? e->error_code : 1; return 0; }
static XErrorHandler zc_x_guard_begin(Display *d) {
  if (d) XSync(d, False);
  zc_x_error = 0;
  return XSetErrorHandler(zc_x_error_record);
}
/* tear the window and the world down under the guard: the window may already be gone (a failed creation, or a host
 * that destroyed the parent first), and freeing it then is a BadWindow */
static void zc_free_view_world(PuglView *view, PuglWorld *world) {
  Display *d = world ? (Display*)puglGetNativeWorld(world) : NULL;
  XErrorHandler prev = zc_x_guard_begin(d);
  if (view) puglFreeView(view);
  if (d) XSync(d, False);
  if (world) puglFreeWorld(world);   /* closes the display: anything it flushes still meets the recorder */
  XSetErrorHandler(prev);
}

static LV2UI_Handle
instantiate(const LV2UI_Descriptor *d, const char *uri, const char *bundle,
            LV2UI_Write_Function wf, LV2UI_Controller ctl,
            LV2UI_Widget *widget, const LV2_Feature *const *features) {
  (void)d; (void)uri;
  ZcUI *ui = (ZcUI*)calloc(1, sizeof(ZcUI));
  if (!ui) return NULL;
  ui->write = wf; ui->controller = ctl;

  /* bundled fonts: <bundle>/fonts (if it fails, ui_panel falls back to the system sans) */
  if (bundle) {
    char fontdir[2048];
    size_t n = strlen(bundle);
    int slash = (n > 0 && bundle[n-1] == '/');
    snprintf(fontdir, sizeof fontdir, "%s%sfonts", bundle, slash ? "" : "/");
    ui->fonts_loaded = zc_fonts_load(fontdir);
    char photodir[2048];
    snprintf(photodir, sizeof photodir, "%s%sphoto", bundle, slash ? "" : "/");
    ph_load(&ui->img, photodir);
  }
  ui->drag_knob = -1; ui->show_knob = -1; ui->last_knob = -1;
  ui->set_open = 0; ui->set_hover_seg = -1; ui->set_hover_close = 0;
  ui->anim[0] = ui->anim[1] = 1.0;   /* start settled, not fading in */
  /* the plugin's factory state (the TTL's defaults) until the host's first port_event */
  for (int i = 0; i < ZC_N_PORTS; i++) ui->v[i] = ZC_TTL_DEFAULT[i];

  PuglNativeView parent = 0;
  LV2_URID_Map *map = NULL; const LV2_Options_Option *opts = NULL;
  for (int i = 0; features && features[i]; i++) {
    if (!strcmp(features[i]->URI, LV2_UI__parent)) parent = (PuglNativeView)(uintptr_t)features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_UI__resize)) ui->resize = (LV2UI_Resize*)features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_UI__touch)) ui->touch = (LV2UI_Touch*)features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_URID__map)) map = (LV2_URID_Map*)features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_OPTIONS__options)) opts = (const LV2_Options_Option*)features[i]->data;
  }
  /* ui:scaleFactor (a HiDPI host): the default and minimum sizes follow it, so the editor is not half-size on 2x */
  double hidpi = 1.0;
  if (map && opts) {
    LV2_URID k_sf = map->map(map->handle, LV2_UI__scaleFactor), k_fl = map->map(map->handle, LV2_ATOM__Float);
    for (const LV2_Options_Option *o = opts; o->key || o->value; o++)
      if (o->key == k_sf && o->type == k_fl && o->size == sizeof(float)) {
        float f = *(const float*)o->value;
        if (isfinite(f) && f >= 0.5f && f <= 4.0f) hidpi = f;
      }
  }
  const double def_w = CANVAS_W * hidpi, def_h = CANVAS_H * hidpi;

  ui->world = puglNewWorld(PUGL_MODULE, 0);
  if (!ui->world) goto fail;
  puglSetWorldString(ui->world, PUGL_CLASS_NAME, "Dynamo0C");
  ui->view = puglNewView(ui->world);
  if (!ui->view) goto fail;

  puglSetHandle(ui->view, ui);
  puglSetEventFunc(ui->view, on_event);
  puglSetBackend(ui->view, puglCairoBackend());
  /* Resizable, but WITHOUT PUGL_FIXED_ASPECT. The locked aspect was what, in Ardour's X11
   * embedding, triggered a size "correction" loop on every control event -> the window
   * DRIFTED in width (grew to the right) when moving the knobs. The panel already letterboxes itself
   * (recompute_scale: uniform scale min(sx,sy) + centering), so it handles any proportion
   * without deforming and without growing. Manual resize is kept; the drift is eliminated. */
  puglSetSizeHint(ui->view, PUGL_DEFAULT_SIZE, (PuglSpan)def_w, (PuglSpan)def_h);
  puglSetSizeHint(ui->view, PUGL_MIN_SIZE, (PuglSpan)(PH_W*PH_MIN_SCALE*hidpi), (PuglSpan)(PH_H*PH_MIN_SCALE*hidpi));
  puglSetViewHint(ui->view, PUGL_RESIZABLE, PUGL_TRUE);
  ui->win_w = (int)def_w; ui->win_h = (int)def_h; recompute_scale(ui);
  if (parent) puglSetParent(ui->view, parent);

  /* Creating the window is where the X server can refuse us (a parent we cannot be a child of): under the guard, a
   * failed editor returns NULL to the host instead of killing it. */
  { Display *dpy = (Display*)puglGetNativeWorld(ui->world);
    XErrorHandler prev = zc_x_guard_begin(dpy);
    PuglStatus st = puglRealize(ui->view);
    if (dpy) XSync(dpy, False);
    XSetErrorHandler(prev);
    if (st != PUGL_SUCCESS || zc_x_error) goto fail; }
  puglShow(ui->view, PUGL_SHOW_RAISE);
  puglObscureView(ui->view);
  if (ui->resize) ui->resize->ui_resize(ui->resize->handle, (int)def_w, (int)def_h);
  *widget = (LV2UI_Widget)puglGetNativeView(ui->view);
  return ui;

fail:   /* everything instantiate may have taken, released (the images, the fonts' reference, the window) */
  zc_free_view_world(ui->view, ui->world);
  ph_free(&ui->img);
  if (ui->fonts_loaded) zc_fonts_free();
  free(ui);
  return NULL;
}

static void cleanup(LV2UI_Handle h) {
  ZcUI *ui = (ZcUI*)h;
  on_release(ui);   /* a drag still open when the editor closes: its ui:touch gesture is closed too */
  if (ui->bg) cairo_surface_destroy(ui->bg);
  ph_free(&ui->img);
  zc_free_view_world(ui->view, ui->world);
  if (ui->fonts_loaded) zc_fonts_free();
  free(ui);
}

static void port_event(LV2UI_Handle h, uint32_t port, uint32_t size,
                       uint32_t format, const void *buf) {
  ZcUI *ui = (ZcUI*)h;
  if (format != 0 || size != sizeof(float) || port >= ZC_N_PORTS) return;
  float val = *(const float*)buf;
#ifndef ZC_UI_TRUST_PORTS   /* the gate's negative arm builds without this guard and must be caught */
  /* A host may hand anything (a broken session, an OSC message): a non-finite value is ignored, a continuous control
   * is clamped to its [0, 1] range, so nothing downstream (the pointer, the read-out's scale index, a drag that writes
   * the value back) ever sees a value outside what the TTL declares. */
  if (!isfinite(val)) return;
  if (port != P_DRIVE_OS && port != P_LATENCY) { if (val < 0.0f) val = 0.0f; if (val > 1.0f) val = 1.0f; }
  else if (port == P_DRIVE_OS) { if (val < 0.0f) val = 0.0f; if (val > (float)(SET_NOPT - 1)) val = (float)(SET_NOPT - 1); }
#endif
  if (ui->v[port] != val) { ui->v[port] = val; redraw(ui); }
}

static int ui_idle(LV2UI_Handle h) {
  ZcUI *ui = (ZcUI*)h;
  /* animate POWER / EQ engage<->bypass toward their targets (0.2s, framerate-independent) */
  { double now = puglGetTime(ui->world);
    double dt = now - ui->last_t; ui->last_t = now;
    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;
    const int ports[2] = { P_POWER, P_EQ_ON };
    for (int i = 0; i < 2; i++) {
      double tgt = ui->v[ports[i]] > 0.5f ? 1.0 : 0.0;
      if (ui->anim[i] != tgt) {
        double st = dt / 0.2;
        if (ui->anim[i] < tgt) { ui->anim[i] += st; if (ui->anim[i] > tgt) ui->anim[i] = tgt; }
        else                   { ui->anim[i] -= st; if (ui->anim[i] < tgt) ui->anim[i] = tgt; }
        redraw(ui);
      }
    } }
  /* expires the read-out after release (1 s with no activity) and with no drag */
  if (ui->show_knob >= 0 && ui->drag_knob < 0 &&
      (puglGetTime(ui->world) - ui->show_time) > 1.0) {
    ui->show_knob = -1; redraw(ui);
  }
  /* FRAME-RATE CAP. A knob drag or a resize can ask for a repaint per pointer event, and the host's UI thread runs
   * these idles: anything that repaints that often starves it (a vector panel of this family once froze Ardour). 30
   * fps is plenty; needs_redraw stays set, so nothing is dropped, only coalesced. */
  { double np = puglGetTime(ui->world);
    if (ui->needs_redraw && (np - ui->last_paint) >= (1.0/30.0)) {
      ui->needs_redraw = 0; ui->last_paint = np; puglObscureView(ui->view); } }
  puglUpdate(ui->world, 0.0);
  return 0;
}

/* NOTE ON LV2_UI__resize, which this deliberately does NOT export.
 *
 * The host-calls-us direction of that extension is unusable here, and was a crash. extension_data
 * is a plain lookup with no instance argument, so the LV2UI_Resize it returns has to be a static
 * whose `handle` cannot point at any particular UI -- it was { NULL, ui_resize_cb }, and hosts
 * (Ardour and jalv both do this) call it as resize->ui_resize(resize->handle, w, h). The callback
 * dereferenced that NULL on its first line.
 *
 * There is no way to fix it while keeping the extension: the handle simply is not available at
 * lookup time. Removing it costs nothing, because Pugl already delivers PUGL_CONFIGURE for every
 * size change, which is where the UI actually learns its dimensions. The OTHER direction --
 * ui->resize, the feature the host passes to instantiate so we can ask IT to resize us -- is
 * untouched and still works. */
static const void *extension_data(const char *uri) {
  static const LV2UI_Idle_Interface idle = { ui_idle };
  if (!strcmp(uri, LV2_UI__idleInterface)) return &idle;
  return NULL;
}

static const LV2UI_Descriptor descriptor = {
  ZC_UI_URI, instantiate, cleanup, port_event, extension_data
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
