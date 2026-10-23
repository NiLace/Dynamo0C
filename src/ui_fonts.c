/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* ui_fonts.c -- implementation of ui_fonts.h. FreeType loads each TTF; cairo-ft wraps the
 * FT_Face in a cairo_font_face_t.
 *
 * Each cairo face owns a PRIVATE FT_Library holding its one FT_Face, hung on the face as user
 * data whose destructor is FreeType's own FT_Done_FreeType (which also frees the face). cairo
 * keeps faces in a process-wide cache past our cairo_font_face_destroy, possibly after the host
 * has closed the editor and dlclosed this module (built with -fvisibility=hidden, it really
 * unloads). When cairo finally drops a face, FreeType frees it, and nothing calls back into this
 * module and nothing is freed under cairo (freeing the FT_Face and its library on close, under faces
 * cairo still held, was a use-after-free in the host). Gate: make ui-unload.
 *
 * FT_Done_FreeType is FT_Error(*)(FT_Library) called as cairo's void(*)(void*): a conversion C
 * leaves undefined and the x86-64 System V ABI makes exact (the pointer travels in the same
 * register, the int result is discarded). A wrapper of the right type would be code in THIS
 * module, which is what must not run after an unload.
 *
 * ZC_UI_FONTS_SHARED_FT=1 builds the old code, for the gate's negative arm only. */
#include "ui_fonts.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <cairo/cairo-ft.h>
#include <stdio.h>

#ifndef ZC_UI_FONTS_SHARED_FT
#define ZC_UI_FONTS_SHARED_FT 0
#endif

cairo_font_face_t *zc_face[ZF_COUNT] = {0};

static int s_refs = 0;   /* number of live UIs: our references are dropped when it reaches 0 */

/* file per role (order = ZcFontId). */
static const char *const FILES[ZF_COUNT] = {
  "Oswald-SemiBold.ttf",          /* ZF_BRAND */
  "Oswald-Medium.ttf",            /* ZF_HEAD  */
  "BarlowCondensed-SemiBold.ttf", /* ZF_LABEL */
  "BarlowCondensed-Medium.ttf",   /* ZF_TICK  */
  "Inter-SemiBold.ttf",           /* ZF_VALUE */
};

#if ZC_UI_FONTS_SHARED_FT
static FT_Library s_ft;
static FT_Face    s_ftface[ZF_COUNT] = {0};

int zc_fonts_load(const char *dir) {
  if (s_refs) { s_refs++; return 1; }
  if (!dir || FT_Init_FreeType(&s_ft)) return 0;
  s_refs = 1;
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
  return 1;
}

void zc_fonts_free(void) {
  if (!s_refs || --s_refs > 0) return;
  for (int i = 0; i < ZF_COUNT; i++) {
    if (zc_face[i])   { cairo_font_face_destroy(zc_face[i]); zc_face[i] = NULL; }
    if (s_ftface[i])  { FT_Done_Face(s_ftface[i]);           s_ftface[i] = NULL; }
  }
  FT_Done_FreeType(s_ft);
}
#else
static cairo_user_data_key_t s_lib_key;

/* One face from one file, owning its own FreeType library (see the top). NULL on any failure. */
static cairo_font_face_t *load_face(const char *path) {
  FT_Library lib = NULL;
  FT_Face face = NULL;
  if (FT_Init_FreeType(&lib)) return NULL;
  if (FT_New_Face(lib, path, 0, &face)) { FT_Done_FreeType(lib); return NULL; }
  cairo_font_face_t *f = cairo_ft_font_face_create_for_ft_face(face, 0);
  if (cairo_font_face_status(f) != CAIRO_STATUS_SUCCESS
      || cairo_font_face_set_user_data(f, &s_lib_key, lib,
                                       (cairo_destroy_func_t)(void (*)(void))FT_Done_FreeType)
         != CAIRO_STATUS_SUCCESS) {
    cairo_font_face_destroy(f);
    FT_Done_FreeType(lib);
    return NULL;
  }
  return f;
}

int zc_fonts_load(const char *dir) {
  if (s_refs) { s_refs++; return 1; }   /* already up: take a reference */
  if (!dir) return 0;                   /* nothing to load: NO reference taken */
  s_refs = 1;   /* a reference is held even if some faces fail (the panel falls back for those) */
  for (int i = 0; i < ZF_COUNT; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, FILES[i]);
    zc_face[i] = load_face(path);
  }
  return 1;
}

void zc_fonts_free(void) {
  if (!s_refs || --s_refs > 0) return;   /* there are still UIs using the shared faces */
  /* Drop OUR references only: cairo may hold its own for a while, and the face's private
   * library goes with the last one, by FreeType's code. */
  for (int i = 0; i < ZF_COUNT; i++)
    if (zc_face[i]) { cairo_font_face_destroy(zc_face[i]); zc_face[i] = NULL; }
}
#endif
