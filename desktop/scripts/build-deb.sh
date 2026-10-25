#!/usr/bin/env bash
# Builds Sreon .deb packages.
#
# usage: build-deb.sh [amd64|arm64|all]       (default: this machine's architecture)
#
# Needs: gcc, make, Node 18+, npm, and an internet connection (npm downloads Electron).
# Output: desktop/dist/sreon_<version>_<arch>.deb
set -euo pipefail
cd "$(dirname "$0")/.."

# A Mac cannot build a Linux package directly. Hand over to Docker, which gives it a Linux machine.
if [ "$(uname -s)" != "Linux" ]; then
  exec bash scripts/build-deb-docker.sh "$@"
fi

# (No `case` inside $( ): the bash 3.2 that macOS ships cannot parse that.)
detect_arch() {
  case "$(uname -m)" in
    x86_64|amd64) echo amd64 ;;
    aarch64|arm64) echo arm64 ;;
    *) uname -m ;;
  esac
}
electron_arch() {
  case "$1" in
    amd64) echo x64 ;;
    arm64) echo arm64 ;;
  esac
}
HOST=$(detect_arch)
WANT=${1:-$HOST}
case "$WANT" in all) LIST="amd64 arm64";; amd64|arm64) LIST=$WANT;; *) echo "usage: $0 [amd64|arm64|all]"; exit 1;; esac

# electron-builder's own .deb packager (fpm) exists only for x86. On an arm64 machine the system one is used.
if [ "$HOST" = arm64 ]; then
  command -v fpm >/dev/null 2>&1 || {
    echo "On an arm64 machine, electron-builder needs fpm:"
    echo "    sudo apt install ruby ruby-dev build-essential && sudo gem install fpm"
    exit 1
  }
  export USE_SYSTEM_FPM=true
fi

[ -d node_modules/electron ] || npm install --no-audit --no-fund
mkdir -p dist

for ARCH in $LIST; do
  echo "=== $ARCH ==="
  bash scripts/stage.sh "$ARCH"
  # Tests run for the architecture of this machine only (a foreign binary cannot run here).
  if [ "$ARCH" = "$HOST" ]; then node --test test/run.js; fi
  EB_ARCH=$(electron_arch "$ARCH")
  npx electron-builder --linux deb "--$EB_ARCH" --publish never
done

echo
ls -1 dist/*.deb
echo "Install on a matching machine with:  sudo apt install ./dist/sreon_<version>_<arch>.deb"
