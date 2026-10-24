/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) The NL Sounds contributors */
/* ui_fonts.h -- loading of the bundled font (FreeType -> cairo-ft) for the Dynamo 0C EQ panel. Every word on the
 * panel is printed in its image; the one live text, a knob's value while it moves, uses this face. If it did not load
 * (NULL), ui_panel.h falls back to Cairo's "toy" API (a system font). Roles -> files in ui_fonts.c. */
#ifndef ZC_UI_FONTS_H
#define ZC_UI_FONTS_H

#include <cairo/cairo.h>

typedef enum {
  ZF_VALUE = 0,   /* Inter SemiBold -- a knob's value while it moves */
  ZF_COUNT
} ZcFontId;

/* loaded cairo faces (NULL until zc_fonts_load, or if the file failed). */
extern cairo_font_face_t *zc_face[ZF_COUNT];

/* Loads the TTFs from <dir> and takes a reference. Returns 1 if a reference was acquired
 * (the caller MUST pair it with exactly one zc_fonts_free), 0 only if <dir> is NULL (no
 * reference taken -> nothing to free). Per-font failures leave that zc_face[]
 * entry NULL and the panel falls back to the system font. Reference-counted across instances. */
int  zc_fonts_load(const char *dir);
void zc_fonts_free(void);

#endif /* ZC_UI_FONTS_H */
