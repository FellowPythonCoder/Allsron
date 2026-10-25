#!/usr/bin/env bash
# Builds the SPRFST compiler for one architecture and gathers everything the packaged app carries:
#   stage/bin/sprfst        the compiler and virtual machine, built for the target
#   stage/sreon/            the engine, standard library, rules, guidebook, examples, splash
#
# usage: stage.sh [amd64|arm64]        (default: this machine's architecture)
#
# Building for the other architecture needs a cross compiler:
#   arm64 on an x86 machine :  sudo apt install gcc-aarch64-linux-gnu
#   amd64 on an arm64 machine:  sudo apt install gcc-x86-64-linux-gnu
# or just run this script on a machine of the target architecture.
set -euo pipefail
cd "$(dirname "$0")/../.."

# (No `case` inside $( ): the bash 3.2 that macOS ships cannot parse that.)
detect_arch() {
  case "$(uname -m)" in
    x86_64|amd64) echo amd64 ;;
    aarch64|arm64) echo arm64 ;;
    *) uname -m ;;
  esac
}
if [ "$(uname -s)" != "Linux" ]; then
  echo "error: this builds Linux programs, and this is $(uname -s)."
  echo "       On a Mac, run:  bash desktop/scripts/build-deb-docker.sh"
  exit 1
fi
HOST=$(detect_arch)
ARCH=${1:-$HOST}
case "$ARCH" in amd64|arm64) ;; *) echo "unknown architecture '$ARCH' (use amd64 or arm64)"; exit 1;; esac

# A compiler for the target. CC=... overrides.
if [ -n "${CC:-}" ]; then TARGET_CC=$CC
elif [ "$ARCH" = "$HOST" ]; then TARGET_CC=cc
elif [ "$ARCH" = arm64 ]; then TARGET_CC=aarch64-linux-gnu-gcc
else TARGET_CC=x86_64-linux-gnu-gcc; fi
command -v "$TARGET_CC" >/dev/null 2>&1 || {
  echo "error: no compiler '$TARGET_CC' for $ARCH on this $HOST machine."
  [ "$ARCH" = arm64 ] && echo "       install one with:  sudo apt install gcc-aarch64-linux-gnu"
  [ "$ARCH" = amd64 ] && echo "       install one with:  sudo apt install gcc-x86-64-linux-gnu"
  echo "       or run this on a $ARCH machine."
  exit 1
}

# A compiler that runs here is always needed to prepare the engine (embed the interface files).
make >/dev/null
build/bin/sprfst run tools/embed.spf >/dev/null

STAGE=desktop/stage
rm -rf "$STAGE"
mkdir -p "$STAGE/sreon/assets/logo" "$STAGE/bin" desktop/build

if [ "$ARCH" = "$HOST" ] && [ "$TARGET_CC" = cc ]; then
  cp build/bin/sprfst "$STAGE/bin/sprfst"
else
  # Same flags as the Makefile.
  "$TARGET_CC" -std=c11 -Icompiler/include -O2 -DNDEBUG -w -fno-strict-aliasing -D_GNU_SOURCE -pthread \
    compiler/src/*.c -o "$STAGE/bin/sprfst" -lm -pthread
fi

cp -R engine std rules guidebook examples "$STAGE/sreon/"
cp assets/splash.html "$STAGE/sreon/assets/"
cp assets/logo/Sreon-1024.png "$STAGE/sreon/assets/logo/"
cp assets/logo/Sreon-1024.png desktop/build/icon.png

# The staged copy must work on its own, away from the source tree. A foreign-architecture
# binary cannot run here, so for those the host compiler checks the same engine files.
if [ "$ARCH" = "$HOST" ]; then CHECK="$STAGE/bin/sprfst"; else CHECK=build/bin/sprfst; fi
SPRFST_HOME="$PWD/$STAGE/sreon" "$CHECK" check "$STAGE/sreon/engine/main.spf" >/dev/null

echo "staged $ARCH: $(file -b "$STAGE/bin/sprfst" | cut -d, -f1-2)  ($(du -sh "$STAGE" | cut -f1))"
