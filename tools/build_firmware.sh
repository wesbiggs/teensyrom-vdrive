#!/bin/sh
# Build a release firmware hex: the TeensyROM commit named in firmware/UPSTREAM, firmware/teensyrom-vdrive.patch applied,
# built with TeensyROM's own tools/build-firmware.mjs.
# usage: tools/build_firmware.sh tr|tr-plus [outdir] [--install-toolchain]
#   outdir defaults to build/firmware; the TeensyROM tree goes to build/teensyrom-<target> (fresh every run).
#   --install-toolchain: arduino-cli (TeensyROM's installer), teensy:avr@1.61.0 and CRC32@2.0.1 (CI; skip it on a machine already set up).
# Needs git, node >= 24 (the TeensyROM build scripts) and, without --install-toolchain, arduino-cli with that core and library.
set -e
TARGET="$1"; OUT="${2:-build/firmware}"
case "$TARGET" in tr|tr-plus) ;; *) echo "usage: $0 tr|tr-plus [outdir] [--install-toolchain]"; exit 1 ;; esac
case "$OUT" in --*) OUT=build/firmware ;; esac
INSTALL=0; for a in "$@"; do [ "$a" = --install-toolchain ] && INSTALL=1; done
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SHA="$(tr -d ' \n' < "$HERE/firmware/UPSTREAM")"
TR="$HERE/build/teensyrom-$TARGET"
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"

rm -rf "$TR"; mkdir -p "$TR"; cd "$TR"
git init -q
git fetch -q --depth 1 https://github.com/SensoriumEmbedded/TeensyROM.git "$SHA"
git -c advice.detachedHead=false checkout -q FETCH_HEAD
git apply "$HERE/firmware/teensyrom-vdrive.patch"
# Committed so the tree is clean: the build date comes from the commit, and newer upstream build scripts mark a dirty tree's version ('*', -dirty).
git add -A
git -c user.name=teensyrom-vdrive -c user.email=noreply@invalid commit -qm "teensyrom-vdrive $(cat "$HERE/firmware/REV")"

if [ $INSTALL = 1 ]; then
  CLI="$(node tools/install-arduino-cli.mjs)"
  [ "$CLI" = arduino-cli ] || PATH="$(dirname "$CLI"):$PATH"
  URL=https://www.pjrc.com/teensy/package_teensy_index.json
  arduino-cli core update-index --additional-urls "$URL"
  arduino-cli core install teensy:avr@1.61.0 --additional-urls "$URL"
  arduino-cli lib install CRC32@2.0.1
fi

# --target tr builds without Fab04_Features (Fab04FeatureCtl.h leaves it commented out upstream).
node tools/build-firmware.mjs --target "$TARGET" --force
set -- build/firmware/*.hex
[ $# = 1 ] && [ -f "$1" ] || { echo "expected one hex in $TR/build/firmware, found: $*"; exit 1; }
cp "$1" "$OUT/"
echo "$OUT/$(basename "$1")"
