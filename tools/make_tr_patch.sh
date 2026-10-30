#!/bin/sh
# Generate firmware/teensyrom-vdrive.patch against a TeensyROM checkout.
# usage: tools/make_tr_patch.sh /path/to/TeensyROM
set -e
TR="$1"; [ -d "$TR/Source/Teensy/MinimalBoot/Common" ] || { echo "usage: $0 /path/to/TeensyROM"; exit 1; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
C=Source/Teensy/MinimalBoot/Common
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
mkdir -p "$W/$C/IO_Handlers"
for f in Common_Defs.h Menu_Regs.h IOHandlers.h ISRs.c IO_Handlers/StatusFunctions.c IO_Handlers/IOH_KernalReplace.c IO_Handlers/Swift_ATcommands.c IO_Handlers/IOH_Swiftlink.c; do cp "$TR/$C/$f" "$W/$C/$f"; done
mkdir -p "$W/Source/Teensy"; cp "$TR/Source/Teensy/Teensy.ino" "$TR/Source/Teensy/IOHandlers.ino" "$TR/Source/Teensy/SerUSBIO.ino" "$TR/Source/Teensy/MainMenuItems.h" "$W/Source/Teensy/"
M=Source/C64/MainMenuCRT/source; mkdir -p "$W/$M" "$W/Source/Teensy/TRMenuFiles/ROMs"
cp "$TR/$M/MainMenu.asm" "$W/$M/"; cp "$TR/Source/Teensy/TRMenuFiles/ROMs/TeensyROMC64.h" "$W/Source/Teensy/TRMenuFiles/ROMs/"
cd "$W"; git init -q; git add -A; git -c user.name=x -c user.email=x@x commit -qm base
# fixes to upstream Kernal Replace (RAM under the KERNAL, fast image copy, late-ISR counters): applied before the edits below
git apply "$HERE/firmware/kernalreplace-fixes.patch"
# fix to upstream SwiftLink: Tx bytes coalesced, never a blocking client.write() (it hung loop() with the stack polled from there)
git apply "$HERE/firmware/swiftlink-tx-fix.patch"

python3 -I - "$C" "$HERE/firmware/REV" "$HERE/firmware/vdrive_remote.inc" "$HERE/firmware/vdrive_probe.asm" <<'PY'
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
# Inserted after the ASIDAllowed statement and not by rewriting the "Busy!" test below it: upstream PRs add lines inside
# that test (the RTC command's refusal), and a hunk that rewrites its condition would not apply on top of them.
edit("Source/Teensy/SerUSBIO.ino",
     "       TokenVal == CopyFileToken     || TokenVal == DeleteFileToken);\n",
     "       TokenVal == CopyFileToken     || TokenVal == DeleteFileToken);\n"
     "#ifdef Fab04_VDrive\n"
     "   //VDrive running a C64 program: directory listings (so a client can browse for an image to mount) once the drive is quiet;\n"
     "   //  ASIDAllowed is only read by the Busy! test below, so it doubles as \"allowed while a program runs\"\n"
     "   if (VD_IsHandler(CurrentIOHandler) && (TokenVal == GetDirectoryToken || TokenVal == GetDirNDJSONToken))\n   {\n"
     "      char Msg[64];\n      if (VDrive_RemoteQuiet(Msg, sizeof Msg))\n      {\n         SendU16(FailToken);\n         CmdChannel->printf(\"%s\\n\", Msg);\n         return;\n      }\n"
     "      ASIDAllowed = true;\n   }\n"
     "#endif\n")
edit(f"{C}/Common_Defs.h",
     "   eepAdNext          = 4235, // Next address to be used\n",
     "   eepAdVDriveDevs    = 4235, // (2:uint8_t x2)  VDrive KERNAL device numbers of the SD / USB units, 0 = default (VD_DEV0/1)\n"
     "   eepAdNext          = 4237, // Next address to be used\n")
edit("Source/Teensy/Teensy.ino",
     "   //future use:\n",
     "#ifdef Fab04_VDrive\n   EEPROM.write(eepAdVDriveDevs, 0);   //VDrive device numbers: 0 = defaults (8/9)\n   EEPROM.write(eepAdVDriveDevs+1, 0);\n#endif\n\n   //future use:\n")
# The enum and table entries go *before* the Kernal Replace entries, not after: the lines after them (IOH_Debug and the
# "last manually selectable" comment) are what upstream's open GeoRAM PRs rewrite, and a hunk with those lines as context
# would not apply on top of them. (Include order is separate and stays after IOH_KernalReplace.c, which IOH_VDrive.c needs.)
edit(f"{C}/Menu_Regs.h",
     "   #ifdef Fab04_KernalReplace\n      IOH_KernalReplace,\n   #endif  \n",
     "   #ifdef Fab04_VDrive\n      IOH_VDrive,\n      IOH_VDriveSwift,\n   #ifdef Fab04_REU\n      IOH_VDriveSwiftREU,\n      IOH_VDriveREU,\n   #endif\n   #endif  \n"
     "   #ifdef Fab04_KernalReplace\n      IOH_KernalReplace,\n   #endif  \n")
edit(f"{C}/IOHandlers.h",
     '   #ifdef Fab04_KernalReplace\n      #include "IO_Handlers/IOH_KernalReplace.c"\n   #endif    \n',
     '   #ifdef Fab04_KernalReplace\n      #include "IO_Handlers/IOH_KernalReplace.c"\n   #endif    \n'
     '   #ifdef Fab04_VDrive\n      #include "IO_Handlers/IOH_VDrive.c"\n   #endif    \n')
edit(f"{C}/IOHandlers.h",
     "   #ifdef Fab04_KernalReplace\n      &IOHndlr_KernalReplace,      //IOH_KernalReplace,\n   #endif  \n",
     "   #ifdef Fab04_VDrive\n      &IOHndlr_VDrive,             //IOH_VDrive,\n      &IOHndlr_VDriveSwift,        //IOH_VDriveSwift,\n   #ifdef Fab04_REU\n      &IOHndlr_VDriveSwiftREU,     //IOH_VDriveSwiftREU,\n      &IOHndlr_VDriveREU,          //IOH_VDriveREU,\n   #endif\n   #endif  \n"
     "   #ifdef Fab04_KernalReplace\n      &IOHndlr_KernalReplace,      //IOH_KernalReplace,\n   #endif  \n")
edit(f"{C}/IOHandlers.h",
     '      #include "IO_Handlers/IOH_Swiftlink.c"\n',
     '      #include "IO_Handlers/IOH_Swiftlink.c"\n'
     '   #ifdef Fab04_VDrive\n      #include "IO_Handlers/IOH_VDriveSwift.c"   //needs IOH_VDrive.c and IOH_Swiftlink.c above\n'
     '   #ifdef Fab04_REU\n      #include "IO_Handlers/IOH_VDriveREU.c"        //needs IOH_VDrive.c and IOH_REU.c above\n      #include "IO_Handlers/IOH_VDriveSwiftREU.c"   //needs IOH_VDriveSwift.c and IOH_VDriveREU.c above\n   #endif\n   #endif\n')
edit(f"{C}/IO_Handlers/StatusFunctions.c",
     "      NextIOHndlr = MenuSource[SelItemFullIdx].IOHndlrAssoc;\n",
     "      NextIOHndlr = MenuSource[SelItemFullIdx].IOHndlrAssoc;\n"
     "#ifdef Fab04_VDrive\n      NextIOHndlr = VDriveSwift_Remap(NextIOHndlr);   //SwiftLink programs keep the virtual drive\n#endif\n")
edit(f"{C}/IO_Handlers/StatusFunctions.c",
     "   if (NextIOHndlr == IOH_KernalReplace)\n   {\n      InitHndlr_KERNALReplace_PreStart(); //separate function so it doesn't get called twice\n",
     "   if (NextIOHndlr == IOH_KernalReplace\n#ifdef Fab04_VDrive\n       || VD_IsHandler(NextIOHndlr)\n#endif\n      )\n   {\n      InitHndlr_KERNALReplace_PreStart(); //separate function so it doesn't get called twice\n"
     "#ifdef Fab04_VDrive\n      if (VD_IsHandler(NextIOHndlr)) VDrive_PreStart(); //SD/USB probing now, while the C64 still waits\n#endif\n")
# The launch-time bus probe (docs/DESIGN.md 37). KERNALPreStart() first lets VDrive_ProbeStep() answer the menu's probe steps;
# the menu clears rwRegScratch before PreStart (a TeensyROM without VDrive never sets it, so its menu skips the probe) and runs
# VDriveProbe after it, with the C64's own KERNAL still in place.
edit(f"{C}/IO_Handlers/StatusFunctions.c",
     "   //called before BASIC init and program load/launch\n#ifdef Fab04_KernalReplace\n",
     "   //called before BASIC init and program load/launch\n"
     "#ifdef Fab04_VDrive\n   if (VDrive_ProbeStep()) return; //a step of the menu's bus probe after a VDrive PreStart (MainMenu.asm, VDriveProbe)\n#endif\n"
     "#ifdef Fab04_KernalReplace\n")
edit("Source/C64/MainMenuCRT/source/MainMenu.asm",
     ";Check for Kernal Replace Special IO and start it now\n   lda #rCtlKERNALPreStartWAIT\n   sta wRegControl+IO1Port\n"
     "   jsr WaitForTRDots ;use WaitForTRWaitMsg instead?\n",
     ";Check for Kernal Replace Special IO and start it now\n"
     "   lda #0\n   sta rwRegScratch+IO1Port ;VDrive: a nonzero answer asks for VDriveProbe\n"
     "   lda #rCtlKERNALPreStartWAIT\n   sta wRegControl+IO1Port\n"
     "   jsr WaitForTRDots ;use WaitForTRWaitMsg instead?\n"
     "   jsr VDriveProbe\n")
edit("Source/C64/MainMenuCRT/source/MainMenu.asm",
     "   ;rts ;SelectItem never returns\n\nAnyKeyErrMsgWait:\n",
     "   ;rts ;SelectItem never returns\n\n" + open(sys.argv[4]).read() + "AnyKeyErrMsgWait:\n")
# VDrive Config in the Teensy menu's Utilities, run with a VDrive Special IO (VDriveSwift_Remap picks the user's default one). After
# "LOAD"*",8,1 and RUN": the menu finds that entry and "Exit to BASIC" by their place in the list (items 2 and 3).
edit("Source/Teensy/MainMenuItems.h",
     '#include "TRMenuFiles/ROMs/ccgms_2021_Swiftlink_DE_38400.prg.h"\n',
     '#include "TRMenuFiles/ROMs/ccgms_2021_Swiftlink_DE_38400.prg.h"\n'
     '#ifdef Fab04_VDrive\n   #include "TRMenuFiles/ROMs/vdconfig.prg.h"\n#endif\n')
edit("Source/Teensy/MainMenuItems.h",
     ', (uint8_t*)Load8Run_prg               , sizeof(Load8Run_prg) , //used by location, see Load8Run:\n',
     ', (uint8_t*)Load8Run_prg               , sizeof(Load8Run_prg) , //used by location, see Load8Run:\n'
     '#ifdef Fab04_VDrive\n'
     '     rtFilePrg  , IOH_VDrive       , (char*)"VDrive Config             +VDrive", (uint8_t*)vdconfig_prg               , sizeof(vdconfig_prg) ,\n'
     '#endif\n')
# Added as a case of its own ahead of upstream's, instead of rewriting upstream's IOHandlerInit() line: an open upstream PR
# strips that line's trailing space, which would break a hunk that has it as context.
edit("Source/Teensy/IOHandlers.ino",
     "   PendingfBusSnoop = NULL; //clean slate for whatever InitHndlr below is about to stage\n",
     "   PendingfBusSnoop = NULL; //clean slate for whatever InitHndlr below is about to stage\n"
     "#ifdef Fab04_VDrive\n"
     "   if (IO1[rWRegCurrMenuWAIT] == rmtTeensy && MenuSource[SelItemFullIdx].IOHndlrAssoc != IOH_None)\n   {\n"
     "      IOHandlerInit(VDriveSwift_Remap(MenuSource[SelItemFullIdx].IOHndlrAssoc));   //SwiftLink programs keep the virtual drive\n      return;\n   }\n"
     "#endif\n")
PY
cp "$HERE/firmware/IOH_VDrive.c" "$HERE/firmware/IOH_VDriveSwift.c" "$HERE/firmware/IOH_VDriveREU.c" "$HERE/firmware/IOH_VDriveSwiftREU.c" \
   "$C/IO_Handlers/"
# the VDrive Config PRG as a menu binary, in the TR's own header format
python3 -I "$HERE/tools/make_util.py" "$W/util" >/dev/null
mkdir -p Source/Teensy/TRMenuFiles/ROMs
python3 -I - "$W/util/VDrive Config.prg" Source/Teensy/TRMenuFiles/ROMs/vdconfig.prg.h <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
rows = ",\n".join("\t" + ", ".join("0x%02x" % b for b in data[i:i + 12]) for i in range(0, len(data), 12))
open(sys.argv[2], "w").write("#ifndef VDCONFIG_PRG_H\n#define VDCONFIG_PRG_H\n\n// VDrive Config (teensyrom-vdrive util/vdconfig.s), generated by tools/make_tr_patch.sh\n"
                             "PROGMEM static const unsigned char vdconfig_prg[] = {\n" + rows + "\n};\n\n#endif /* VDCONFIG_PRG_H */\n")
PY
rm -rf "$W/util"
# the menu, rebuilt into its header (TeensyROMC64.h) by the TR's own tool (ACME 0.97, node), in a scratch copy of what it reads
B="$(mktemp -d)"; mkdir -p "$B/Source" "$B/$C"
cp -R "$TR/tools" "$B/"; cp -R "$TR/Source/C64" "$B/Source/"; cp "$C/Menu_Regs.h" "$B/$C/"
cp "$M/MainMenu.asm" "$B/$M/"
(cd "$B" && node tools/build-c64.mjs --project MainMenuCRT --rom-dir "$W/Source/Teensy/TRMenuFiles/ROMs" >/dev/null)
cmp -s "$B/$M/Menu_Regs.i" "$TR/$M/Menu_Regs.i" || { echo "Menu_Regs.i would change: carry it in the patch" >&2; exit 1; }
rm -rf "$B"
mkdir -p "$C/vdrive"
cp "$HERE"/d64/d64.c "$HERE"/d64/d64.h "$HERE"/d64/vdrive.c "$HERE"/d64/vdrive.h \
   "$HERE"/d64/vdrive_io.c "$HERE"/d64/vdrive_io.h "$HERE"/d64/vd_cfg.h "$C/vdrive/"
git add -A -N
git diff --no-color > "$HERE/firmware/teensyrom-vdrive.patch"
echo "wrote firmware/teensyrom-vdrive.patch ($(grep -c '^+++ ' "$HERE/firmware/teensyrom-vdrive.patch") files)"
