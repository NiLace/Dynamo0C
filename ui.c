/* ui.c -- Dynamo 0C EQ : LV2 UI (Pugl + Cairo). Single translation unit.
 * Drawing in ui_panel.h (pure module). Here: Pugl window, events, control-port writes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pugl/pugl.h>
#include <pugl/cairo.h>
#include <cairo/cairo.h>

#ifdef HAVE_LV2_1_18_6
#include <lv2/ui/ui.h>
#else
#include <lv2/lv2plug.in/ns/extensions/ui/ui.h>
#endif

#include "uris.h"
#include "ports.h"
#include "ui_fonts.h"
#include "ui_panel.h"

#define DRAG_RANGE 200.0   /* px of drag = full travel */
#define DCLICK_S   0.35    /* double-click window (s) */
#define ZC_FINE    6.0     /* hold SHIFT while dragging -> this many times finer */

typedef struct {
  LV2UI_Write_Function write;
  LV2UI_Controller     controller;
  LV2UI_Resize        *resize;
  PuglWorld           *world;
  PuglView            *view;

  float  v[ZC_N_PORTS];

  int    drag_knob;        /* -1 or index */
  double drag_yprev, drag_val;   /* incremental drag: last pointer y + accumulated 0..1 value */
  int    show_knob; double show_time;
  int    last_knob; double last_press;
  int    needs_redraw;

  ZcHit  hits[2]; int nhits;   /* bell/shelf icon hit-boxes, per instance, filled at draw time */

  int    win_w, win_h;     /* current window size */
  double scale, ox, oy;    /* logical->window transform (uniform scale + centering) */
  int    fonts_loaded;     /* nonzero if this instance holds a font reference (balances the free) */
} ZcUI;

/* recompute uniform scale + centering offset based on the window size */
static void recompute_scale(ZcUI *ui) {
  double sx = (double)ui->win_w / PANEL_W, sy = (double)ui->win_h / PANEL_H;
  ui->scale = sx < sy ? sx : sy;
  ui->ox = (ui->win_w - PANEL_W * ui->scale) / 2.0;
  ui->oy = (ui->win_h - PANEL_H * ui->scale) / 2.0;
}

/* ---------- port writes ---------- */
static void write_port(ZcUI *ui, int port, float val) {
  ui->v[port] = val;
  ui->write(ui->controller, (uint32_t)port, sizeof(float), 0, &val);
}
static void redraw(ZcUI *ui) { ui->needs_redraw = 1; }

/* ---------- hit-testing: works in the panel's LOGICAL coords.
 *   on_event already converts window coords to logical (subtract the centering, divide by the
 *   scale) before calling here, so these tests are independent of the current size. */
static int knob_at(double x, double y) {
  for (int i = 0; i < ZC_NKNOB; i++) {
    double R = (KNOBS[i].r > 0.0) ? KNOBS[i].r : KNOB_R;
    double dx = x - KNOBS[i].cx, dy = y - KNOBS[i].cy;
    if (dx*dx + dy*dy <= (R+7)*(R+7)) return i;
  }
  return -1;
}
static int switch_at(double x, double y) {
  for (int i = 0; i < ZC_NSW; i++) {
    double sx, sy, sw, sh; switch_rect(i, &sx, &sy, &sw, &sh);
    if (x >= sx && x <= sx + sw && y >= sy && y <= sy + sh) return i;
  }
  return -1;
}

/* ---------- events ---------- */
static void on_press(ZcUI *ui, double x, double y) {
  double now = puglGetTime(ui->world);
  int k = knob_at(x, y);
  if (k >= 0) {
    if (ui->last_knob == k && (now - ui->last_press) < DCLICK_S) {   /* double-click = reset */
      write_port(ui, KNOBS[k].port, (float)KNOBS[k].def);
      ui->show_knob = k; ui->show_time = now; ui->drag_knob = -1; redraw(ui);
    } else {
      ui->drag_knob = k; ui->drag_yprev = y; ui->drag_val = ui->v[KNOBS[k].port];
      ui->show_knob = k; ui->show_time = now; redraw(ui);
    }
    ui->last_knob = k; ui->last_press = now;
    return;
  }
  /* header bell/shelf icon (HIGH/LOW): toggles like its LF/HF switch */
  for (int i = 0; i < ui->nhits; i++) {
    const ZcHit *h = &ui->hits[i];
    if (x >= h->x0 && x <= h->x1 && y >= h->y0 && y <= h->y1) {
      write_port(ui, h->port, ui->v[h->port] > 0.5f ? 0.0f : 1.0f);
      redraw(ui); ui->last_knob = -1; return;
    }
  }
  int s = switch_at(x, y);
  if (s >= 0) {
    float nv = ui->v[SWITCHES[s].port] > 0.5f ? 0.0f : 1.0f;
    write_port(ui, SWITCHES[s].port, nv);
    if (SWITCHES[s].port2 >= 0) write_port(ui, SWITCHES[s].port2, nv);   /* BUMP: HP bump + LP resonant together */
    redraw(ui);
  }
  ui->last_knob = -1;
}
/* Incremental drag: accumulate the delta each motion (not absolute from the press point), so that
 * pressing/releasing SHIFT mid-drag switches coarse<->fine with no jump. SHIFT -> ZC_FINE x finer. */
static void on_motion(ZcUI *ui, double y, unsigned mods) {
  if (ui->drag_knob < 0) return;
  int k = ui->drag_knob;
  double range = (mods & PUGL_MOD_SHIFT) ? DRAG_RANGE * ZC_FINE : DRAG_RANGE;
  ui->drag_val += (ui->drag_yprev - y) / range;
  ui->drag_yprev = y;
  if (ui->drag_val < 0) ui->drag_val = 0;
  if (ui->drag_val > 1) ui->drag_val = 1;
  write_port(ui, KNOBS[k].port, (float)ui->drag_val);
  ui->show_knob = k; ui->show_time = puglGetTime(ui->world);
  redraw(ui);
}
static void on_release(ZcUI *ui) { ui->drag_knob = -1; }

static void on_expose(ZcUI *ui, PuglView *view) {
  cairo_t *cr = (cairo_t*)puglGetContext(view);
  if (!cr) return;
  /* clears the whole window (letterbox bars) and draws the panel scaled and centered */
  cairo_set_source_rgb(cr, 0.039, 0.039, 0.035);
  cairo_paint(cr);
  cairo_save(cr);
  cairo_translate(cr, ui->ox, ui->oy);
  cairo_scale(cr, ui->scale, ui->scale);
  zc_draw_panel(cr, ui->v, ui->show_knob, ui->hits, &ui->nhits);
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
    case PUGL_BUTTON_PRESS:   on_press(ui, (e->button.x - ui->ox)/s, (e->button.y - ui->oy)/s); break;
    case PUGL_BUTTON_RELEASE: on_release(ui); break;
    case PUGL_MOTION:         on_motion(ui, (e->motion.y - ui->oy)/s, e->motion.state); break;
    default: break;
  }
  return PUGL_SUCCESS;
}

/* ---------- LV2 UI ---------- */
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
  }
  ui->drag_knob = -1; ui->show_knob = -1; ui->last_knob = -1;
  /* default values (until the host sends port_event) */
  for (int i = 0; i < ZC_N_PORTS; i++) ui->v[i] = 0.0f;
  ui->v[P_POWER] = 1; ui->v[P_EQ_ON] = 1; ui->v[P_HF_SHELF] = 1; ui->v[P_OVERSAMPLE] = 3; /* 3 = Auto */
  ui->v[P_HP_BUMP] = 1;
  for (int i = 0; i < ZC_NKNOB; i++) ui->v[KNOBS[i].port] = (float)KNOBS[i].def;

  PuglNativeView parent = 0;
  for (int i = 0; features && features[i]; i++) {
    if (!strcmp(features[i]->URI, LV2_UI__parent)) parent = (PuglNativeView)(uintptr_t)features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_UI__resize)) ui->resize = (LV2UI_Resize*)features[i]->data;
  }

  ui->world = puglNewWorld(PUGL_MODULE, 0);
  if (!ui->world) { free(ui); return NULL; }
  puglSetWorldString(ui->world, PUGL_CLASS_NAME, "Dynamo0C");
  ui->view = puglNewView(ui->world);
  if (!ui->view) { puglFreeWorld(ui->world); free(ui); return NULL; }

  puglSetHandle(ui->view, ui);
  puglSetEventFunc(ui->view, on_event);
  puglSetBackend(ui->view, puglCairoBackend());
  /* Resizable, but WITHOUT PUGL_FIXED_ASPECT. The locked aspect was what, in Ardour's X11
   * embedding, triggered a size "correction" loop on every control event -> the window
   * DRIFTED in width (grew to the right) when moving the knobs. The panel already letterboxes itself
   * (recompute_scale: uniform scale min(sx,sy) + centering), so it handles any proportion
   * without deforming and without growing. Manual resize is kept; the drift is eliminated. */
  puglSetSizeHint(ui->view, PUGL_DEFAULT_SIZE, PANEL_W, PANEL_H);
  puglSetSizeHint(ui->view, PUGL_MIN_SIZE, (PuglSpan)(PANEL_W*0.6), (PuglSpan)(PANEL_H*0.6));
  puglSetViewHint(ui->view, PUGL_RESIZABLE, PUGL_TRUE);
  ui->win_w = PANEL_W; ui->win_h = PANEL_H; recompute_scale(ui);
  if (parent) puglSetParent(ui->view, parent);

  if (puglRealize(ui->view) != PUGL_SUCCESS) {
    puglFreeView(ui->view); puglFreeWorld(ui->world); free(ui); return NULL;
  }
  puglShow(ui->view, PUGL_SHOW_RAISE);
  puglObscureView(ui->view);
  if (ui->resize) ui->resize->ui_resize(ui->resize->handle, PANEL_W, PANEL_H);
  *widget = (LV2UI_Widget)puglGetNativeView(ui->view);
  return ui;
}

static void cleanup(LV2UI_Handle h) {
  ZcUI *ui = (ZcUI*)h;
  if (ui->view)  puglFreeView(ui->view);
  if (ui->world) puglFreeWorld(ui->world);
  if (ui->fonts_loaded) zc_fonts_free();
  free(ui);
}

static void port_event(LV2UI_Handle h, uint32_t port, uint32_t size,
                       uint32_t format, const void *buf) {
  ZcUI *ui = (ZcUI*)h;
  if (format != 0 || size != sizeof(float) || port >= ZC_N_PORTS) return;
  float val = *(const float*)buf;
  if (ui->v[port] != val) { ui->v[port] = val; redraw(ui); }
}

static int ui_idle(LV2UI_Handle h) {
  ZcUI *ui = (ZcUI*)h;
  /* expires the read-out after release (1 s with no activity) and with no drag */
  if (ui->show_knob >= 0 && ui->drag_knob < 0 &&
      (puglGetTime(ui->world) - ui->show_time) > 1.0) {
    ui->show_knob = -1; redraw(ui);
  }
  if (ui->needs_redraw) { ui->needs_redraw = 0; puglObscureView(ui->view); }
  puglUpdate(ui->world, 0.0);
  return 0;
}

/* the host notifies us of a new window size (user resize) */
static int ui_resize_cb(LV2UI_Feature_Handle handle, int width, int height) {
  ZcUI *ui = (ZcUI*)handle;
  ui->win_w = width; ui->win_h = height; recompute_scale(ui); redraw(ui);
  return 0;
}

static const void *extension_data(const char *uri) {
  static const LV2UI_Idle_Interface idle = { ui_idle };
  static const LV2UI_Resize resize = { NULL, ui_resize_cb };
  if (!strcmp(uri, LV2_UI__idleInterface)) return &idle;
  if (!strcmp(uri, LV2_UI__resize))        return &resize;
  return NULL;
}

static const LV2UI_Descriptor descriptor = {
  ZC_UI_URI, instantiate, cleanup, port_event, extension_data
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index) {
  return index == 0 ? &descriptor : NULL;
}
