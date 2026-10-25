# Sreon — build system.   make app    builds build/Sreon.app (macOS)
#                          make test   runs the engine tests (any OS)
#                          make dmg    builds build/Sreon.dmg
#                          make deb    builds a Linux .deb for this machine (needs internet once)
#                          make deb-arm64 | deb-amd64 | deb-all   the other architectures (see desktop/README.md)
#                          make desktop-test   tests the Linux/Windows shell logic (needs Node 18+)
CC      ?= cc
UNAME_S := $(shell uname -s)
SRCS    := $(wildcard compiler/src/*.c)
CFLAGS  := -std=c11 -Icompiler/include -O2 -DNDEBUG -w -fno-strict-aliasing
LDFLAGS := -lm
ifeq ($(UNAME_S),Darwin)
  CFLAGS  += -DSPRFST_MACOS=1
  LDFLAGS += -framework CoreFoundation
else
  CFLAGS  += -D_GNU_SOURCE -pthread
  LDFLAGS += -pthread
endif

.PHONY: all app dmg deb deb-arm64 deb-amd64 deb-all desktop-test test embed clean icons
all: build/bin/sprfst

build/bin/sprfst: $(SRCS) $(wildcard compiler/include/sprfst/*.h)
	@mkdir -p build/bin
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)

embed: build/bin/sprfst
	build/bin/sprfst run tools/embed.spf

test: build/bin/sprfst
	tools/test.sh build/bin/sprfst

app:
	tools/build_app.sh

dmg:
	tools/build_app.sh --dmg

deb:
	bash desktop/scripts/build-deb.sh

deb-arm64:
	bash desktop/scripts/build-deb.sh arm64

deb-amd64:
	bash desktop/scripts/build-deb.sh amd64

deb-all:
	bash desktop/scripts/build-deb.sh all

desktop-test: build/bin/sprfst
	cd desktop && node --test test/run.js && node test/electron-smoke.js

icons:
	python3 tools/make_icons.py

clean:
	rm -rf build
