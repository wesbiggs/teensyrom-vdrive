#!/bin/sh
# Build a release firmware hex: the TeensyROM commit named in firmware/UPSTREAM, firmware/teensyrom-vdrive.patch applied,
# built with TeensyROM's own tools/build-firmware.mjs.
# usage: tools/build_firmware.sh tr|tr-plus|tr-plus-dma [outdir] [--install-toolchain]
#   tr, tr-plus: without DMA LOAD, as the VD_DMA_LOAD=0 patch (docs/DESIGN.md 49): Fab04_VDriveDMALoad taken out again and the KERNAL table
#     (vd_kpatch.h) rebuilt with tools/build.py -D VD_DMA_LOAD=0 (needs ca65/ld65). TeensyROM+_<ver>_full.hex, TeensyROM_<ver>_full.hex.
#   tr-plus-dma: the committed patch as it is (DMA LOAD on), version <ver>-DMA (menu title and TeensyROM+_<ver>-DMA_full.hex).
#   outdir defaults to build/firmware; the TeensyROM tree goes to build/teensyrom-<target> (fresh every run).
#   --install-toolchain: arduino-cli (TeensyROM's installer), teensy:avr@1.61.0 and CRC32@2.0.1 (CI; skip it on a machine already set up).
# Needs git, node >= 24 (the TeensyROM build scripts) and, without --install-toolchain, arduino-cli with that core and library.
set -e
TARGET="$1"; OUT="${2:-build/firmware}"
case "$TARGET" in tr|tr-plus|tr-plus-dma) ;; *) echo "usage: $0 tr|tr-plus|tr-plus-dma [outdir] [--install-toolchain]"; exit 1 ;; esac
BOARD="${TARGET%-dma}"
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
C=Source/Teensy/MinimalBoot/Common
if [ "$TARGET" = tr-plus-dma ]; then
  python3 -I - "$C/Common_Defs.h" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s, n = re.subn(r'(#define TRVersion\s+"[^"]+)"', r'\1-DMA"', s, count=1)
assert n == 1 and "#define Fab04_VDriveDMALoad" in s, "TRVersion / Fab04_VDriveDMALoad not found"
open(p, "w").write(s)
PY
else
  # what VD_DMA_LOAD=0 tools/make_tr_patch.sh writes: no Fab04_VDriveDMALoad, the KERNAL table without the DMA request
  python3 -I - "$C/Common_Defs.h" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s, n = re.subn(r'^[ \t]*#define Fab04_VDriveDMALoad\b.*\n', '', s, flags=re.M)
assert n == 1, "Fab04_VDriveDMALoad define not found"
open(p, "w").write(s)
PY
  python3 -I "$HERE/tools/build.py" --table "$C/vdrive/vd_kpatch.h" -D VD_DMA_LOAD=0
fi
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
node tools/build-firmware.mjs --target "$BOARD" --force
set -- build/firmware/*.hex
[ $# = 1 ] && [ -f "$1" ] || { echo "expected one hex in $TR/build/firmware, found: $*"; exit 1; }
cp "$1" "$OUT/"
echo "$OUT/$(basename "$1")"
