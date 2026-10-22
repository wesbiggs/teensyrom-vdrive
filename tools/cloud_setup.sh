#!/bin/sh
# Set up a fresh Linux (Debian/Ubuntu) cloud container for `make test`, VICE runs included.
# usage: tools/cloud_setup.sh            then:  tools/cloud_setup.sh --env  prints the variables to use (see CLAUDE.md "Testing in the cloud")
#
# Ubuntu's vice package is the DFSG build: it has no Commodore ROMs. VICE's upstream source tree ships them, so the C64 and drive ROM
# directories are fetched from the VICE source mirror into VICE's per-user data directory (~/.local/share/vice), where x64sc finds them.
# They never go into this repository (no Commodore ROM bytes in the repo); `make test` gets the KERNAL through STOCK=.
set -e
ROMDIR="$HOME/.local/share/vice"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SHIM="$HERE/build/cloud-bin"

if [ "$1" = "--env" ]; then
   echo "export PATH=\"$SHIM:\$PATH\""
   echo "export STOCK=\"$ROMDIR/C64/kernal-901227-03.bin\""
   exit 0
fi

need=""
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo   # cloud containers run as root, GitHub Actions runners do not
for t in x64sc c1541 petcat ca65 ld65 xvfb-run clang++; do command -v "$t" >/dev/null 2>&1 || need=1; done
if [ -n "$need" ]; then
   $SUDO apt-get install -y vice cc65 xvfb clang >/dev/null 2>&1 || { $SUDO apt-get update >/dev/null 2>&1; $SUDO apt-get install -y vice cc65 xvfb clang >/dev/null; }
fi
python3 -c "import py65" 2>/dev/null || pip install -q py65 2>/dev/null || pip install -q --break-system-packages py65

if [ ! -f "$ROMDIR/C64/kernal-901227-03.bin" ] || [ ! -f "$ROMDIR/DRIVES/dos1541ii-251968-03.bin" ]; then
   T="$(mktemp -d)"
   git clone -q --depth 1 --filter=blob:none --sparse https://github.com/VICE-Team/svn-mirror.git "$T/vice"
   (cd "$T/vice" && git sparse-checkout set vice/data/C64 vice/data/DRIVES)
   mkdir -p "$ROMDIR"
   cp -r "$T/vice/vice/data/C64" "$T/vice/vice/data/DRIVES" "$ROMDIR/"
   rm -rf "$T"
fi

# GCC's -Werror trips on existing glue code in tests/fw_host (-Wextra enum/format-truncation); the owner builds with clang (macOS)
mkdir -p "$SHIM"
ln -sf "$(command -v clang++)" "$SHIM/c++"

echo "ready: KERNAL $ROMDIR/C64/kernal-901227-03.bin"
echo "run:   eval \"\$(tools/cloud_setup.sh --env)\"; xvfb-run -a make test STOCK=\"\$STOCK\""
