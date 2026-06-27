/* ui_fonts.c -- implementation of ui_fonts.h. FreeType loads each TTF; cairo-ft wraps the
 * FT_Face in a cairo_font_face_t. The FT_Face must outlive the cairo face (cairo references
 * it, does not copy it), which is why both are kept and freed in reverse order. */
#include "ui_fonts.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <cairo/cairo-ft.h>
#include <stdio.h>

cairo_font_face_t *zc_face[ZF_COUNT] = {0};

static FT_Library s_ft;
static FT_Face    s_ftface[ZF_COUNT] = {0};
static int        s_loaded = 0;
static int        s_refs   = 0;   /* number of live UIs: only freed when it reaches 0 */

/* file per role (order = ZcFontId). */
static const char *const FILES[ZF_COUNT] = {
  "Oswald-SemiBold.ttf",          /* ZF_BRAND */
  "Oswald-Medium.ttf",            /* ZF_HEAD  */
  "BarlowCondensed-SemiBold.ttf", /* ZF_LABEL */
  "BarlowCondensed-Medium.ttf",   /* ZF_TICK  */
  "Inter-SemiBold.ttf",           /* ZF_VALUE */
};

int zc_fonts_load(const char *dir) {
  if (s_loaded) { s_refs++; return 1; }            /* already up: take a reference */
  if (!dir || FT_Init_FreeType(&s_ft)) return 0;   /* could not initialize: NO reference taken */
  s_loaded = 1; s_refs = 1;   /* a reference is now held even if some faces fail (the rest fall back) */
  for (int i = 0; i < ZF_COUNT; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, FILES[i]);
    if (FT_New_Face(s_ft, path, 0, &s_ftface[i])) { s_ftface[i] = NULL; zc_face[i] = NULL; continue; }
    cairo_font_face_t *f = cairo_ft_font_face_create_for_ft_face(s_ftface[i], 0);
    if (cairo_font_face_status(f) != CAIRO_STATUS_SUCCESS) {
      cairo_font_face_destroy(f);
      FT_Done_Face(s_ftface[i]); s_ftface[i] = NULL; zc_face[i] = NULL; continue;
    }
    zc_face[i] = f;
  }
  return 1;   /* reference held; per-font success is reflected in zc_face[] (NULL = fell back) */
}

void zc_fonts_free(void) {
  if (!s_loaded) return;
  if (--s_refs > 0) return;   /* there are still UIs using the shared faces */
  for (int i = 0; i < ZF_COUNT; i++) {
    if (zc_face[i])   { cairo_font_face_destroy(zc_face[i]); zc_face[i] = NULL; }
    if (s_ftface[i])  { FT_Done_Face(s_ftface[i]);           s_ftface[i] = NULL; }
  }
  FT_Done_FreeType(s_ft);
  s_loaded = 0;
}
