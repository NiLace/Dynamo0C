#!/usr/bin/make -f
# Dynamo 0C EQ -- LV2 parametric equalizer (4 bands + HP/LP filters), DSP + custom UI.

PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2
PKG_CONFIG ?= pkg-config
STRIP ?= strip

# NO -ffast-math. It looks free and is not, for three measured reasons:
#   1. It implies -ffinite-math-only, under which isfinite() folds to a constant 1. Measured:
#      isfinite(NaN) and isfinite(Inf) both returned 1. Every numerical guard in matched.h would
#      be born dead -- they cannot report a failure they are not allowed to see.
#   2. It lets the compiler reassociate freely, so a refactor that changes nothing semantically
#      changes the output in its last bits. Demonstrated here: designing one channel and copying
#      to the other (mathematically identical) nulls at -240 dB with it and BIT-IDENTICALLY
#      without it. That makes the null test, our only real instrument, non-deterministic.
#   3. In a shared object it does not even buy FTZ/DAZ: crtfastmath.o is only linked into
#      executables.
# -msse3 is what actually enables the DAZ half of denormal_off(): without it __SSE3__ is
# undefined and that half is not compiled at all (measured, it was silently absent).
# The price is honest and was accepted with the number in hand: +11.3 % CPU, 21 of 21 paired
# runs, p < 0.0001 -- which in absolute terms is 0.77 % -> 0.86 % of one core.
MACHINE := $(shell uname -m)
ifneq (,$(filter x86_64 i686 i386,$(MACHINE)))
  OPTIMIZATIONS ?= -msse -msse2 -msse3 -mfpmath=sse -fno-math-errno -fno-trapping-math \
                   -fomit-frame-pointer -O3 -DNDEBUG
else
  OPTIMIZATIONS ?= -fno-math-errno -fno-trapping-math -fomit-frame-pointer -O3 -DNDEBUG
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
# DSP-only build (e.g. automated null/oracle testing where no X11/cairo is available).
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

# `test` is the gate for this repo, so it has to RUN the checks and not merely build them. A
# target that lists prerequisites without a recipe prints "Nothing to be done" and exits 0, which
# is a green light that never executed anything.
test: all $(BUILDDIR)host_test valpoly drvconst equiv firfast knob rrconv drive-thd stability touch
	@echo "=== host_test: the wrapper contract ==="
	$(BUILDDIR)host_test $(BUILDDIR)$(LV2NAME)$(LIB_EXT)
	@echo
	@echo "=== ALL GATES GREEN ==="

# Bundle validation through lilv, the same parser hosts use. lv2lint and sord_validate are not
# available everywhere, and lv2_validate wraps sord_validate and exits 0 when it cannot find it,
# so neither can be relied on. This checks the INSTALLED bundle rather than build/, because that
# is the artefact a host loads.
# It does not cover what lv2lint would: port naming conventions, unit coherence, missing
# rdfs:comment.
# Derived from uris.h so there is only one copy: hardcoding it here would let the check keep
# passing against the OLD plugin after a URI change, which is a gate that cannot go red.
ZC_URI := $(shell grep -oE '"https?://[^"]+"' uris.h | head -1 | tr -d '"')
LV2CHECK_DIR ?= $(HOME)/.lv2/dynamo-0c.lv2
lv2check:
	@test -d "$(LV2CHECK_DIR)" || { echo "no bundle at $(LV2CHECK_DIR) -- make install first"; exit 1; }
	@lv2ls 2>/dev/null | grep -qxF "$(ZC_URI)" || { echo "FAIL: lilv does not list $(ZC_URI) -- the TTL did not parse"; exit 1; }
	@echo "[lv2ls]  lilv parses the bundle and lists the URI  OK"
	@ttl=`lv2info "$(ZC_URI)" 2>/dev/null | grep -cE "^	Port [0-9]+:"`; \
	 code=`grep -oE 'ZC_N_PORTS[^0-9]*[0-9]+' ports.h | grep -oE '[0-9]+$$'`; \
	 echo "[ports]  TTL=$$ttl  ports.h=$$code"; \
	 test "$$ttl" = "$$code" || { echo "FAIL: the TTL and the code disagree on the port count"; exit 1; }
	@ttl=`grep -A2 'lv2:symbol "drive_os"' lv2ttl/$(LV2NAME).ttl.in | grep -oE 'lv2:default [0-9]+' | grep -oE '[0-9]+$$'`; \
	 code=`grep -oE 'ZC_DRIVE_OS_DEFAULT[^0-9]*[0-9]+' ports.h | grep -oE '[0-9]+$$'`; \
	 echo "[os def] TTL=$$ttl  ports.h=$$code"; \
	 test -n "$$ttl" -a "$$ttl" = "$$code" || { echo "FAIL: the TTL and the code disagree on the drive oversampling default"; exit 1; }
	@lv2info "$(ZC_URI)" 2>/dev/null | grep -q "nilace@nylarea.com" \
	  || { echo "FAIL: the author email is not nilace@nylarea.com -- identity gate"; exit 1; }
	@echo "[author] nilace@nylarea.com  OK"
	@echo "LV2 BUNDLE OK ✓"

# gain_probe is INFORMATIONAL -- it always returns 0 -- so it is not part of the gate.
probe: all $(BUILDDIR)gain_probe
	$(BUILDDIR)gain_probe $(BUILDDIR)$(LV2NAME)$(LIB_EXT)

# A GATE, not a harness: it exits non-zero if any design over the whole control range,
# at any supported sample rate, comes out non-finite, unstable, degree-overflowing or with a
# short numerator. It is built WITHOUT -ffast-math on purpose: with -ffinite-math-only the
# compiler folds isfinite() to a constant 1 and the sweep goes blind to its first check while
# still reporting a clean pass, which is worse than not having it.
# A finer step finds more, at 3.3 ms per design: 0.1 takes seconds, 0.02 takes minutes.
STABILITY_STEP ?= 0.1
drive-thd: compare/drive_thd.c ports.h uris.h
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -I. -o $(BUILDDIR)drive_thd compare/drive_thd.c -lm -ldl
	$(BUILDDIR)drive_thd $(BUILDDIR)$(LV2NAME)$(LIB_EXT)

equiv: compare/equiv_rebuild.c dsp.h matched.h coeffs.h ports.h
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -msse -msse2 -msse3 -mfpmath=sse -fno-math-errno -I. -o $(BUILDDIR)equiv_rebuild compare/equiv_rebuild.c -lm
	$(BUILDDIR)equiv_rebuild

# The drive shaper constants are defined ONCE, in dsp.h. This pins that: the values the
# compiler sees must equal the ones compare/drive_consts.py parses out of the source, and the
# derived pair (TB, FP0) must still follow from the free three (S, K, B). A stale copy in a
# harness is invisible to a null test, because the reference would carry the same error.
# Both negative arms run: --mutate breaks the parser comparison, --mutate-derived breaks the
# relation. A check that cannot go red establishes nothing.
# The technical report as a PDF, typeset from the same HTML that is read on screen, so the
# two cannot describe different things. Headless Chromium is the renderer; the page carries
# its own @page and @media print rules, so nothing about the layout lives here.
# The PDF is NOT versioned: it is regenerable from a file that is.
PAPER_HTML = docs/technical-report.html
PAPER_PDF  = $(BUILDDIR)dynamo0c-technical-report.pdf
paper: $(PAPER_HTML)
	@mkdir -p $(BUILDDIR)
	@command -v chromium >/dev/null || { echo "chromium not found -- needed to typeset the PDF"; exit 1; }
	chromium --headless --disable-gpu --no-sandbox --force-color-profile=srgb \
	  --no-pdf-header-footer --virtual-time-budget=20000 \
	  --print-to-pdf=$(PAPER_PDF) "file://$(CURDIR)/$(PAPER_HTML)" 2>/dev/null
	@test -s $(PAPER_PDF) || { echo "FAIL: no PDF was produced"; exit 1; }
	@echo "written $(PAPER_PDF)"

drvconst: compare/drive_consts.c compare/drive_consts.py dsp.h
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -I. -o $(BUILDDIR)drive_consts compare/drive_consts.c -lm
	@echo "=== drvconst: one definition of the drive constants, and the derived pair follows ==="
	@$(BUILDDIR)drive_consts | grep '^\#'
	@$(BUILDDIR)drive_consts > $(BUILDDIR)dc_cc.txt
	@$(BUILDDIR)drive_consts --mutate | grep -v '^\#' > $(BUILDDIR)dc_mut.txt
	@grep -v '^\#' $(BUILDDIR)dc_cc.txt > $(BUILDDIR)dc_c.txt
	@python3 compare/drive_consts.py > $(BUILDDIR)dc_py.txt
	@cmp -s $(BUILDDIR)dc_c.txt $(BUILDDIR)dc_py.txt \
	  && echo "[values] dsp.h via the compiler == dsp.h via the parser  OK" \
	  || { echo "FAIL: the compiler and the parser disagree on the drive constants"; exit 1; }
	@echo "--- negative legs (both must be caught):"
	@if cmp -s $(BUILDDIR)dc_mut.txt $(BUILDDIR)dc_py.txt; \
	  then echo "FAIL: a perturbed constant went unnoticed"; exit 1; \
	  else echo "[--mutate] a perturbed constant is caught  OK"; fi
	@if $(BUILDDIR)drive_consts --mutate-derived >/dev/null 2>&1; \
	  then echo "FAIL: a broken derived relation went unnoticed"; exit 1; \
	  else echo "[--mutate-derived] a broken relation is caught  OK"; fi

valpoly: compare/valpoly.c
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -I. -o $(BUILDDIR)valpoly compare/valpoly.c -lm
	$(BUILDDIR)valpoly

# The fast oversampler (polyphase + SIMD) against the REFERENCE one (zeros + folded convolution).
# They are not bit-identical by design -- same terms, different order of additions -- so what is
# pinned is that the difference stays at rounding level (-240 dB of budget over a -319 dB result).
# The --mutate leg nudges one coefficient by 1e-9 and MUST fail: a gate that cannot go red is
# decoration. Both legs run, so the gate is checked every time it is used.
# The drive knob must not crunch WHILE it is turned. Every other gate here measures with the
# controls parked, so none of them could see the defect this caught: the ADAA's stored
# antiderivative kept the previous gain's scaling and the difference quotient divides by dx.
# --mutate reproduces the unfixed behaviour and MUST fail.
# The round-robin refresh spreads a redesign over several blocks. That is only safe if it
# CONVERGES: an instance walked from A to B must end up bit-identical to one that started at B.
# A cursor bug or a forgotten field would leave a band on stale coefficients FOREVER, and no null
# test would see it -- they all render from a fresh instance. This gate caught exactly that.
# ui:touch is what tells the host a gesture STARTED, and without it automation in touch mode and
# MIDI-learn misbehave. Declaring the feature in the TTL proves nothing, so this gate is a fake host
# that offers ui:touch, synthesises a real drag on knob 0, and checks the sequence.
# Needs a DISPLAY; without one it skips and says so. --mutate ignores the callbacks and must fail.
touch: compare/touch_gate.c ui.c ui_panel.h ports.h
	@mkdir -p $(BUILDDIR)
ifneq ($(BUILDUI), no)
	$(CC) -O2 -I. -o $(BUILDDIR)touch_gate compare/touch_gate.c -ldl -lX11
	@echo "=== touch: the UI emits ui:touch around a real drag ==="
	$(BUILDDIR)touch_gate $(BUILDDIR)$(LV2NAME)_UI$(LIB_EXT)
	@echo "--- negative leg (ignoring the callbacks must be caught):"
	$(BUILDDIR)touch_gate $(BUILDDIR)$(LV2NAME)_UI$(LIB_EXT) --mutate
else
	@echo "=== touch: SKIPPED, this build has no UI (BUILDUI=no) ==="
endif

rrconv: compare/rr_converge.c dsp.h coeffs.h $(BUILDDIR)$(LV2NAME)$(LIB_EXT)
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -o $(BUILDDIR)rr_converge compare/rr_converge.c -ldl -lm
	@echo "=== rrconv: a spread-out redesign converges to the one-shot design ==="
	$(BUILDDIR)rr_converge $(BUILDDIR)$(LV2NAME)$(LIB_EXT) 64
	$(BUILDDIR)rr_converge $(BUILDDIR)$(LV2NAME)$(LIB_EXT) 512
	@echo "--- and again with the coefficient grid up (waits ~3 s for the fill):"
	@ZC_WAIT_GRID=1 $(BUILDDIR)rr_converge $(BUILDDIR)$(LV2NAME)$(LIB_EXT) 64

knob: compare/knob_gate.c dsp.h coeffs.h
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -I. -o $(BUILDDIR)knob_gate compare/knob_gate.c -lm
	@echo "=== knob: no crunch while the drive knob moves ==="
	$(BUILDDIR)knob_gate
	@echo "--- negative leg (the old behaviour must be caught):"
	$(BUILDDIR)knob_gate --mutate

firfast: compare/fir_fast_check.c dsp.h coeffs.h
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -I. -o $(BUILDDIR)fir_fast_check compare/fir_fast_check.c -lm
	@echo "=== firfast: the SIMD polyphase path equals the reference one ==="
	$(BUILDDIR)fir_fast_check
	@echo "--- negative leg (a mutated coefficient must be caught):"
	$(BUILDDIR)fir_fast_check --mutate

stability: compare/design_fuzz.c dsp.h matched.h coeffs.h
	@mkdir -p $(BUILDDIR)
	$(CC) -O2 -fno-math-errno -fno-trapping-math -I. -o $(BUILDDIR)design_fuzz compare/design_fuzz.c -lm
	$(BUILDDIR)design_fuzz $(STABILITY_STEP)
$(BUILDDIR)host_test: host_test.c ports.h uris.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ host_test.c -lm -ldl
$(BUILDDIR)gain_probe: gain_probe.c ports.h uris.h
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ gain_probe.c -lm -ldl

# ATOMIC INSTALL -- every file goes to a temp name and is then mv'd (rename(2)) into place.
# Do NOT go back to installing straight over the destination. MEASURED on this very path:
# both `cp` AND GNU `install` keep the SAME INODE, i.e. they truncate and rewrite the file in
# place. If a host (Ardour) has that .so mmap'd, its not-yet-faulted pages then come from the NEW
# file -> it executes garbage and dies with SIGSEGV/SIGBUS. It crashed Ardour on every install.
# rename(2) instead gives the new file a NEW inode and leaves the old one alive for whoever still
# has it mapped, so a running host keeps working and picks the new code up when it next dlopens.
# (A /tmp probe wrongly suggested `install` was safe -- tmpfs behaved differently. Measure on the
#  real path, never on a proxy.)
D = $(DESTDIR)$(LV2DIR)/$(BUNDLE)
install: all
	install -d $(D)
	@for f in $(targets); do b=`basename $$f`; \
	  install -m755 $$f $(D)/.$$b.new && mv -f $(D)/.$$b.new $(D)/$$b; done
	@for f in $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl; do b=`basename $$f`; \
	  install -m644 $$f $(D)/.$$b.new && mv -f $(D)/.$$b.new $(D)/$$b; done
ifneq ($(BUILDUI), no)
	install -d $(D)/fonts
	@for f in fonts/*.ttf fonts/OFL.txt; do b=`basename $$f`; \
	  install -m644 $$f $(D)/fonts/.$$b.new && mv -f $(D)/fonts/.$$b.new $(D)/fonts/$$b; done
endif
	@echo "installed atomically to $(D)"
# The bundle is validated right after being written. lv2check reads the INSTALLED bundle, which is
# the artefact a host loads, so this is the only place it can run.
	@$(MAKE) --no-print-directory lv2check LV2DIR=$(LV2DIR)

uninstall:
	rm -rf $(DESTDIR)$(LV2DIR)/$(BUNDLE)

clean:
	rm -rf $(BUILDDIR)

.PHONY: default all install uninstall clean preview test stability drvconst paper FORCE \
        equiv valpoly drive-thd probe lv2check
