#!/bin/sh
# Generate firmware/teensyrom-vdrive.patch against a TeensyROM checkout.
# usage: tools/make_tr_patch.sh /path/to/TeensyROM
set -e
TR="$1"; [ -d "$TR/Source/Teensy/MinimalBoot/Common" ] || { echo "usage: $0 /path/to/TeensyROM"; exit 1; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
C=Source/Teensy/MinimalBoot/Common
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W/$C/IO_Handlers"
for f in Common_Defs.h Menu_Regs.h IOHandlers.h IO_Handlers/StatusFunctions.c; do cp "$TR/$C/$f" "$W/$C/$f"; done
mkdir -p "$W/Source/Teensy"; cp "$TR/Source/Teensy/Teensy.ino" "$W/Source/Teensy/Teensy.ino"
cd "$W"; git init -q; git add -A; git -c user.name=x -c user.email=x@x commit -qm base

python3 -I - "$C" <<'PY'
import sys
C = sys.argv[1]
def edit(path, old, new):
    s = open(path).read()
    assert s.count(old) == 1, (path, old)
    open(path, "w").write(s.replace(old, new))
edit(f"{C}/Common_Defs.h",
     "   #define Fab04_KernalReplace        //include Kernal Replacement support\n",
     "   #define Fab04_KernalReplace        //include Kernal Replacement support\n"
     "   #define Fab04_VDrive               //virtual IEC drive for the replacement KERNAL (needs Fab04_KernalReplace)\n")
edit(f"{C}/Common_Defs.h",
     "   eepAdNext          = 4235, // Next address to be used\n",
     "   eepAdVDriveDevs    = 4235, // (2:uint8_t x2)  VDrive KERNAL device numbers of the SD / USB units, 0 = default (VD_DEV0/1)\n"
     "   eepAdNext          = 4237, // Next address to be used\n")
edit("Source/Teensy/Teensy.ino",
     "   //future use:\n",
     "#ifdef Fab04_VDrive\n   EEPROM.write(eepAdVDriveDevs, 0);   //VDrive device numbers: 0 = defaults (8/9)\n   EEPROM.write(eepAdVDriveDevs+1, 0);\n#endif\n\n   //future use:\n")
edit(f"{C}/Menu_Regs.h",
     "   #ifdef Fab04_KernalReplace\n      IOH_KernalReplace,\n   #endif  \n",
     "   #ifdef Fab04_KernalReplace\n      IOH_KernalReplace,\n   #endif  \n"
     "   #ifdef Fab04_VDrive\n      IOH_VDrive,\n   #endif  \n")
edit(f"{C}/IOHandlers.h",
     '   #ifdef Fab04_KernalReplace\n      #include "IO_Handlers/IOH_KernalReplace.c"\n   #endif    \n',
     '   #ifdef Fab04_KernalReplace\n      #include "IO_Handlers/IOH_KernalReplace.c"\n   #endif    \n'
     '   #ifdef Fab04_VDrive\n      #include "IO_Handlers/IOH_VDrive.c"\n   #endif    \n')
edit(f"{C}/IOHandlers.h",
     "   #ifdef Fab04_KernalReplace\n      &IOHndlr_KernalReplace,      //IOH_KernalReplace,\n   #endif  \n",
     "   #ifdef Fab04_KernalReplace\n      &IOHndlr_KernalReplace,      //IOH_KernalReplace,\n   #endif  \n"
     "   #ifdef Fab04_VDrive\n      &IOHndlr_VDrive,             //IOH_VDrive,\n   #endif  \n")
edit(f"{C}/IO_Handlers/StatusFunctions.c",
     "   if (NextIOHndlr == IOH_KernalReplace)\n",
     "   if (NextIOHndlr == IOH_KernalReplace\n#ifdef Fab04_VDrive\n       || NextIOHndlr == IOH_VDrive\n#endif\n      )\n")
PY
cp "$HERE/firmware/IOH_VDrive.c" "$C/IO_Handlers/IOH_VDrive.c"
mkdir -p "$C/vdrive"
cp "$HERE"/d64/d64.c "$HERE"/d64/d64.h "$HERE"/d64/vdrive.c "$HERE"/d64/vdrive.h \
   "$HERE"/d64/vdrive_io.c "$HERE"/d64/vdrive_io.h "$HERE"/d64/vd_cfg.h "$C/vdrive/"
git add -A -N
git diff --no-color > "$HERE/firmware/teensyrom-vdrive.patch"
echo "wrote firmware/teensyrom-vdrive.patch ($(grep -c '^+++ ' "$HERE/firmware/teensyrom-vdrive.patch") files)"
