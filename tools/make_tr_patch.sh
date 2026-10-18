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
mkdir -p "$W/Source/Teensy"; cp "$TR/Source/Teensy/Teensy.ino" "$TR/Source/Teensy/IOHandlers.ino" "$TR/Source/Teensy/SerUSBIO.ino" "$W/Source/Teensy/"
cd "$W"; git init -q; git add -A; git -c user.name=x -c user.email=x@x commit -qm base

python3 -I - "$C" "$HERE/firmware/REV" "$HERE/firmware/vdrive_remote.inc" <<'PY'
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
import re
base = re.search(r'#define TRVersion\s+"([^"]+)"', open(f"{C}/Common_Defs.h").read()).group(1)
rev = open(sys.argv[2]).read().strip()                      # firmware/REV: bumped by hand on firmware-affecting commits
vd_version = f"{base}.{rev}" if base.count(".") >= 2 else f"{base}.0.{rev}"   # e.g. 0.9 -> 0.9.0.1 (digits and dots only: the flash tool parses it)
edit(f"{C}/Common_Defs.h",
     f'#define TRVersion              "{base}"    //*VERSION*',
     f'#define TRVersion              "{vd_version}"    //*VERSION*  (stock {base} + VDrive rev {rev})')
edit(f"{C}/Common_Defs.h",
     "#define FWFullToken       0x64E2  // Full firmware response\n",
     "#define FWFullToken       0x64E2  // Full firmware response\n"
     "#define VDriveGetToken    0x64E3  // VDrive: read what a unit has mounted\n"
     "#define VDriveSetToken    0x64E4  // VDrive: mount an image on a unit / unmount\n")
edit("Source/Teensy/SerUSBIO.ino",
     "      case FWCheckToken: //Check firmware type\n         SendU16(FWFullToken);\n         return;   \n",
     "      case FWCheckToken: //Check firmware type\n         SendU16(FWFullToken);\n         return;   \n"
     "#ifdef Fab04_VDrive\n      case VDriveGetToken:  //VDrive disk image of a unit (works while a C64 program runs)\n      case VDriveSetToken:  //VDrive change the disk image of a unit\n"
     "         VDriveRemoteCommand(TokenVal == VDriveSetToken);\n         return;\n#endif\n")
edit("Source/Teensy/SerUSBIO.ino",
     "FLASHMEM bool ReceiveFileName(RegMenuTypes* DriveType, char *FileNamePath)\n",
     open(sys.argv[3]).read() + "\nFLASHMEM bool ReceiveFileName(RegMenuTypes* DriveType, char *FileNamePath)\n")
edit("Source/Teensy/SerUSBIO.ino",
     "   if (CurrentIOHandler != IOH_TeensyROM && !ASIDAllowed)\n",
     "   //VDrive running a C64 program: directory listings (so a client can browse for an image to mount) once the drive is quiet\n"
     "   bool VDriveListing = false;\n"
     "#ifdef Fab04_VDrive\n"
     "   VDriveListing = (CurrentIOHandler == IOH_VDrive || CurrentIOHandler == IOH_VDriveSwift) &&\n"
     "      (TokenVal == GetDirectoryToken || TokenVal == GetDirNDJSONToken);\n"
     "   if (VDriveListing)\n   {\n      char Msg[64];\n      if (VDrive_RemoteQuiet(Msg, sizeof Msg))\n      {\n         SendU16(FailToken);\n         CmdChannel->printf(\"%s\\n\", Msg);\n         return;\n      }\n   }\n"
     "#endif\n\n"
     "   if (CurrentIOHandler != IOH_TeensyROM && !ASIDAllowed && !VDriveListing)\n")
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
     "   #ifdef Fab04_VDrive\n      IOH_VDrive,\n      IOH_VDriveSwift,\n   #endif  \n")
edit(f"{C}/IOHandlers.h",
     '   #ifdef Fab04_KernalReplace\n      #include "IO_Handlers/IOH_KernalReplace.c"\n   #endif    \n',
     '   #ifdef Fab04_KernalReplace\n      #include "IO_Handlers/IOH_KernalReplace.c"\n   #endif    \n'
     '   #ifdef Fab04_VDrive\n      #include "IO_Handlers/IOH_VDrive.c"\n   #endif    \n')
edit(f"{C}/IOHandlers.h",
     "   #ifdef Fab04_KernalReplace\n      &IOHndlr_KernalReplace,      //IOH_KernalReplace,\n   #endif  \n",
     "   #ifdef Fab04_KernalReplace\n      &IOHndlr_KernalReplace,      //IOH_KernalReplace,\n   #endif  \n"
     "   #ifdef Fab04_VDrive\n      &IOHndlr_VDrive,             //IOH_VDrive,\n      &IOHndlr_VDriveSwift,        //IOH_VDriveSwift,\n   #endif  \n")
edit(f"{C}/IOHandlers.h",
     '      #include "IO_Handlers/IOH_Swiftlink.c"\n',
     '      #include "IO_Handlers/IOH_Swiftlink.c"\n'
     '   #ifdef Fab04_VDrive\n      #include "IO_Handlers/IOH_VDriveSwift.c"   //needs IOH_VDrive.c and IOH_Swiftlink.c above\n   #endif\n')
edit(f"{C}/IO_Handlers/StatusFunctions.c",
     "      NextIOHndlr = MenuSource[SelItemFullIdx].IOHndlrAssoc;\n",
     "      NextIOHndlr = MenuSource[SelItemFullIdx].IOHndlrAssoc;\n"
     "#ifdef Fab04_VDrive\n      NextIOHndlr = VDriveSwift_Remap(NextIOHndlr);   //SwiftLink programs keep the virtual drive\n#endif\n")
edit(f"{C}/IO_Handlers/StatusFunctions.c",
     "   if (NextIOHndlr == IOH_KernalReplace)\n   {\n      InitHndlr_KERNALReplace_PreStart(); //separate function so it doesn't get called twice\n",
     "   if (NextIOHndlr == IOH_KernalReplace\n#ifdef Fab04_VDrive\n       || NextIOHndlr == IOH_VDrive || NextIOHndlr == IOH_VDriveSwift\n#endif\n      )\n   {\n      InitHndlr_KERNALReplace_PreStart(); //separate function so it doesn't get called twice\n"
     "#ifdef Fab04_VDrive\n      if (NextIOHndlr == IOH_VDrive || NextIOHndlr == IOH_VDriveSwift) VDrive_PreStart(); //SD/USB probing now, while the C64 still waits\n#endif\n")
edit("Source/Teensy/IOHandlers.ino",
     "      IOHandlerInit(MenuSource[SelItemFullIdx].IOHndlrAssoc); \n",
     "#ifdef Fab04_VDrive\n      IOHandlerInit(VDriveSwift_Remap(MenuSource[SelItemFullIdx].IOHndlrAssoc));   //SwiftLink programs keep the virtual drive\n#else\n"
     "      IOHandlerInit(MenuSource[SelItemFullIdx].IOHndlrAssoc); \n#endif\n")
PY
cp "$HERE/firmware/IOH_VDrive.c" "$HERE/firmware/IOH_VDriveSwift.c" "$C/IO_Handlers/"
mkdir -p "$C/vdrive"
cp "$HERE"/d64/d64.c "$HERE"/d64/d64.h "$HERE"/d64/vdrive.c "$HERE"/d64/vdrive.h \
   "$HERE"/d64/vdrive_io.c "$HERE"/d64/vdrive_io.h "$HERE"/d64/vd_cfg.h "$C/vdrive/"
git add -A -N
git diff --no-color > "$HERE/firmware/teensyrom-vdrive.patch"
echo "wrote firmware/teensyrom-vdrive.patch ($(grep -c '^+++ ' "$HERE/firmware/teensyrom-vdrive.patch") files)"
