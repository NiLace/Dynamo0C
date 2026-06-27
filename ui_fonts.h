/* ui_fonts.h -- loading of bundled fonts (FreeType -> cairo-ft) for the Dynamo 0C EQ panel.
 * The cairo faces are exposed via zc_face[]; if they did not load (NULL), ui_panel.h falls back to
 * Cairo's "toy" API (system font). Roles -> files in ui_fonts.c. */
#ifndef ZC_UI_FONTS_H
#define ZC_UI_FONTS_H

#include <cairo/cairo.h>

typedef enum {
  ZF_BRAND = 0,   /* Oswald SemiBold  -- brand "DYNAMO 0C" */
  ZF_HEAD,        /* Oswald Medium    -- band headers (HIGH/HI·MID/...) */
  ZF_LABEL,       /* Barlow Cond SB   -- knob labels, switches, subtitle, footer */
  ZF_TICK,        /* Barlow Cond Med  -- scale numbers */
  ZF_VALUE,       /* Inter SemiBold   -- numeric read-out */
  ZF_COUNT
} ZcFontId;

/* loaded cairo faces (NULL until zc_fonts_load, or if the file failed). */
extern cairo_font_face_t *zc_face[ZF_COUNT];

/* Loads the TTFs from <dir> and takes a reference. Returns 1 if a reference was acquired
 * (the caller MUST pair it with exactly one zc_fonts_free), 0 only if FreeType could not
 * initialize (no reference taken -> nothing to free). Per-font failures leave that zc_face[]
 * entry NULL and the panel falls back to the system font. Reference-counted across instances. */
int  zc_fonts_load(const char *dir);
void zc_fonts_free(void);

#endif /* ZC_UI_FONTS_H */
