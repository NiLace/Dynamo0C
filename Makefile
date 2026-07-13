#!/usr/bin/make -f
# Dynamo 0C EQ -- LV2 parametric equalizer (4 bands + HP/LP filters), DSP + custom UI.

PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2
PKG_CONFIG ?= pkg-config
STRIP ?= strip

MACHINE := $(shell uname -m)
ifneq (,$(filter x86_64 i686 i386,$(MACHINE)))
  OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -DNDEBUG
else
  OPTIMIZATIONS ?= -ffast-math -fomit-frame-pointer -O3 -DNDEBUG
endif
CFLAGS ?= $(OPTIMIZATIONS) -Wall

ifdef DEBUG
  OPTIMIZATIONS = -O0 -g -DDEBUG
  STRIP = true
endif

BUILDDIR = build/
LV2NAME  = Dynamo-0C
LV2GUI   = Dynamo-0C_UI
BUNDLE   = dynamo-0c.lv2
LIB_EXT  = .so

# UI: on by default (the plugin ships with its Pugl/Cairo interface). make BUILDUI=no for a headless
# DSP-only build for headless environments (no X11/cairo available).
BUILDUI ?= yes

override CFLAGS += -fPIC -fvisibility=hidden -pthread
override CFLAGS += `$(PKG_CONFIG) --cflags lv2`
LOADLIBES = -lm

ifeq ($(shell $(PKG_CONFIG) --exists lv2 || echo no), no)
  $(error LV2 SDK not found (pkg-config lv2))
endif

# --- UI (Pugl/Cairo). pugl is vendored in deps/pugl (self-contained) ---
PUGL_DIR   ?= deps/pugl
PUGL_SRC    = $(PUGL_DIR)/src/common.c $(PUGL_DIR)/src/internal.c \
              $(PUGL_DIR)/src/x11.c $(PUGL_DIR)/src/x11_cairo.c
PUGLCFLAGS  = -I$(PUGL_DIR)/include `$(PKG_CONFIG) --cflags cairo x11 freetype2`
PUGLLIBS    = `$(PKG_CONFIG) --libs cairo x11 freetype2` -lXcursor -lXrandr

ZC_UI =
targets = $(BUILDDIR)$(LV2NAME)$(LIB_EXT)
ifneq ($(BUILDUI), no)
  # note: '#' starts a comment in make -> we use the zc: prefix (defined in the ttl) instead of the IRI
  ZC_UI  = ui:ui zc:ui ;
  targets += $(BUILDDIR)$(LV2GUI)$(LIB_EXT)
endif

default: all
all: $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(targets)

# NOTE: manifest.ttl and $(LV2NAME).ttl are GENERATED and their contents depend on BUILDUI (UI stanza).
# They depend on FORCE so they ALWAYS regenerate: otherwise a `make` without BUILDUI=yes leaves them
# UI-less and make thinks them "up to date" on the next `make install BUILDUI=yes` (regression: plugin with no interface).
FORCE:

$(BUILDDIR)manifest.ttl: lv2ttl/manifest.ttl.in lv2ttl/manifest.gui.ttl.in FORCE
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" lv2ttl/manifest.ttl.in > $@
ifneq ($(BUILDUI), no)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" lv2ttl/manifest.gui.ttl.in >> $@
endif

$(BUILDDIR)$(LV2NAME).ttl: lv2ttl/$(LV2NAME).ttl.in lv2ttl/$(LV2NAME).gui.ttl.in FORCE
	@mkdir -p $(BUILDDIR)
	sed 's|@ZC_UI@|$(ZC_UI)|' lv2ttl/$(LV2NAME).ttl.in > $@
ifneq ($(BUILDUI), no)
	cat lv2ttl/$(LV2NAME).gui.ttl.in >> $@
endif

$(BUILDDIR)$(LV2NAME)$(LIB_EXT): lv2.c dsp.h coeffs.h ports.h uris.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ lv2.c -shared $(LDFLAGS) $(LOADLIBES)
	$(STRIP) -s $@

$(BUILDDIR)$(LV2GUI)$(LIB_EXT): ui.c ui_fonts.c ui_fonts.h ui_panel.h ports.h uris.h $(PUGL_SRC)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PUGLCFLAGS) -o $@ ui.c ui_fonts.c $(PUGL_SRC) \
	  -shared $(LDFLAGS) $(PUGLLIBS)
	$(STRIP) -s $@

# panel previewer to PNG (no host): make preview && ./build/ui_preview out.png 2
preview: $(BUILDDIR)ui_preview
$(BUILDDIR)ui_preview: ui_preview.c ui_fonts.c ui_fonts.h ui_panel.h ports.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) `$(PKG_CONFIG) --cflags cairo freetype2` -o $@ ui_preview.c ui_fonts.c \
	  `$(PKG_CONFIG) --libs cairo freetype2` -lm

# verification harnesses (dlopen the built .so): make test && ./build/host_test
test: $(BUILDDIR)host_test $(BUILDDIR)gain_probe
$(BUILDDIR)host_test: host_test.c ports.h uris.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ host_test.c -lm -ldl
$(BUILDDIR)gain_probe: gain_probe.c ports.h uris.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ gain_probe.c -lm -ldl

install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(targets) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)
ifneq ($(BUILDUI), no)
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)/fonts
	install -m644 fonts/*.ttf fonts/OFL.txt $(DESTDIR)$(LV2DIR)/$(BUNDLE)/fonts
endif

uninstall:
	rm -rf $(DESTDIR)$(LV2DIR)/$(BUNDLE)

clean:
	rm -rf $(BUILDDIR)

.PHONY: default all install uninstall clean preview test FORCE
