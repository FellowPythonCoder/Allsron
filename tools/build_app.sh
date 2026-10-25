#!/bin/bash
# Builds Sreon.app (universal arm64 + x86_64) from this folder.
#   tools/build_app.sh                 build/Sreon.app, ad-hoc signed
#   tools/build_app.sh --install       also copy it to /Applications
#   tools/build_app.sh --dmg           also make build/Sreon.dmg
#   tools/build_app.sh --native        only this Mac's architecture (faster)
# Signing for distribution: export SREON_SIGN_ID="Developer ID Application: Your Name (TEAMID)"
# Notarising:               export SREON_NOTARY_PROFILE=<notarytool keychain profile>
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = "Darwin" ] || { echo "Sreon builds on a Mac (it needs Xcode's command line tools)."; exit 1; }
command -v swiftc >/dev/null || { echo "Install the command line tools first:  xcode-select --install"; exit 1; }

INSTALL=0; DMG=0; NATIVE=0
for a in "$@"; do case "$a" in --install) INSTALL=1;; --dmg) DMG=1;; --native) NATIVE=1;; esac; done
VERSION=$(sed -n 's/^const VERSION = "\(.*\)"/\1/p' engine/kit.spf)
BUILD_NO=$(date +%Y%m%d%H%M)
HOST=$(uname -m)
if [ "$NATIVE" = 1 ]; then ARCHS="$HOST"; else ARCHS="arm64 x86_64"; fi
OUT=build; APP=$OUT/Sreon.app
rm -rf "$APP"; mkdir -p "$OUT/obj" "$APP/Contents/MacOS" "$APP/Contents/Resources/bin"

echo "==> SPRFST interpreter ($ARCHS)"
for arch in $ARCHS; do
  cc -arch "$arch" -mmacosx-version-min=12.0 -std=c11 -Icompiler/include -O2 -DNDEBUG -w -fno-strict-aliasing \
     -DSPRFST_MACOS=1 compiler/src/*.c -o "$OUT/obj/sprfst-$arch" -lm -framework CoreFoundation
done
if [ "$(echo $ARCHS | wc -w)" -gt 1 ]; then lipo -create $OUT/obj/sprfst-* -output "$APP/Contents/Resources/bin/sprfst"
else cp "$OUT/obj/sprfst-$ARCHS" "$APP/Contents/Resources/bin/sprfst"; fi

echo "==> Refresh embedded UI assets"
"$APP/Contents/Resources/bin/sprfst" run tools/embed.spf >/dev/null

echo "==> Engine self-check"
SPRFST_HOME="$PWD" "$APP/Contents/Resources/bin/sprfst" check engine/main.spf >/dev/null

echo "==> Native shell"
for arch in $ARCHS; do
  swiftc -O -target "$arch-apple-macos12.0" shell/Sources/*.swift -o "$OUT/obj/Sreon-$arch" \
     -framework Cocoa -framework WebKit -framework UserNotifications -framework CoreServices
done
if [ "$(echo $ARCHS | wc -w)" -gt 1 ]; then lipo -create $OUT/obj/Sreon-* -output "$APP/Contents/MacOS/Sreon"
else cp "$OUT/obj/Sreon-$ARCHS" "$APP/Contents/MacOS/Sreon"; fi

echo "==> Bundle"
R="$APP/Contents/Resources"
cp -R engine std rules guidebook examples "$R/"
rm -f "$R/engine/_t.spf"
cp assets/logo/Sreon.icns "$R/Sreon.icns"
cp assets/splash.html "$R/splash.html"
sed -e "s/@VERSION@/$VERSION/" -e "s/@BUILD@/$BUILD_NO/" packaging/Info.plist > "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"

echo "==> Sign"
if [ -n "${SREON_SIGN_ID:-}" ]; then
  codesign --force --options runtime --timestamp --sign "$SREON_SIGN_ID" "$R/bin/sprfst"
  codesign --force --options runtime --timestamp --sign "$SREON_SIGN_ID" "$APP"
else
  codesign --force --sign - "$R/bin/sprfst"
  codesign --force --deep --sign - "$APP"
fi

if [ "$DMG" = 1 ]; then
  echo "==> Disk image"
  STAGE=$OUT/dmg; rm -rf "$STAGE" "$OUT/Sreon.dmg"; mkdir -p "$STAGE"
  cp -R "$APP" "$STAGE/"; ln -s /Applications "$STAGE/Applications"
  hdiutil create -volname "Sreon" -srcfolder "$STAGE" -ov -format UDZO "$OUT/Sreon-$VERSION.dmg" >/dev/null
  if [ -n "${SREON_SIGN_ID:-}" ]; then codesign --force --sign "$SREON_SIGN_ID" "$OUT/Sreon-$VERSION.dmg"; fi
  if [ -n "${SREON_NOTARY_PROFILE:-}" ]; then
    xcrun notarytool submit "$OUT/Sreon-$VERSION.dmg" --keychain-profile "$SREON_NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT/Sreon-$VERSION.dmg"
  fi
  echo "    $OUT/Sreon-$VERSION.dmg"
fi
if [ "$INSTALL" = 1 ]; then rm -rf /Applications/Sreon.app; cp -R "$APP" /Applications/; echo "    installed to /Applications/Sreon.app"; fi
echo "Done: $APP"
