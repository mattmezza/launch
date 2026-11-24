CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/launch
PKG_CONFIG ?= pkg-config
PACKAGES = x11 xinerama xext cairo-xlib pangocairo fontconfig gio-unix-2.0

CPPFLAGS += -D_GNU_SOURCE -DLAUNCH_DATA_DIR='"$(DATADIR)"' -Ivendor/cui -Iassets \
            $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CFLAGS ?= -O2
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PACKAGES)) -lm
SOURCES = main config model usage calc view dashboard calendar system glow x11 clipboard files web cui
OBJECTS = $(addprefix build/,$(addsuffix .o,$(SOURCES)))

.PHONY: all clean check install install-dmenu

ifneq ($(filter clean,$(MAKECMDGOALS)),)
.NOTPARALLEL:
endif

all: build/launch build/dmenu

build/launch: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/dmenu: build/launch
	ln -sf launch $@

build:
	mkdir -p $@

build/%.o: src/%.c Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -MMD -MP -c $< -o $@

# Upstream amalgamation has its own compiler settings.
build/cui.o: src/cui.c Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

install: build/launch
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(DATADIR)"
	install -m 755 build/launch "$(DESTDIR)$(BINDIR)/launch"
	ln -sfnT launch "$(DESTDIR)$(BINDIR)/launch-dmenu"
	install -m 644 config.ini assets/emoji.tsv assets/UNICODE-LICENSE assets/HEROICONS-LICENSE "$(DESTDIR)$(DATADIR)/"
	install -m 644 LICENSE "$(DESTDIR)$(DATADIR)/LICENSE"
	install -m 644 vendor/cui/LICENSE "$(DESTDIR)$(DATADIR)/CUI-LICENSE"
	install -m 644 vendor/cui/CLAY-LICENSE "$(DESTDIR)$(DATADIR)/"

install-dmenu: install
	ln -sT launch "$(DESTDIR)$(BINDIR)/dmenu"

check: all
	xvfb-run -a -s '-screen 0 1280x800x24' python3 tests/x11.py

clean:
	rm -rf build
