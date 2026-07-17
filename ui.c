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

  int    set_open;         /* the OVERSAMPLING card is open (opened from the DRIVE label) */
  int    set_hover_seg;    /* segment under the pointer while it is open (-1 = none) */
  int    set_hover_close;  /* pointer is over the close X */

  ZcHit  hits[2]; int nhits;   /* bell/shelf icon hit-boxes, per instance, filled at draw time */

  double anim[2];          /* animated engaged-ness of POWER / EQ (0..1), 0.2s fades */
  double last_t;           /* last idle timestamp -> framerate-independent animation */
  double last_paint;       /* timestamp of the last real repaint (frame-rate cap, see ui_idle) */
  cairo_surface_t *bg;     /* cached STATIC layer: backdrop + shadow + chassis + everything that
                            * does not follow a live port value (see zc_draw_static) */
  int    bg_w, bg_h;       /* window size the cache was built for */
  double bg_anim[2]; int bg_key;   /* the rest of its key: fades + the discrete switch states */

  int    win_w, win_h;     /* current window size */
  double scale, ox, oy;    /* logical->window transform (uniform scale + centering) */
  int    fonts_loaded;     /* nonzero if this instance holds a font reference (balances the free) */
} ZcUI;

/* recompute uniform scale + centering offset based on the window size */
/* the layout canvas is the chassis PLUS a margin of backdrop all around (chassis size unchanged) */
#define CANVAS_W (PANEL_W + 2*UI_MARGIN)
#define CANVAS_H (PANEL_H + 2*UI_MARGIN)
static void recompute_scale(ZcUI *ui) {
  double sx = (double)ui->win_w / PANEL_W, sy = (double)ui->win_h / PANEL_H;
  ui->scale = sx < sy ? sx : sy;
  ui->ox = (ui->win_w - CANVAS_W * ui->scale) / 2.0;
  ui->oy = (ui->win_h - CANVAS_H * ui->scale) / 2.0;
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

  /* OVERSAMPLING card open: it is MODAL. The X closes, a segment selects (and closes), and a click
   * anywhere outside the card closes without changing anything. Nothing under the card can be hit
   * -- that is the point of the dim. */
  if (ui->set_open) {
    double dxc = x - SET_CLOSE_CX, dyc = y - SET_CLOSE_CY;
    if (dxc*dxc + dyc*dyc <= SET_CLOSE_R*SET_CLOSE_R) {   /* the X */
      ui->set_open = 0; redraw(ui); ui->last_knob = -1; return;
    }
    for (int i = 0; i < SET_NOPT; i++) {
      double sx, sy, sw, sh; set_seg_rect(i, &sx, &sy, &sw, &sh);
      if (x >= sx && x <= sx+sw && y >= sy && y <= sy+sh) {
        write_port(ui, P_DRIVE_OS, (float)i);
        ui->set_open = 0; redraw(ui); ui->last_knob = -1; return;
      }
    }
    if (!set_in_card(x, y)) { ui->set_open = 0; redraw(ui); }   /* click outside -> close */
    ui->last_knob = -1; return;
  }
  /* clicking the TITLE toggles engage/bypass -- works even while bypassed (it is the only thing
   * that does). Replaces the old POWER switch. */
  { double tx, ty, tw, th; title_rect(&tx, &ty, &tw, &th);
    if (x >= tx && x <= tx+tw && y >= ty && y <= ty+th) {
      write_port(ui, P_POWER, ui->v[P_POWER] > 0.5f ? 0.0f : 1.0f);
      redraw(ui); ui->last_knob = -1; return; } }
  /* while bypassed, only the title responds */
  if (ui->v[P_POWER] <= 0.5f) { ui->last_knob = -1; return; }

  /* the DRIVE LABEL is a control: it opens the OVERSAMPLING card (family pattern -- this is the
   * card's original protocol, it just used to hang off the title). Tested BEFORE the knob: the two
   * boxes do not overlap (label 130..180, knob 192..228), but the label must win if that changes. */
  { double lx, ly, lw, lh; drive_label_rect(&lx, &ly, &lw, &lh);
    if (x >= lx && x <= lx+lw && y >= ly && y <= ly+lh) {
      ui->set_open = 1; ui->set_hover_seg = -1; ui->set_hover_close = 0;
      redraw(ui); ui->last_knob = -1; return; } }

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
/* hover feedback while the card is open: the X and the segments light under the pointer.
 * Only redraws when the hovered thing actually CHANGES -- a motion event per pixel that repainted
 * the panel would cost a core for nothing (see the family's UI perf notes). */
static void on_hover_settings(ZcUI *ui, double x, double y) {
  int seg = -1;
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

/* hover feedback while the settings overlay is open (highlight segment / close-X) */

/* The backdrop, the drop shadow and the chassis depend ONLY on the window size -- never on a port
 * value or on dim. Redrawing them every frame was most of the cost. Render once, blit after;
 * rebuild only on resize. */
static void ensure_bg(ZcUI *ui, cairo_t *target) {
  /* the static layer also depends on the discrete switch states.
   * NOT on the OS any more: its only static consumer was the SW_OS cell, and that cell is gone.
   * The OS now shows on the DRIVE label, which draw_knob_live paints every frame -- so keying the
   * cache on it would rebuild the whole chassis (the expensive layer) on every OS change, for a
   * pixel the cache never held. */
  int key = (ui->v[P_LF_SHELF]>0.5f) | ((ui->v[P_HF_SHELF]>0.5f)<<1) | ((ui->v[P_EQ_ON]>0.5f)<<2)
          | ((ui->v[P_HP_BUMP]>0.5f)<<3);
  if (ui->bg && ui->bg_w == ui->win_w && ui->bg_h == ui->win_h
      && ui->bg_anim[0]==ui->anim[0] && ui->bg_anim[1]==ui->anim[1] && ui->bg_key==key) return;
  if (ui->bg) cairo_surface_destroy(ui->bg);
  ui->bg = cairo_surface_create_similar(cairo_get_target(target), CAIRO_CONTENT_COLOR_ALPHA,
                                        ui->win_w, ui->win_h);
  cairo_t *c = cairo_create(ui->bg);
  zc_backdrop(c, (double)ui->win_w, (double)ui->win_h);
  cairo_translate(c, ui->ox, ui->oy);
  cairo_scale(c, ui->scale, ui->scale);
  cairo_translate(c, UI_MARGIN, UI_MARGIN);
  zc_shadow(c);
  cairo_save(c); rrect(c, 0, 0, PANEL_W, PANEL_H, 11); cairo_clip(c);
  zc_draw_chassis(c);
  zc_draw_static(c, ui->v, ui->hits, &ui->nhits, ui->anim);
  cairo_restore(c);
  cairo_destroy(c);
  ui->bg_w = ui->win_w; ui->bg_h = ui->win_h;
  ui->bg_anim[0]=ui->anim[0]; ui->bg_anim[1]=ui->anim[1]; ui->bg_key=key;
}

static void on_expose(ZcUI *ui, PuglView *view) {
  cairo_t *cr = (cairo_t*)puglGetContext(view);
  if (!cr) return;
  /* 1) the cached static layer (backdrop + shadow + chassis) */
  ensure_bg(ui, cr);
  cairo_set_source_surface(cr, ui->bg, 0, 0); cairo_paint(cr);
  /* 2) only what actually changes, on top */
  cairo_save(cr);
  cairo_translate(cr, ui->ox, ui->oy);
  cairo_scale(cr, ui->scale, ui->scale);
  cairo_translate(cr, UI_MARGIN, UI_MARGIN);
  cairo_save(cr); rrect(cr, 0, 0, PANEL_W, PANEL_H, 11); cairo_clip(cr);
  zc_draw_live(cr, ui->v, ui->show_knob, ui->anim, ui->set_open);
  /* the card stays INSIDE the chassis clip: it dims the panel, and an unclipped square dim would
   * paint over the chassis' rounded corners and out onto the backdrop. */
  if (ui->set_open) zc_draw_settings(cr, ui->v, ui->set_hover_seg, ui->set_hover_close);
  cairo_restore(cr);
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
    case PUGL_BUTTON_PRESS:   on_press(ui, (e->button.x - ui->ox)/s - UI_MARGIN, (e->button.y - ui->oy)/s - UI_MARGIN); break;
    case PUGL_BUTTON_RELEASE: on_release(ui); break;
    case PUGL_MOTION:
      if (ui->set_open)   /* the card is modal: hover feeds it, never a knob drag underneath */
        on_hover_settings(ui, (e->motion.x - ui->ox)/s - UI_MARGIN,
                              (e->motion.y - ui->oy)/s - UI_MARGIN);
      else
        on_motion(ui, (e->motion.y - ui->oy)/s - UI_MARGIN, e->motion.state);
      break;
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
  ui->set_open = 0; ui->set_hover_seg = -1; ui->set_hover_close = 0;
  ui->anim[0] = ui->anim[1] = 1.0;   /* start settled, not fading in */
  /* default values (until the host sends port_event) */
  for (int i = 0; i < ZC_N_PORTS; i++) ui->v[i] = 0.0f;
  ui->v[P_POWER] = 1; ui->v[P_EQ_ON] = 1; ui->v[P_HF_SHELF] = 1;
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
  puglSetSizeHint(ui->view, PUGL_DEFAULT_SIZE, (PuglSpan)CANVAS_W, (PuglSpan)CANVAS_H);
  puglSetSizeHint(ui->view, PUGL_MIN_SIZE, (PuglSpan)(CANVAS_W*0.6), (PuglSpan)(CANVAS_H*0.6));
  puglSetViewHint(ui->view, PUGL_RESIZABLE, PUGL_TRUE);
  ui->win_w = (int)CANVAS_W; ui->win_h = (int)CANVAS_H; recompute_scale(ui);
  if (parent) puglSetParent(ui->view, parent);

  if (puglRealize(ui->view) != PUGL_SUCCESS) {
    puglFreeView(ui->view); puglFreeWorld(ui->world); free(ui); return NULL;
  }
  puglShow(ui->view, PUGL_SHOW_RAISE);
  puglObscureView(ui->view);
  if (ui->resize) ui->resize->ui_resize(ui->resize->handle, (int)CANVAS_W, (int)CANVAS_H);
  *widget = (LV2UI_Widget)puglGetNativeView(ui->view);
  return ui;
}

static void cleanup(LV2UI_Handle h) {
  ZcUI *ui = (ZcUI*)h;
  if (ui->bg) cairo_surface_destroy(ui->bg);
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
  /* animate POWER / EQ engage<->bypass toward their targets (0.2s, framerate-independent) */
  { double now = puglGetTime(ui->world);
    double dt = now - ui->last_t; ui->last_t = now;
    if (dt < 0) dt = 0; if (dt > 0.1) dt = 0.1;
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
  /* FRAME-RATE CAP. Each repaint is a FULL redraw (this panel has 11 knobs -- measured ~20 ms), so
   * anything that redraws often eats a core, per open window. The host's UI thread runs these
   * idles, so it starves it (Ardour froze). 30 fps is plenty; needs_redraw stays set -> nothing is
   * dropped, only coalesced. */
  { double np = puglGetTime(ui->world);
    if (ui->needs_redraw && (np - ui->last_paint) >= (1.0/30.0)) {
      ui->needs_redraw = 0; ui->last_paint = np; puglObscureView(ui->view); } }
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
