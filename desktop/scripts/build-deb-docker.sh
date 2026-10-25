#!/usr/bin/env bash
# Builds Sreon .deb packages inside a Linux container. This is the way to do it on a Mac
# (or Windows with WSL), because a .deb must be built by a Linux compiler.
#
# usage: build-deb-docker.sh [amd64|arm64|all]    (default: your machine's own architecture)
#
# Needs Docker running (Docker Desktop, Colima or OrbStack) and an internet connection.
# On an Apple-silicon Mac the arm64 build is native and fast; the amd64 build is emulated and slow.
# Output: desktop/dist/sreon_<version>_<arch>.deb
set -euo pipefail
cd "$(dirname "$0")/../.."

command -v docker >/dev/null 2>&1 || {
  echo "Docker is needed to build a Linux package on this machine, and it is not installed."
  echo "Install Docker Desktop (https://www.docker.com/products/docker-desktop/) or, with Homebrew:"
  echo "    brew install --cask docker      # then open the Docker app once"
  exit 1
}
docker info >/dev/null 2>&1 || {
  echo "Docker is installed but not running. Open the Docker app, wait until it says it is running, and try again."
  exit 1
}

native_arch() {
  case "$(uname -m)" in
    x86_64|amd64) echo amd64 ;;
    aarch64|arm64) echo arm64 ;;
    *) uname -m ;;
  esac
}
WANT=${1:-$(native_arch)}
case "$WANT" in
  all) LIST="arm64 amd64" ;;
  amd64|arm64) LIST=$WANT ;;
  *) echo "usage: $0 [amd64|arm64|all]"; exit 1 ;;
esac

mkdir -p desktop/dist

for ARCH in $LIST; do
  echo "=== building $ARCH in a Linux container ==="
  # The source is mounted read-only and copied inside the container, leaving out anything built on
  # this machine (a Mac-built compiler would be the wrong kind of program for Linux).
  docker run --rm --platform "linux/$ARCH" \
    -v "$PWD":/src:ro \
    -v "$PWD/desktop/dist":/out \
    node:20-bookworm \
    bash -c '
      set -euo pipefail
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq
      apt-get install -y -qq --no-install-recommends build-essential ruby ruby-dev file libarchive-tools xz-utils >/dev/null
      gem install --no-document fpm >/dev/null
      mkdir /work
      tar -C /src --exclude=./build --exclude=./.git --exclude=./desktop/node_modules --exclude=./desktop/stage --exclude=./desktop/dist -cf - . | tar -C /work -xf -
      cd /work
      bash desktop/scripts/build-deb.sh '"$ARCH"'
      cp desktop/dist/*.deb /out/
    '
done

echo
echo "Done. Your package(s):"
ls -1 desktop/dist/*.deb
