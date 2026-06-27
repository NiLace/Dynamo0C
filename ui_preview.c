/* ui_preview.c -- renders the panel to PNG (no Pugl/host) to iterate on the look.
 *   ./ui_preview out.png [scale] [fontdir]                                         */
#include <stdlib.h>
#include "ports.h"
#include "ui_fonts.h"
#include "ui_panel.h"

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "preview.png";
  double sc = argc > 2 ? atof(argv[2]) : 2.0;
  const char *fontdir = argc > 3 ? argv[3] : "fonts";
  zc_fonts_load(fontdir);
  for (int i = 0; i < ZF_COUNT; i++)
    if (!zc_face[i]) {
      fprintf(stderr, "warning: not all fonts loaded from '%s' (falling back to system)\n", fontdir);
      break;
    }

  float v[ZC_N_PORTS]; for (int i = 0; i < ZC_N_PORTS; i++) v[i] = 0.0f;
  v[P_POWER] = 1; v[P_EQ_ON] = 1; v[P_HP_BUMP] = 1;
  /* knob defaults */
  for (int i = 0; i < ZC_NKNOB; i++) v[KNOBS[i].port] = (float)KNOBS[i].def;
  v[P_HF_SHELF] = 1;   /* HIGH = shelf by default */
  v[P_LF_SHELF] = 0;   /* LOW  = bell */
  /* a realistic position to see curves */
  v[P_HI_GAIN] = 0.70f; v[P_LO_GAIN] = 0.78f; v[P_HIMID_GAIN] = 0.62f;
  v[P_HP_FREQ] = 0.25f; v[P_LP_FREQ] = 0.80f;

  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                          (int)(PANEL_W*sc), (int)(PANEL_H*sc));
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, sc, sc);
  /* sample read-out on the HIGH GAIN (knob index 1) */
  ZcHit hits[2]; int nhits = 0;
  zc_draw_panel(cr, v, 1, hits, &nhits);
  cairo_surface_write_to_png(s, out);
  cairo_destroy(cr); cairo_surface_destroy(s);
  zc_fonts_free();
  printf("written %s  (%dx%d)\n", out, (int)(PANEL_W*sc), (int)(PANEL_H*sc));
  return 0;
}
