# Dynamo 0C EQ - build the LV2 bundle (Linux, x86-64).
#
#   make            build/dynamo-0c.lv2
#   make install    to ~/.lv2 (atomic: safe while a host has the plugin loaded)
#   make install PREFIX=/usr DESTDIR=...   to $(DESTDIR)$(PREFIX)/lib/lv2, for packaging
#   make test       the bundle exports its entry points and carries every file it loads
#   make clean
#
# Needs a C compiler and the development headers of LV2, Cairo, FreeType and X11 (plus Xcursor and Xrandr).
# Generic x86-64 with SSE3: no AVX2 or other newer instruction is required, so the bundle runs on any 64-bit Intel or
# AMD CPU. The window toolkit (pugl) is included in deps/.

PKG_CONFIG ?= pkg-config
STRIP      ?= strip
BUILD      := build
BUNDLE     := $(BUILD)/dynamo-0c.lv2
PREFIX     ?=
ifeq ($(PREFIX),)
INSTALL_DIR ?= $(DESTDIR)$(HOME)/.lv2
else
INSTALL_DIR ?= $(DESTDIR)$(PREFIX)/lib/lv2
endif

ifneq ($(shell $(PKG_CONFIG) --exists lv2 cairo x11 freetype2 && echo ok),ok)
$(error the development files of lv2, cairo, x11 or freetype2 were not found by $(PKG_CONFIG): install them first)
endif

# NO -ffast-math: it folds isfinite() to a constant, so the numerical guards could not see a failure.
# CFLAGS from the environment replace the optimisation level (a packager's own); what the plugin NEEDS is added
# anyway: SSE maths, and -msse3, without which denormal_off() loses its DAZ half (it is compiled under __SSE3__).
OPTIMIZATIONS := -fomit-frame-pointer -O3 -DNDEBUG
CFLAGS ?= $(OPTIMIZATIONS) -Wall
ifneq (,$(filter x86_64 i686 i386,$(shell uname -m)))
override CFLAGS += -msse -msse2 -msse3 -mfpmath=sse
endif
override CFLAGS += -fno-math-errno -fno-trapping-math -fPIC -fvisibility=hidden -pthread `$(PKG_CONFIG) --cflags lv2`
PUGL_SRC   := deps/pugl/src/common.c deps/pugl/src/internal.c deps/pugl/src/x11.c deps/pugl/src/x11_cairo.c
PUGLCFLAGS := -Ideps/pugl/include `$(PKG_CONFIG) --cflags cairo x11 freetype2`
PUGLLIBS   := `$(PKG_CONFIG) --libs cairo x11 freetype2` -lXcursor -lXrandr

.PHONY: all install test clean
all: $(BUNDLE)/Dynamo-0C.so $(BUNDLE)/Dynamo-0C_UI.so $(BUNDLE)/manifest.ttl $(BUNDLE)/Dynamo-0C.ttl
	@mkdir -p $(BUNDLE)/fonts $(BUNDLE)/photo
	@cp gui/fonts/*.ttf gui/fonts/OFL.txt $(BUNDLE)/fonts/
	@cp gui/photo/panel.png gui/photo/buttons.png $(BUNDLE)/photo/
	@echo "bundle ready: $(BUNDLE)"

$(BUNDLE)/Dynamo-0C.so: src/lv2.c $(wildcard src/*.h)
	@mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) -o $@ src/lv2.c -shared -lm
	$(STRIP) -s $@

$(BUNDLE)/Dynamo-0C_UI.so: src/ui.c src/ui_fonts.c $(wildcard src/*.h) $(PUGL_SRC)
	@mkdir -p $(BUNDLE)
	$(CC) $(CFLAGS) $(PUGLCFLAGS) -o $@ src/ui.c src/ui_fonts.c $(PUGL_SRC) -shared $(PUGLLIBS)
	$(STRIP) -s $@

$(BUNDLE)/manifest.ttl: lv2/manifest.ttl.in lv2/manifest.gui.ttl.in
	@mkdir -p $(BUNDLE)
	sed "s/@LV2NAME@/Dynamo-0C/;s/@LV2GUI@/Dynamo-0C_UI/;s/@LIB_EXT@/.so/" lv2/manifest.ttl.in > $@
	sed "s/@LV2NAME@/Dynamo-0C/;s/@LV2GUI@/Dynamo-0C_UI/;s/@LIB_EXT@/.so/" lv2/manifest.gui.ttl.in >> $@
$(BUNDLE)/Dynamo-0C.ttl: lv2/Dynamo-0C.ttl.in lv2/Dynamo-0C.gui.ttl.in
	@mkdir -p $(BUNDLE)
	sed 's|@ZC_UI@|ui:ui zc:ui ;|' lv2/Dynamo-0C.ttl.in > $@
	cat lv2/Dynamo-0C.gui.ttl.in >> $@

# Atomic install. A plain `cp` rewrites the .so in place and can crash a host that has it memory-mapped.
install: all
	@mkdir -p $(INSTALL_DIR)
	@rm -rf $(INSTALL_DIR)/dynamo-0c.lv2.tmp $(INSTALL_DIR)/dynamo-0c.lv2.old
	@cp -r $(BUNDLE) $(INSTALL_DIR)/dynamo-0c.lv2.tmp
	@if [ -d $(INSTALL_DIR)/dynamo-0c.lv2 ]; then mv $(INSTALL_DIR)/dynamo-0c.lv2 $(INSTALL_DIR)/dynamo-0c.lv2.old; fi
	@mv $(INSTALL_DIR)/dynamo-0c.lv2.tmp $(INSTALL_DIR)/dynamo-0c.lv2
	@rm -rf $(INSTALL_DIR)/dynamo-0c.lv2.old
	@echo "installed in $(INSTALL_DIR)/dynamo-0c.lv2"

# What a build can get wrong without an error: the entry points hidden by -fvisibility, a runtime file left out.
test: all
	@fail=0; \
	nm -D --defined-only $(BUNDLE)/Dynamo-0C.so | grep -q ' lv2_descriptor$$' || { echo "FAIL: Dynamo-0C.so does not export lv2_descriptor"; fail=1; }; \
	nm -D --defined-only $(BUNDLE)/Dynamo-0C_UI.so | grep -q ' lv2ui_descriptor$$' || { echo "FAIL: Dynamo-0C_UI.so does not export lv2ui_descriptor"; fail=1; }; \
	for f in manifest.ttl Dynamo-0C.ttl photo/panel.png photo/buttons.png fonts/OFL.txt; do [ -f $(BUNDLE)/$$f ] || { echo "FAIL: missing $$f"; fail=1; }; done; \
	[ $$fail -eq 0 ] && echo "bundle OK: entry points exported, all runtime files present"; exit $$fail

clean:
	rm -rf $(BUILD)
