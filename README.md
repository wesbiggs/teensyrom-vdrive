# TeensyROM+ Virtual Drive Support

Make the SD card and the USB drive of a [TeensyROM+](https://github.com/SensoriumEmbedded/TeensyROM) behave as IEC disk drives on a
Commodore 64, backed by `.d64`, `.d71` and `.d81` images. No IEC cable or additional hardware is required, and the virtual drives
can operate alongside real or simulated IEC devices (SD2IEC, Pi1541, etc., should work).

It has two halves:

* **A patched KERNAL** (901227-03 plus a few hundred bytes of 6502, served live by the TR+). The serial routines
  (`LISTEN TALK SECOND TKSA CIOUT ACPTR UNLSN UNTLK`) ask a mailbox in I/O space (`$DE10`, with a 128-byte window at `$DF80`) whether the
  device is virtual. If so, the Teensy answers; otherwise the stock IEC code runs, so real drives keep working. LOAD and SAVE have
  block-transfer fast paths.
* **Teensy firmware** (`firmware/`, a patch for the TeensyROM source) with a portable C99 D64/D71/D81 engine (`d64/`) and a virtual drive on top of it:
  channels, the command channel, directory listings, write support, `CD` into folders and images.

## Status and Caveats

### Hardware Notes

The test suite runs primarily on VICE, with the TeensyROM+ mocked. Not all combinations of real-world devices have been tested. It works on my real C64 NTSC breadbin with a TR+, with or without a real 1541-][ drive. See the Status section of [CLAUDE.md](CLAUDE.md) for exactly what has and has not been checked on hardware. Testers are welcome!
Please raise a GitHub issue if you find a non-working configuration.

Newer features pass the host tests and VICE but have **not yet been run on a real C64**: direct sector access, `C:` copy, REL files, append,
`V` validate, `L:` / `W-` lock and write protect, plain files from the card, `MD:` / `RD:`, the Alt button, VDrive Config, the machine-code
VDCOPY, the default device 8, and `.d71` images. The test disk (`make testdisk`, see
[Building and testing](#building-and-testing)) has a BASIC test program for most of them.

The original TR does not have the Kernal Replace functionality needed for this software, though it may be possible in the future to create a more limited TR-compatible version. 
The C128 is not yet supported, but this is likely possible in the future.

Tape (device 1) no longer works in any VDrive setup because its KERNAL code space was reused; for convenience, in its place `LOAD`, `SAVE` and `VERIFY` without a device number use device 8, as on the SX-64.

### Timing sensitivity

The C64's ability to read the 8 KB of RAM under KERNAL ROM ($E000-$FFFF) is used in many games and advanced programs. When the TR+ is in the Kernal Replace mode (as required by this software), late ISRs can cause bad reads at the byte level, leading to potential memory corruption. VDrive avoids this by running the Ethernet stack from the main loop instead of a timer interrupt once a program starts (under any VDrive or Kernal Replace Special IO). Rare glitches remain while the network is busy (seen early in a CCGMS BBS receive). Other TR+ functionality (NFC readers, etc.) may similarly affect reads, but has not been fully tested.

## Installing
1. Build the patched KERNAL from your own copy of a 901227-03 dump (the ROM itself is not in this repo, but it can be obtained for personal use from a full installation of [VICE](https://vice-emu.sourceforge.io/) or directly from [this link](https://sourceforge.net/p/vice-emu/code/HEAD/tree/trunk/vice/data/C64/kernal-901227-03.bin?format=raw)):
   ```
   make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin     # 8192 bytes, mailbox at $DE10, window at $DF80
   ```
   Copy it to the SD card or USB drive. To check that you built the same image as this repo (SHA-256, `shasum -a 256` or `sha256sum`):

   | File | SHA-256 |
   |---|---|
   | stock `kernal-901227-03.bin` (input) | `83c60d47047d7beab8e5b7bf6f67f80daa088b7a6a27de0d7e016f6484042721` |
   | `kernal-tr.bin` from this revision | `14a7d27e0393c56185116679db217fb8ca3cb9c2a79e37e91ebc477ba71b25d6` |
   | `kernal-tr.bin` last run on a real C64 (0.9.0.28, commit `bf3c74c`; same patches through `9270008`) | `e1db8e02b6cab9f6eb99a597a1c04f2b5a67fac914a36dc1e4259d7272f53af2` |

   A different stock hash means a different ROM dump; the patches may not fit it. The "this revision" row is checked by `make test`.
   The two `kernal-tr.bin` rows differ when this revision has KERNAL changes not yet run on hardware (see [CLAUDE.md](CLAUDE.md), Status).
2. Patch the firmware source (TeensyROM checkout, FW ≥ 0.9 layout) and build the TR+ target:
   ```
   cd /path/to/TeensyROM && git apply /path/to/teensyrom-vdrive/firmware/teensyrom-vdrive.patch
   ```
   The patch adds `Fab04_VDrive` (enabled next to `Fab04_KernalReplace` in `Common_Defs.h`), a new Special IO
   `IOH_VDrive` ("VDrive KERNAL"), `IOH_VDriveSwift` ("VDrive+SwiftLink") and `IOH_VDriveSwiftREU` ("VDrive+Swift+REU"), the library under `MinimalBoot/Common/vdrive/`, and one condition in
   `KERNALPreStart()` so the KERNAL image is loaded when this Special IO is selected.

   Build with `arduino-cli` and the Teensy core `teensy:avr@1.61.0` (library `CRC32@2.0.1`). **Do not use Teensyduino 1.62.0**: SD reads
   stall with it. The easiest way to flash is to copy the resulting `.hex` to the SD card and select it in the TR menu (flashing over USB needs
   the Teensy's program button, since the TR has no soft reboot). The build is stamped `<stock>.<rev>`, e.g. `0.9.0.29`: that number shows in
   the C64 menu title and in the hex file name (`TeensyROM+_0.9.0.29_full.hex`), so you can tell a VDrive build from a stock `0.9` one.
3. On the C64: in the TR file browser select the image and press `<Shift-K>`; in Settings choose Special IO
   **VDrive KERNAL** (`a`/`A`); then start BASIC (`F2`) or launch a PRG.


The "VDrive+SwiftLink" Special IO runs the virtual drive and the SwiftLink / Turbo232 modem together. When the default Special IO is VDrive or VDrive+SwiftLink, menu programs paired with plain SwiftLink (CCGMS, Retromate, Simple SwiftLink Term) start with it, so launching one does not drop the drive. `.crt` SwiftLink/Turbo232 images are left alone and run with the C64's own KERNAL. It costs SwiftLink's normal start-up RAM (about 355 KB of RAM2 buffers, drive menu freed), so it is a separate Special IO.

The "VDrive+Swift+REU" Special IO adds an REU (RAM Expansion Unit) to those two. SwiftLink's buffers leave room for a **128 KB** REU (a 1700), not 512 KB; software that insists on a bigger one will refuse it. When it is the default Special IO, menu programs paired with SwiftLink or with the REU start with it; otherwise REU programs keep the plain 512 KB REU.

## Using it

Once enabled, your TeensyROM+ provides two virtual devices. By default, device numbers are automatic (SD and USB take the first free numbers from 8 up); they can be changed/fixed with `U0>` (see below).
The power-on banner shows them, SD first:
```
    **** COMMODORE 64 BASIC V2 ****

 64K RAM, 38911 FREE, VDRIVE 8+9
```
(`VDRIVE 9` with only a card, `VDRIVE N/A` with neither card nor stick; no `VDRIVE` part when the image runs under plain Kernal Replace or older firmware.)

Each of the SD and USB virtual drives can have one `.d64`, `.d71`, or `.d81` file mounted at any given time (or can be unmounted, akin to removing the disk from a real drive). When unmounted, the devices provide access to the actual SD or USB filesystem: you can navigate, load plain `PRG` files, create and remove folders, and create or mount images.

If you launch a PRG program from within a `.dXX` image on the TeensyROM+, the firmware automatically mounts the containing disk image.

If you want to create a new virtual image, you can make one in VICE and copy it to the relevant SD card or USB stick in the usual way, or you can use the `N0:` (format) command with nothing mounted and `.dXX` in the disk name (more details below).

To switch between multiple disk images, you can use the TR+ alt button. Setup and usage is documented below. You can also issue mount/unmount commands to the TR+'s command channel over serial or Ethernet.

### Commands

The examples here use `8` as an illustration only, and work the same on both SD and USB.

| Command | What it does |
|---|---|
| `LOAD"$",8 : LIST` | List the mounted image (or current folder if none mounted) |
| `LOAD"NAME",8` | Load a BASIC file |
| `LOAD"NAME",8,1` | Load a binary file |
| `LOAD"NAME"` | Same as `LOAD"NAME",8` (also `SAVE`, `VERIFY`): device 8 is the default, as on the SX-64. It is whatever answers at 8, the SD drive or a real drive |
| `SAVE"NAME",8` | Save a BASIC file |
| `SAVE"@0:NAME",8` | Save a BASIC file, replacing an existing one |
| `VERIFY"NAME",8` | Verify a BASIC file |
| `OPEN15,8,15,"CD:GAME.D64"` | Mount a disk image (`.d64`/`.d71`,`.d81` supported) |
| `OPEN15,8,15,"CD:SUBDIR"` | Move into a folder |
| `OPEN15,8,15,"CD:.."` | Move up one level |
| `OPEN15,8,15,"CD:←"` | Unmount the image (← is the left-arrow key, top left; `$5F`, shown as `_` on a PC) |
| `OPEN15,8,15,"CD:/"` | Unmount and move to the folder root |
| `OPEN15,8,15,"CD:/GAMES/G.D81"` | Paths work: absolute (leading `/`) or relative, folders and an image in one go |
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
| `LOAD"$==T",8` | Enter a folder called `T` (or `P`) |

`$==path` (also `$:==path`, `$0:==path`) always means a path, even without a `/` or an image suffix. A single `=`, as in `$=T` / `$=P`, stays a
CBM listing modifier and is not a path. With an image mounted, a bare name is a file inside it, and so is a name with a `/` in it (`A/B` is a legal CBM file name). Only a leading `/`,
a component with an image suffix (`X.D64/`) or `..` makes it a path; leave the image with `CD:←`.

### Device numbers
The KERNAL image file contains no device numbers, so one image serves every setup (the firmware writes the banner's numbers into the copy it serves, see below). Device number assignment is automatic by default: on the first disk access of a run (the first `LISTEN` / `TALK` / `LOAD` / `SAVE`, so after the program has been
loaded) the KERNAL probes the real serial bus at 8, 9, 10, ... and the SD drive takes the first free number, the USB drive the next
(only units whose card / stick is present). With no other drive attached that is 8 and 9, and the probe costs a few milliseconds; with a
physical drive at 8 the SD drive becomes 9, and so on.

The power-on banner shows the numbers the scan is going to give: the KERNAL does the same probe at power-on, on the real bus only (the
mailbox is not live yet then), for the automatic units the firmware tells it about. `PEEK(56854)` (SD) and `PEEK(56855)` (USB) show the current numbers (0 = none;
defaults until the first disk access), the directory header id shows it too (`0 "SD:/" 09 2A`), and the serial console prints the numbers
at start-up. Nothing is probed when nothing needs a number (no card, or both numbers fixed). A drive that has not powered up yet when the
first access happens looks like a gap, so fix the numbers if some device boots slowly (e.g. a Wi-Fi drive).

To fix a number, use the same command real drives use (stored in EEPROM; 0 or an invalid value = automatic):
```
OPEN15,8,15,"U0>"+CHR$(10):CLOSE15     the SD drive is now device 10 (stored immediately; 8 goes back to the real bus)
OPEN15,9,15,"U0>"+CHR$(11):CLOSE15     the USB drive becomes 11
OPEN15,10,15,"U0>"+CHR$(0):CLOSE15     CHR$(0) makes that unit automatic again
```
The accepted range is 8-30 (a number outside that range, or the other unit's number will give status `33,SYNTAX ERROR`). A fixed number is never handed out to the other
unit by the scan; every other device number keeps using the real IEC code. However, if you set one of the VDrives to the same number as a real device, the real device will be hidden and inaccessible.

### Images and mount points
Image formats: `.d64` (35/40 tracks), `.d71` (1571, 70 tracks) and `.d81` (1581; root directory only, no sub-directories) are recognised by file size and extension; files may carry the trailing error-info bytes, but they are ignored (no read errors 21-29 come from the image).
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
| `MD:NAME` / `RD:NAME` | With nothing mounted: make / remove (empty) folders on the card. With an image mounted (D81 partitions): not supported |
| `C:new=old` copy | Also joins files: `C:new=a,b,c` (up to 8); the new file has the first source's type |
| Direct sector access | `OPEN n,8,n,"#"` takes a 256-byte buffer channel; on channel 15 `U1:n 0 t s` (or `UA`) reads a sector into it, `U2`/`UB` writes it back (`B-R`/`B-W` do the same as `U1`/`U2`), `B-P:n,pos` sets the buffer pointer, `B-A`/`B-F` allocate and free a sector in the BAM (`B-A` on a used sector answers `65` with the next free one). D64, D71 and D81; needs an image mounted |

### Not yet supported
| Feature | Notes |
|---|---|
| Writing plain files to the card (`SAVE` with nothing mounted) | Mount an image (or create one with `N:NAME.D64,ID`) |
| File names longer than 16 characters | Hidden in host listings |
| Running programs with their own fastloaders | No `M-R`/`M-W`/`M-E`, so programs that upload code to the drive (or probe its ROM to detect the drive type) cannot run |
| `U3`-`U8` user jumps, `B-E` | Need drive code |
| Copy protection that checks for bad sectors | Error-info bytes in images are ignored |

## Utilities

### Copying files between drives: `VDCOPY`
`make util` builds `build/util/vdcopy.prg` (pure machine code behind a `SYS` stub, loads at `$0801`; it is also on `vdtest.d64`).
It copies between any two units, in either direction: real drive to SD/USB, SD/USB to a real drive, or SD to USB (an image mounted on each). Put it on the card, load it from the SD drive and `RUN` it (with the real drive and its disk on the bus, if you use one):
```
LOAD"VDCOPY",9 : RUN        (9 when a real drive is 8; the SD drive is 8 when there is none)
```
It asks for the source unit (default 8) and the destination unit (default: the SD drive, `PEEK(56854)`; type both numbers for any other combination, e.g. 8 and 9 for SD to USB), checks that both answer, lists the
source disk (PRG, SEQ and USR files, 20 lines at a time) and asks what to copy: `A` for all, or numbers such as `1,3,5-8`. Existing files on
the destination are skipped, or replaced if you answer `Y` to "replace existing". Copies go 2 KB at a time (read from the source, then written
to the destination), the destination's error channel is checked after every chunk, so a full disk (72) or a failed write stops that file and
removes the partial copy. RUN/STOP stops after the current chunk. A vdrive source or destination needs an image mounted (`CD:`). Hardware-verified: vdrive to vdrive and vdrive to a 1541 (that was the earlier BASIC version; the machine-code rewrite is checked in VICE for all directions, not yet on hardware). The byte copy itself goes through the KERNAL one byte at a time, so its speed is set by the drives; the machine-code version removes the BASIC overhead in reading the directory and per file. Uses `$0801-$0FFF`, tables at `$2000-$37FF` and the buffer `$C000-$C7FF`.
REL files are not shown; names with `* , : = ?` are skipped.

### VDrive Config (the list editor)
`VDrive Config.prg` (built by `make util` into `build/util/`; copy it to the card and start it from the TeensyROM menu with the VDrive handler) edits the Alt-button lists (see 'Changing the disk with the alt button' above) on the C64 and shows / changes what is mounted:
```
RETURN mount the entry   B browse the card, mount any image   U unmount
A add (browse, colour, flashes)   E edit   D delete   +/- move down/up
T switch SD / USB list   R refresh   Q quit        (left arrow cancels an entry field)
```
The top lines show the image mounted on each drive; `*` marks the list entry that is mounted. Changes to the list are saved immediately in `vdrive-config.txt`. Details: `docs/DESIGN.md` section 23.

## Building and testing
Needs `cc65` (`ca65`/`ld65`), VICE (`x64sc`, `c1541`), a C/C++ compiler and Python 3.

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

## License
MIT License, see [LICENSE](LICENSE), the same license as the TeensyROM firmware this patches. A firmware build combines both, so keep both copyright notices when distributing one.
