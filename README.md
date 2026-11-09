# TeensyROM+ Virtual Drive Support

Make the SD card and the USB drive of a [TeensyROM+](https://github.com/SensoriumEmbedded/TeensyROM) behave as IEC disk drives on a
Commodore 64, backed by `.d64`, `.d71` and `.d81` images. No IEC cable or additional hardware is required, and the virtual drives
can operate alongside real or simulated IEC devices (SD2IEC, Pi1541, etc., should work).

It has two halves:

* **A patched KERNAL** (the C64's own KERNAL ROM, read and patched in memory by the TR+ at each launch, plus about a kilobyte of 6502, served live). The serial routines
  (`LISTEN TALK SECOND TKSA CIOUT ACPTR UNLSN UNTLK`) ask a mailbox in I/O space (`$DE10`, with a 128-byte window at `$DF80`) whether the
  device is virtual. If so, the Teensy answers; otherwise the stock IEC code runs, so real drives keep working. LOAD and SAVE have
  block-transfer fast paths.
* **Teensy firmware** (`firmware/`, a patch for the TeensyROM source) with a portable C99 D64/D71/D81 engine (`d64/`) and a virtual drive on top of it:
  channels, the command channel, directory listings, write support, `CD` into folders and images.

## Status and Caveats

### Hardware Notes

The test suite runs primarily on VICE, with the TeensyROM+ mocked. Not all combinations of real-world devices have been tested. It works on my real C64 NTSC breadbin with a TR+, with or without a real 1541-][ drive. See the Status section of [CLAUDE.md](CLAUDE.md) for exactly what has and has not been checked on hardware. Testers are welcome!
Please raise a GitHub issue if you find a non-working configuration.

The `VDSUITE` test program passes on a real C64 on both the SD and the USB unit: `.d64` / `.d71` / `.d81` images made with `N:`, direct sector access,
`C:` copy, REL files, append, `V` validate, `L:` / `W-` lock and write protect, bad sectors, plain files from the card, `MD:` / `RD:`, CMD paths and `S:` on images.
Still **not run on a real C64**: the machine-code VDCOPY, the default device 8, VDrive+REU, `STATUS` compared with a real drive, and RUN/STOP during a load.
The test disk (`make testdisk`, see [Building and testing](#building-and-testing)) has a BASIC test program for most of them.

The original TR does not have the Kernal Replace functionality needed for this software. A more limited TR version, hooking the KERNAL's RAM vectors instead, is possible but not built yet; see [TR+ and TR compared](#tr-and-tr-compared) for what it would keep and lose.
A C128 in C64 mode works with 0.9.0.42: the firmware includes upstream's DMA HIRAM sense for Kernal Replace on a C128
([TeensyROM PR 78](https://github.com/SensoriumEmbedded/TeensyROM/pull/78)), and `VDSUITE`'s "run all" passes on an NTSC C128 (its checks by hand are not done yet; a PAL C128 is untested).

Tape (device 1) no longer works in any VDrive setup because its KERNAL code space was reused; for convenience, in its place `LOAD`, `SAVE` and `VERIFY` without a device number use device 8, as on the SX-64,
and SHIFT+RUN/STOP types `LOAD"*",8,1` and `RUN` (the first file on the disk in device 8, loaded to its own address) instead of the tape's `LOAD` and `RUN`.

### Timing sensitivity

The C64's ability to read the 8 KB of RAM under KERNAL ROM ($E000-$FFFF) is used in many games and advanced programs. When the TR+ is in the Kernal Replace mode (as required by this software), late ISRs can cause bad reads at the byte level, leading to potential memory corruption. VDrive avoids this by running the Ethernet stack from the main loop instead of a timer interrupt once a program starts (under any VDrive or Kernal Replace Special IO). Rare glitches remain while the network is busy (seen early in a CCGMS BBS receive). Other TR+ functionality (NFC readers, etc.) may similarly affect reads, but has not been fully tested.

## Installing
1. Patch the firmware source (TeensyROM checkout, FW ≥ 0.9 layout) and build the TR+ target:
   ```
   cd /path/to/TeensyROM && git apply /path/to/teensyrom-vdrive/firmware/teensyrom-vdrive.patch
   ```
   The patch adds `Fab04_VDrive` (enabled next to `Fab04_KernalReplace` in `Common_Defs.h`), a new Special IO
   `IOH_VDrive` ("VDrive KERNAL"), `IOH_VDriveSwift` ("VDrive+SwiftLink") and `IOH_VDriveSwiftREU` ("VDrive+Swift+REU"), `IOH_VDriveREU` ("VDrive+REU"), the library under `MinimalBoot/Common/vdrive/`, and a few lines in
   `KERNALPreStart()` and the Kernal Replace start so these Special IOs serve their own KERNAL image.

   Build with `arduino-cli` and the Teensy core `teensy:avr@1.61.0` (library `CRC32@2.0.1`). **Do not use Teensyduino 1.62.0**: SD reads
   stall with it. The easiest way to flash is to copy the resulting `.hex` to the SD card and select it in the TR menu (flashing over USB needs
   the Teensy's program button, since the TR has no soft reboot). The build is stamped `<stock>.<rev>`, e.g. `0.9.0.33`: that number shows in
   the C64 menu title and in the hex file name (`TeensyROM+_0.9.0.33_full.hex`), so you can tell a VDrive build from a stock `0.9` one.
2. On the C64: in Settings choose Special IO **VDrive KERNAL** (`a`/`A`); then start BASIC (`F2`) or launch a PRG.

   No KERNAL file is needed. At each launch the firmware reads the C64's own KERNAL ROM (by DMA, while the menu waits) and patches it in
   memory, if it is one the patch has been tested on: 901227-03 or -02 (C64), 251104-04 (SX-64), or the C64-mode KERNAL of a C128
   (325179-01, 325182-01). The `<Shift-K>` KERNAL setting now belongs to the plain "Kernal Replace" Special IO only; VDrive ignores it.
   With another ROM (JiffyDOS, other languages...) the drives stay off; then build an image on a PC (next step) and put it on the SD card
   as **`/VDrive/kernal.bin`**, which is used as it is whenever it exists.
3. Only for `/VDrive/kernal.bin`: build the patched KERNAL from your own ROM dump (the ROM itself is not in this repo; a 901227-03 can be obtained for personal use from a full installation of [VICE](https://vice-emu.sourceforge.io/) or directly from [this link](https://sourceforge.net/p/vice-emu/code/HEAD/tree/trunk/vice/data/C64/kernal-901227-03.bin?format=raw)):
   ```
   make build/kernal-tr.bin STOCK=/path/to/kernal.bin     # 8192 bytes, mailbox at $DE10, window at $DF80
   ```
   `tools/build.py` refuses a ROM whose code differs where the patch needs it. From a 901227-03 this is byte for byte the image the
   firmware builds in memory. To check that you built the same image as this repo (SHA-256, `shasum -a 256` or `sha256sum`):

   | File | SHA-256 |
   |---|---|
   | stock `kernal-901227-03.bin` (input) | `83c60d47047d7beab8e5b7bf6f67f80daa088b7a6a27de0d7e016f6484042721` |
   | `kernal-tr.bin` from this revision | `c4e9d2276241202f450648e2aedba910d8f1380fb9e730b6d42bf9014d7a5207` |
   | `kernal-tr.bin` last run on a real C64 (0.9.0.45, commit `02c6490`) | `62704caed1a4f02c795fd5f73bf8e1a428d776cf70fd63c0c739c2aa7719f5cc` |

   A different stock hash means a different ROM dump; the patches may not fit it. The "this revision" row is checked by `make test`.
   The two `kernal-tr.bin` rows differ when this revision has KERNAL changes not yet run on hardware (see [CLAUDE.md](CLAUDE.md), Status).
4. **A KERNAL in the machine itself** (an EPROM in the KERNAL socket, a MEGA65 C64 core's KERNAL file, any board that replaces the
   KERNAL as it does for JiffyDOS) works too, with the same `kernal-tr.bin`: no separate build. Leave `/VDrive/kernal.bin` off the card.
   The firmware reads that KERNAL at launch, sees that it is already this build's VDrive KERNAL, serves nothing (no Kernal Replace)
   and answers the drives as usual. The KERNAL only uses the mailbox while a VDrive Special IO answers (it checks `$DE19` for `$D6`
   first); otherwise (no TR, another Special IO, the TR menu itself) every serial call goes straight to the real bus. Tape stays
   broken and `SYS64738` returns to the TR menu only while a VDrive Special IO runs, as with the served image. The banner shows the
   drive numbers there too (from 0.9.0.49: the firmware leaves them in the TR menu's registers, which the menu's own start-up reads). A `kernal-tr.bin` from
   before 0.9.0.48 has no such check: replace it (with no handler its serial calls wait out a time-out each, and under the TR menu
   they would write the menu's settings). Firmware and KERNAL should come from the same revision; with a resident VDrive KERNAL of
   another revision the firmware still answers the drives but skips the menu's bus probe (the KERNAL's own scan sets the numbers).

While a VDrive Special IO runs, `SYS64738` (or any program that ends by jumping to the RESET vector) returns to the TR menu, writing
back anything not yet saved, instead of restarting BASIC. To get a fresh BASIC, start it from the menu.

The "VDrive+SwiftLink" Special IO runs the virtual drive and the SwiftLink / Turbo232 modem together. When the default Special IO is VDrive or VDrive+SwiftLink, menu programs paired with plain SwiftLink (CCGMS, Retromate, Simple SwiftLink Term) start with it, so launching one does not drop the drive. `.crt` SwiftLink/Turbo232 images are left alone and run with the C64's own KERNAL. It costs SwiftLink's normal start-up RAM (about 355 KB of RAM2 buffers, drive menu freed), so it is a separate Special IO.

The "VDrive+REU" Special IO runs the virtual drive and an REU (RAM Expansion Unit) together, with the same **512 KB** REU as the plain "512KB REU" Special IO. When the default Special IO is any VDrive one, menu programs paired with the REU start with it, so launching one does not drop the drive.

The "VDrive+Swift+REU" Special IO has all three. SwiftLink's buffers leave room for a **128 KB** REU (a 1700), not 512 KB; software that insists on a bigger one will refuse it. When it is the default Special IO, menu programs paired with SwiftLink start with it (REU programs get VDrive+REU and its 512 KB, as above).

## Using it

Once enabled, your TeensyROM+ provides two virtual devices. By default, device numbers are automatic (SD and USB take the first free numbers from 8 up); they can be changed/fixed with `U0>` (see below).
The power-on banner shows them, SD first:
```
    **** COMMODORE 64 BASIC V2 ****

 64K RAM  38911 FREE  VDRIVE SD 8 USB 9
```
(`SD10 USB11` with two-digit numbers, `NOSD` / `NOUSB` for a missing card or stick; under plain Kernal Replace or older firmware the line is the stock `64K RAM SYSTEM  38911 BASIC BYTES FREE`.)

Each of the SD and USB virtual drives can have one `.d64`, `.d71`, or `.d81` file mounted at any given time (or can be unmounted, akin to removing the disk from a real drive). When unmounted, the devices provide access to the actual SD or USB filesystem: you can navigate, load plain `PRG` files, create and remove folders, and create or mount images.

If you launch a PRG program from within a `.dXX` image on the TeensyROM+, the firmware automatically mounts the containing disk image.

If you want to create a new virtual image, you can make one in VICE and copy it to the relevant SD card or USB stick in the usual way, or you can use the `N0:` (format) command with nothing mounted and `.dXX` in the disk name (more details below).

To switch between multiple disk images, you can use the TR+ alt button. Setup and usage is documented below. You can also issue mount/unmount commands to the TR+'s command channel over serial or Ethernet.

The TR+'s LED works as a drive activity light: it blinks while a virtual drive is busy and goes back to how it was (normally on) a moment after the last access. Commands for a real drive on the bus leave it alone. This was contributed by ziggystar12, the creator of Mean Hamster Engine.

### Commands

The examples here use `8` as an illustration only, and work the same on both SD and USB.

| Command | What it does |
|---|---|
| `LOAD"$",8 : LIST` | List the mounted image (or current folder if none mounted) |
| `LOAD"NAME",8` | Load a BASIC file |
| `LOAD"NAME",8,1` | Load a binary file |
| `LOAD"NAME"` | Same as `LOAD"NAME",8` (also `SAVE`, `VERIFY`): device 8 is the default, as on the SX-64. It is whatever answers at 8, the SD drive or a real drive |
| SHIFT+RUN/STOP | Types `LOAD"*",8,1` and `RUN`: loads and starts the first file on the disk in device 8 |
| `SAVE"NAME",8` | Save a BASIC file |
| `SAVE"@0:NAME",8` | Save a BASIC file, replacing an existing one |
| `VERIFY"NAME",8` | Verify a BASIC file |
| `OPEN15,8,15,"CD:GAME.D64"` | Mount a disk image (`.d64`/`.d71`,`.d81` supported) |
| `OPEN15,8,15,"CD:SUBDIR"` | Move into a folder |
| `OPEN15,8,15,"CD:.."` | Move up one level |
| `OPEN15,8,15,"CD:←"` | Unmount the image (← is the left-arrow key, top left; `$5F`, shown as `_` on a PC) |
| `OPEN15,8,15,"CD:/"` | Unmount and move to the folder root |
| `OPEN15,8,15,"CD:/GAMES/G.D81"` | Paths work: absolute (leading `/`) or relative, folders and an image in one go |
| `OPEN15,8,15,"CD//GAMES/:G.D81"` | The CMD HD / sd2iec forms work too: `CD//` (root), `CD//GAMES` (from the root), `CD/GAMES` and `CD/GAMES/:SUB` (relative), `CD_`; see "CMD paths" below |
| `OPEN15,8,15,"S:OLD*"` | Scratch |
| `OPEN15,8,15,"N:NAME,ID"` | Format the mounted disk |
| `OPEN15,8,15,"N:NAME.D64,ID"` | With nothing mounted: create a new blank image (`.D64`, `.D71` or `.D81`) in the current folder (or `N:/DIR/NAME.D64`), mount it and format it with disk name `NAME` (ID `00` if left out). Never replaces an existing file (63). A `.D81` on a slow USB stick may take longer than the KERNAL waits (about 1.6 s; not measured yet) |
| `OPEN15,8,15,"R:NEW=OLD"` | Rename a file |
| `OPEN15,8,15,"UI"` / `"UJ"` / `"U9"` | Reset (status 73); `UI+` / `UI-` (bus speed) are accepted and do nothing |

(Don't forget to `CLOSE15` with the above.)

### Directory usage
Paths also work inside names. A directory listing by path only *lists*: nothing stays selected afterwards (the folder and image selected
before are restored when the listing is closed). `$==` is what selects.

| Command | Result |
|---|---|
| `LOAD"$/GAMES/GAME.D64",8` | List the image (so does `LOAD"GAME.D64",8` when nothing is mounted) |
| `LOAD"$GAMES/",8` | List a folder |
| `LOAD"$/GAMES/GAME.D64/S*",8` | List only the matching files of the image |
| `LOAD"GAMES/GAME.D64/START",8,1` | Load a file from the image. This leaves the folder and image selected, so the program's own LOADs find its files |
| `LOAD"$==GAMES",8` | `CD:` into the folder and list it; it stays selected |
| `LOAD"$==DEMO.D64",8` | Mount the image and list it |
| `LOAD"$==/GAMES/DEMO.D64/S*",8` | Mount the image and list the matching files |

#### CMD paths
The CMD HD syntax (also used by sd2iec) puts a path in front of a colon: `//` starts at the card root, a single `/` at the current
folder, and the path ends with `/` before the colon. The drive / partition number in front is ignored. This works in `LOAD`/`SAVE`/`OPEN`
names, `$` listings, `CD`, `MD`, `RD` and `N`. Our own syntax stays as it was where CMD has no path: after the colon and in names
without one, a leading `/` is the card root (`CD:/GAMES`, `LOAD"$/GAMES/G.D64"`).

| Command | Result |
|---|---|
| `LOAD"//GAMES/G.D64/:START",8,1` | Load a file from an image, from the root |
| `LOAD"/G.D64/:START",8,1` | The same, relative to the current folder |
| `LOAD"$//GAMES/:*",8` | List a folder by path (only lists, like `$/GAMES/`) |
| `OPEN15,8,15,"CD/GAMES"` | Relative: into `GAMES` of the current folder (before this release `CD/GAMES` went from the root; use `CD//GAMES` or `CD:/GAMES` for that) |
| `OPEN15,8,15,"CD0//GAMES/:G.D64"` | Mount an image by path |
| `OPEN15,8,15,"MD//GAMES/:NEW"` | Make a folder elsewhere; the current folder does not change |
| `OPEN15,8,15,"N//GAMES/:NEW.D64,ID"` | Create a new image in another folder (with nothing mounted) |
| `LOAD"$==T",8` | Enter a folder called `T` (or `P`) |

`$==path` (also `$:==path`, `$0:==path`) always means a path, even without a `/` or an image suffix. A single `=`, as in `$=T` / `$=P`, stays a
CBM listing modifier and is not a path. With an image mounted, a bare name is a file inside it, and so is a name with a `/` in it (`A/B` is a legal CBM file name). Only a leading `/`,
a component with an image suffix (`X.D64/`) or `..` makes it a path; leave the image with `CD:←`.

### Device numbers
The KERNAL image file contains no device numbers, so one image serves every setup (the firmware writes the banner's numbers into the copy it serves). Device number assignment is automatic by default: before
it starts a program, the TeensyROM menu probes the real serial bus at 8, 9, 10, ... and the SD drive takes the first free number, the USB
drive the next (only units whose card / stick is present). (A KERNAL image started some other way probes on its first disk access instead.) With no other drive attached that is 8 and 9, and the probe costs a few milliseconds; with a
physical drive at 8 the SD drive becomes 9, and so on.

The power-on banner shows the numbers too (`VDRIVE SD 9 USB10`): the firmware writes them into the KERNAL copy it serves once the menu's
probe is done (a KERNAL started some other way does the same probe itself at power-on, on the real bus only, before showing them).
`PEEK(56854)` (SD) and `PEEK(56855)` (USB) show the current numbers from the program's first instruction (0 = none), the directory header id shows it too (`0 "SD:/" 09 2A`), and the serial console prints the numbers
at start-up. Nothing is probed when nothing needs a number (no card, or both numbers fixed). A drive that has not powered up yet when the
program starts looks like a gap, so fix the numbers if some device boots slowly (e.g. a Wi-Fi drive).

To fix a number, use the same command real drives use (stored in EEPROM; 0 or an invalid value = automatic):
```
OPEN15,8,15,"U0>"+CHR$(10):CLOSE15     the SD drive is now device 10 (stored immediately; 8 goes back to the real bus)
OPEN15,9,15,"U0>"+CHR$(11):CLOSE15     the USB drive becomes 11
OPEN15,10,15,"U0>"+CHR$(0):CLOSE15     CHR$(0) makes that unit automatic again
```
The accepted range is 8-30 (a number outside that range, or the other unit's number will give status `33,SYNTAX ERROR`). A fixed number is never handed out to the other
unit by the scan; every other device number keeps using the real IEC code. However, if you set one of the VDrives to the same number as a real device, the real device will be hidden and inaccessible.

### Images and mount points
Image formats: `.d64` (35/40 tracks), `.d71` (1571, 70 tracks) and `.d81` (1581; root directory only, no sub-directories) are recognised by file size and extension; files may carry the trailing error-info bytes (one per sector, as copiers and VICE write them), and a read of a sector they mark bad fails as on a real drive: `20`-`24`, `27`, `29` or `74` with the sector's track and sector on the error channel (`docs/DESIGN.md` section 35).
Opening a `.dXX` in the TeensyROM menu and launching a program from it mounts that image automatically on device 8 (SD) or 9 (USB), so the program's own LOADs find its files; `CD:←` unmounts it. The last mounted image of each drive is also remembered in a hidden `/.vdrive` file on that card and mounted again at the next start (any unmount forgets it).
The USB drive works the same. Any other device number uses the real IEC bus untouched.

### Changing the disk with the Alt button
Without a computer: set the TR+ Alt button action to **None** (TR settings), and list the images you want in `vdrive-config.txt` in the root of the SD card (and, for the USB stick, in the root of the stick):
```
# path on that drive, border colour (0-15), number of flashes
/GAMES/ZORK.D64, 5, 1
/GAMES/BIG DISK.D81, 3, 2
/UTIL/TOOLS.D64
```
While a program runs with the VDrive handler: one tap = next image of the SD list (the first if nothing is mounted), two taps within one second = next image of the USB list. The border flashes the chosen colour that many times (250 ms on, 750 ms off) once the disk is changed; a missing card/stick, a missing, empty or unusable list flashes red for 2 s. Images that no longer exist are skipped. The change waits for a quiet moment on the drive, like `trvdrive`. Details: `docs/DESIGN.md` section 22.

### Changing the disk while a program runs (remote control)
With the VDrive handler active, the TeensyROM's remote-control port (TCP 2112, or the USB serial port) accepts two extra commands that work while the C64 runs a program: `0x64E3` reads what a unit has mounted, `0x64E4` mounts an image (or unmounts with an empty path). The swap waits for a quiet moment on the C64's drive, closes the unit's open channels and mounts, like `CD:`. Details and framing are in `docs/DESIGN.md` section 20. Directory listings (`trls`) are also allowed while a program runs, after the same wait, so you can browse for an image; the other file commands (`trget`, `trput`, ...) still answer `Busy!`. The `trvdrive` tool in `teensyrom-rcp` wraps get and set:

```
trvdrive -H tr.local                      # show SD and USB
trvdrive -H tr.local sd /GAMES/DISK2.D64  # mount
trvdrive -H tr.local sd -u                # unmount
```

With the TR menu showing (no program running) the same commands show and change the image each unit will mount at the next start (the `/.vdrive` file on its card; the reply says `VDrive not running`). A program launched from a disk image opened in the menu still mounts that image instead.

## Compatibility

### Supported
| Feature | Notes |
|---|---|
| `LOAD` / `SAVE` | Fast path, ~255 B per mailbox transaction |
| `VERIFY`, `OPEN` / `PRINT#` / `INPUT#` | Byte path |
| File types | PRG, SEQ, USR, REL |
| Wildcards | |
| `@0:` replace | |
| Append (`,A`) | `OPEN 2,8,2,"LOG,S,A"`; a disk full puts the file back as it was |
| Error channel | |
| Directory with `$:pattern` | |
| REL files | `OPEN 2,8,2,"NAME,L,"+CHR$(len)`, `P` positioning, `PRINT#` one record, `INPUT#`/`GET#`; D64/D71 up to 720 blocks, D81 with super side sectors |
| `V` validate | Rebuilds the BAM from the files (REL side sectors included); unclosed files are deleted, as on a real drive |
| `L:name` lock, `W-1` / `W-0` write protect | CMD-drive syntax. `L` toggles the lock flag (`<`, the file cannot be scratched); `W-1` makes the mounted image read-only until `W-0` or the next mount |
| Plain files on the card | With nothing mounted, `LOAD"GAME.PRG",8` (also `"GAME"`, `"GA*"`, `"DIR/GAME.PRG"`) loads the file itself; reading only (`SAVE` answers 74), one plain file open per drive (a second answers 70) |
| `MD:NAME` / `RD:NAME` | With nothing mounted: make / remove (empty) folders on the card, also through a CMD path (`MD//GAMES/:NEW`, `RD/OLD/:X`). With an image mounted (D81 partitions): not supported |
| Long host file names | A card file or folder whose name has more than 16 characters, or characters the C64 cannot type (`, : * ? = "`, accents), is listed under an alias with an up arrow: `The-Pumpkins-Breadbin-of-Horror-side1.D64` shows as `THE-PU↑SIDE1.D64`, and `CD:`, `LOAD` and paths take that alias. Two names with the same alias are both hidden (rename one) |
| `C:new=old` copy | Also joins files: `C:new=a,b,c` (up to 8); the new file has the first source's type |
| Direct sector access | `OPEN n,8,n,"#"` takes a 256-byte buffer channel; on channel 15 `U1:n 0 t s` (or `UA`) reads a sector into it, `U2`/`UB` writes it back (`B-R`/`B-W` do the same as `U1`/`U2`), `B-P:n,pos` sets the buffer pointer, `B-A`/`B-F` allocate and free a sector in the BAM (`B-A` on a used sector answers `65` with the next free one). D64, D71 and D81; needs an image mounted |

### Not yet supported
| Feature | Notes |
|---|---|
| Writing plain files to the card (`SAVE` with nothing mounted) | Mount an image (or create one with `N:NAME.D64,ID`) |
| Running programs with their own fastloaders | No `M-R`/`M-W`/`M-E`, so programs that upload code to the drive (or probe its ROM to detect the drive type) cannot run from the card. With a real 1541, `VDCOPY` `D` writes the `.d64` onto a floppy, and the program runs from there |
| `U3`-`U8` user jumps, `B-E` | Need drive code |
| Copy protection that checks for bad sectors | Read errors from the image's error-info bytes are emulated (`U1`, `LOAD`, `GET#`); protections that time the drive, read raw GCR, or check the drive's memory (`M-R`) still fail. Writing a bad sector does not repair it |

### TR+ and TR compared
The original TR cannot serve a replacement KERNAL, so a TR version (**not built yet**; release builds of plain TR firmware carry no VDrive) would hook the KERNAL's
RAM vectors (`$0314-$0333`) instead of patching the serial routines. The drive itself (the Teensy side) is the same on both. What differs:

| Feature | TR+ (patched KERNAL) | TR (RAM vectors, planned) |
|---|---|---|
| Image formats (`.d64`, `.d71`, `.d81`), `CD` and paths (CMD syntax included) | Yes | Yes |
| `N:`, `S:`, `R:`, `C:`, `V`, `L:`, `MD:` / `RD:` | Yes | Yes |
| REL files, append, `#` channels with `U1` / `U2` / `B-P` | Yes | Yes |
| Plain card files, long-name aliases | Yes | Yes |
| Automatic and fixed (`U0>`) device numbers | Yes | Yes |
| Auto-mount and remembered mounts | Yes | Yes |
| `trvdrive` / `trls` while a program runs | Yes | Yes |
| VDrive+SwiftLink | Yes | Yes |
| Fast `LOAD` / `SAVE` / `VERIFY` | Yes | Yes |
| Programs that call the serial routines directly (`LISTEN` `TALK` `SECOND` `TKSA` `ACPTR` `CIOUT` `UNTLK` `UNLSN`, `$FFB1` and friends: DOS wedges, some directory readers and disk utilities) | Yes | No: they talk to the real bus and do not see the virtual drives. VDCOPY and VDrive Config probe units this way and would have to probe another way (e.g. an `OPEN` of channel 15) |
| Programs that call `RESTOR` (`$FF8A`) or install their own vectors (many games and demos at start-up) | Yes | No: the drives are gone until the vectors are set again. Multi-load games that use plain `$FFD5` with the default vectors are fine |
| Code that calls stock routines directly (e.g. `$F333`) instead of the jump table | Yes | No: skips the hooks |
| Programs that call the KERNAL with the character ROM banked in (`$01` = `$33` / `$32`; rare) | Yes | No: the handler is fetched from RAM instead of I/O, and the program crashes |
| Drive numbers in the power-on banner | Yes | No |
| SHIFT+RUN/STOP types `LOAD"*",8,1` and `RUN` | Yes | No: the stock `LOAD` and `RUN` from tape |
| `SYS64738` back to the TR menu | Yes | No: a normal reset |
| Disk switching with the Alt button, with a border flash | Yes | No: the TR (rev 0.2 and 0.3) has no Alt button. Change disks with `trvdrive` over serial or Ethernet instead |
| `LOAD` / `SAVE` / `VERIFY` without a device number | Device 8 | Tape (device 1). Device 8 is possible, but then the tape is blocked; the tape is kept |
| Tape (device 1) | No: its code space holds the patch | Yes |

## Utilities

### Copying files and whole disks between drives: `VDCOPY`
`make util` builds `build/util/vdcopy.prg` (pure machine code behind a `SYS` stub, loads at `$0801`; it is also on `vdtest.d64`).
It copies between any two units, in either direction: real drive to SD/USB, SD/USB to a real drive, or SD to USB. Put it on the card, load it from the SD drive and `RUN` it (with the real drive and its disk on the bus, if you use one):
```
LOAD"VDCOPY",9 : RUN        (9 when a real drive is 8; the SD drive is 8 when there is none)
```
The screen takes the TeensyROM menu's colours (as VDrive Config does). A menu offers `F` (files), `D` (a whole disk) and `Q` (quit, back to BASIC with the old colours).
Both ask for the source unit (default 8) and the destination unit (default: the SD drive, `PEEK(56854)`; type both numbers for any other combination, e.g. 8 and 9 for SD to USB) and check that both answer.

**Files (`F`).** Lists the source disk (PRG, SEQ and USR files, 20 lines at a time) and asks what to copy: `A` for all, or numbers such as `1,3,5-8` (RETURN goes back to the menu). Existing files on
the destination are skipped, or replaced if you answer `Y` to "replace existing". Copies go 2 KB at a time (read from the source, then written
to the destination), the destination's error channel is checked after every chunk, so a full disk (72) or a failed write stops that file and
removes the partial copy. RUN/STOP stops after the current chunk. A vdrive source or destination needs an image mounted (`CD:`). Hardware-verified: vdrive to vdrive and vdrive to a 1541 (that was the earlier BASIC version; the machine-code rewrite is checked in VICE for all directions, not yet on hardware). The byte copy itself goes through the KERNAL one byte at a time, so its speed is set by the drives.
REL files are not shown; names with `* , : = ?` are skipped.

**A whole disk (`D`): a `.d64` onto a real floppy, or a floppy into a `.d64`.** Copies all 683 sectors of a 35-track 1541 disk, one at a time, with the drives' own block commands
(`U1` on the source, `B-P` + `U2` on the destination, through a `#` buffer channel on each), so it works between any two units and needs no firmware support beyond what the drive already does.
Disks whose loaders or copy protection need their own drive code still need their sectors in the standard format to come out right (no GCR, no half tracks, no `.g64`).
* A vdrive **source** asks for an image to mount first (`/GAMES/GAME.D64`, any path `CD:` takes; RETURN = the one already mounted).
* A vdrive **destination** asks for a new image (`GAME` or `GAME.D64`, made with `N:` in the current folder or the path given; an existing file is not touched) or, on RETURN, writes over the mounted image after a `Y`.
* A real **destination**: `F` formats the disk first with the source's disk name and ID (the sector headers then carry the right ID), `W` writes onto the disk as it is (it must be formatted), `Q` cancels.
* The source's track 18 sector 0 must say "1541" (`A`, single-sided); anything else (a `.d71`, a `.d81`, an odd disk) asks before copying 35 tracks.

While it runs, the screen is a map of the disk: one column per track (1-35), one row per sector (0-20); `+` is the sector being copied, a ball one that is done, a reversed digit a read error (the last digit
of its number, e.g. `3` for `23,READ ERROR`). A read error is counted and the sector is written as the drive gave it (a 1541 keeps the data of a `23`), so a damaged floppy still comes over as far as it can be read; the image
then has no error table, so a protection that checks for those errors fails. A write error, a failed transfer or RUN/STOP ends the copy and names the sector. At the end the destination gets `I` (it re-reads the new BAM).
Through the KERNAL's serial routines a real 1541 moves about one sector a second each way, so a whole disk should take 10-15 minutes (an estimate, not measured on hardware yet; `docs/DESIGN.md` section 40 explains why VICE timings say nothing about it).
Uses `$0801-$27FF`, tables at `$2800-$3FFF` and the buffer `$C000-$C7FF`.

### VDrive Config (the list editor)
`VDrive Config` (in the TeensyROM menu's Utilities, run with your default VDrive Special IO; also built by `make util` into `build/util/`) edits the Alt-button lists (see 'Changing the disk with the alt button' above) on the C64 and shows / changes what is mounted:
```
RETURN mount the entry   B browse the card, mount any image   U unmount
A add (browse, colour, flashes)   E edit   D delete   +/- move down/up
T switch SD / USB list   R refresh   Q back to the TeensyROM menu   (left arrow cancels an entry field)
```
The top lines show the image mounted on each drive; `*` marks the list entry that is mounted. Changes to the list are saved immediately in `vdrive-config.txt`. Details: `docs/DESIGN.md` section 23.

## Building and testing
Needs `cc65` (`ca65`/`ld65`), VICE (`x64sc`, `c1541`), a C/C++ compiler and Python 3; ACME 0.97 for the menu's bus probe test (and, with
node, for `tools/make_tr_patch.sh`).

```bash
make test
```

The KERNAL image is built from your own stock ROM, so no Commodore ROM bytes are in this repository:

```bash
make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin
```

### Test disk for real hardware
`make testdisk` builds `build/testdisk/vdtest.d64` with the BASIC test programs from `tests/basic/` (the same ones `make test` runs in VICE)
and `VDCOPY`. Copy it to the SD card, mount it with `OPEN15,8,15,"CD:VDTEST.D64":CLOSE15`, then `LOAD"$",8` to see the programs. Each one
prints its results; please include them in an issue if one fails. `tests/basic/README.md` says what each program checks.

## Further Documentation
* [firmware/README.md](firmware/README.md): how the firmware glue and the KERNAL patches work.
* [docs/DESIGN.md](docs/DESIGN.md): the design, one section per milestone, including alternatives considered (section 19).
* [CLAUDE.md](CLAUDE.md): project notes, status and gotchas for contributors.
* [CONTRIBUTING.md](CONTRIBUTING.md): branches (`main` vs `bleeding-edge`), tests and project rules.
* [CHANGELOG.md](CHANGELOG.md): changes by firmware revision.

## Credits
* ziggystar12 (creator of Mean Hamster Engine): the drive activity LED (0.9.0.46), from his own VDrive build.

## License
MIT License, see [LICENSE](LICENSE), the same license as the TeensyROM firmware this patches. A firmware build combines both, so keep both copyright notices when distributing one.
