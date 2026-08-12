# SPDX-FileCopyrightText: 2025 Geoffrey D. Bennett <g@b4.vu>
# SPDX-License-Identifier: GPL-3.0-or-later

PKG_CONFIG ?= pkg-config

TESTS = test-biquad

# The online update tests need the same libraries as the feature, so
# they are only built when it is enabled
ONLINE_UPDATE ?= 1
ifeq ($(ONLINE_UPDATE),1)
  TESTS += test-index-cache test-client-version
endif

CFLAGS = -I.. -Wall $(shell $(PKG_CONFIG) --cflags glib-2.0)
LDFLAGS = -lm $(shell $(PKG_CONFIG) --libs glib-2.0)

ONLINE_PKGS = gtk4 alsa libsoup-3.0 json-glib-1.0
ONLINE_CFLAGS = -DHAVE_ONLINE_UPDATE -DVERSION=\"test\" \
                $(shell $(PKG_CONFIG) --cflags $(ONLINE_PKGS))
ONLINE_LDFLAGS = -lcrypto $(shell $(PKG_CONFIG) --libs $(ONLINE_PKGS))

all: $(TESTS)

test-biquad: test-biquad.c ../biquad.c ../biquad.h
	$(CC) $(CFLAGS) -o $@ test-biquad.c ../biquad.c $(LDFLAGS)

test-index-cache: test-index-cache.c alsa-stubs.c ../online-update.c \
                  ../online-update.h ../scarlett4-firmware.c \
                  ../scarlett4-firmware.h
	$(CC) $(CFLAGS) $(ONLINE_CFLAGS) -o $@ \
	  test-index-cache.c alsa-stubs.c ../online-update.c \
	  ../scarlett4-firmware.c $(LDFLAGS) $(ONLINE_LDFLAGS)

test-client-version: test-client-version.c alsa-stubs.c ../online-update.c \
                     ../online-update.h ../scarlett4-firmware.c \
                     ../scarlett4-firmware.h
	$(CC) $(CFLAGS) $(ONLINE_CFLAGS) -o $@ \
	  test-client-version.c alsa-stubs.c ../online-update.c \
	  ../scarlett4-firmware.c $(LDFLAGS) $(ONLINE_LDFLAGS)

test: $(TESTS)
	@for t in $(TESTS); do echo "Running $$t..."; ./$$t || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all test clean
