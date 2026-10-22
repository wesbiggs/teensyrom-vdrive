# TeensyROM+ Virtual Drive Support

Make the SD card and the USB drive of a [TeensyROM+](https://github.com/SensoriumEmbedded/TeensyROM) behave as IEC disk drives on a
Commodore 64, backed by `.d64`, `.d71` and `.d81` images. No IEC cable, no additional hardware requirement.

It has two halves:

* **A patched KERNAL** (901227-03 plus a few hundred bytes of 6502, served live by the TR+). The serial routines
  (`LISTEN TALK SECOND TKSA CIOUT ACPTR UNLSN UNTLK`) ask a mailbox in I/O space (`$DE10`, with a 256-byte window at `$DF00`) whether the
  device is virtual. If so the Teensy answers; otherwise the stock IEC code runs, so real drives keep working. LOAD and SAVE have
  block-transfer fast paths.
* **Teensy firmware** (`firmware/`, a patch for the TeensyROM source) with a portable C99 D64/D71/D81 engine (`d64/`) and a virtual drive on top of it:
  channels, the command channel, directory listings, write support, `CD` into folders and images.

## Status
Works on a real C64 with a TR+ for the core path (directory, LOAD, SAVE, VERIFY, error channel); newer features are tested on the host and
in VICE only. See the Status section of [CLAUDE.md](CLAUDE.md) for exactly what has and has not been checked on hardware.
The C128 is not yet supported, and tape (device 1) no longer works because its KERNAL code space was reused.

## Installing
1. Build the patched KERNAL from your own 901227-03 dump (the ROM itself is not in this repo):
   ```
   make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin     # 8192 bytes, mailbox at $DE10, window at $DF00
   ```
   Copy it to the SD card or USB drive.
2. Patch the firmware source (TeensyROM checkout, FW ≥ 0.9 layout) and build the TR+ target:
   ```
   cd /path/to/TeensyROM && git apply /path/to/teensyrom-vdrive/firmware/teensyrom-vdrive.patch
   ```
   The patch adds `Fab04_VDrive` (enabled next to `Fab04_KernalReplace` in `Common_Defs.h`), a new Special IO
   `IOH_VDrive` ("VDrive KERNAL") and `IOH_VDriveSwift` ("VDrive+SwiftLink"), the library under `MinimalBoot/Common/vdrive/`, and one condition in
   `KERNALPreStart()` so the KERNAL image is loaded when this Special IO is selected.
3. On the C64: in the TR file browser select the image and press `<Shift-K>`; in Settings choose Special IO
   **VDrive KERNAL** (`a`/`A`); then start BASIC (`F2`) or launch a PRG.


The "VDrive+SwiftLink" Special IO runs the virtual drive and the SwiftLink / Turbo232 modem together. When the default Special IO is VDrive or VDrive+SwiftLink, menu programs paired with plain SwiftLink (CCGMS, Retromate, Simple SwiftLink Term) start with it, so launching one does not drop the drive. `.crt` SwiftLink/Turbo232 images are left alone and run with the C64's own KERNAL. It costs SwiftLink's normal start-up RAM (about 355 KB of RAM2 buffers, drive menu freed), so it is a separate Special IO.

## Using it

### Commands

By default, device numbers are automatic (SD and USB take the first free numbers from 8 up); they can be changed/fixed with `U0>` (see below).

The examples here use `8` as an illustration only, and work the same on both SD and USB.

| Command | What it does |
|---|---|
| `LOAD"$",8 : LIST` | List the mounted image (or current folder if none mounted) |
| `LOAD"NAME",8` | Load a BASIC file |
| `LOAD"NAME",8,1` | Load a binary file |
| `SAVE"NAME",8` | Save a BASIC file |
| `SAVE"@0:NAME",8` | Save a BASIC file, replacing an existing one |
| `VERIFY"NAME",8` | Verify a BASIC file |
| `OPEN15,8,15,"CD:GAME.D64"` | Mount a disk image (`.d64`/`.d71`,`.d81` supported) |
| `OPEN15,8,15,"CD:SUBDIR"` | Move into a folder |
| `OPEN15,8,15,"CD:.."` | Move up one level |
| `OPEN15,8,15,"CD:_"` | Unmount the image |
| `OPEN15,8,15,"CD:/"` | Unmount and move to the folder root |
| `OPEN15,8,15,"CD:/GAMES/G.D81"` | Paths work: absolute (leading `/`) or relative, folders and an image in one go |
| `OPEN15,8,15,"S:OLD*"` | Scratch |
| `OPEN15,8,15,"N:NAME,ID"` | Format the mounted disk |
| `OPEN15,8,15,"R:NEW=OLD"` | Rename a file |
| `OPEN15,8,15,"UI"` / `"UJ"` | Reset (status 73) |

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
CBM listing modifier and is not a path. With an image mounted, a bare name is a file inside it; leave with `CD:_` (or use a path containing a `/`).

### Images and drives
Image formats: `.d64` (35/40 tracks), `.d71` (1571, 70 tracks) and `.d81` (1581; root directory only, no sub-directories) are recognised by file size and extension; files may carry the trailing error-info bytes.
Opening a `.d64` in the TeensyROM menu and launching a program from it mounts that image automatically on device 8 (SD) or 9 (USB), so the program's own LOADs find its files; `CD:_` unmounts it. The last mounted image of each drive is also remembered in a hidden `/.vdrive` file on that card and mounted again at the next start (any unmount forgets it).
The USB drive works the same. Any other device number uses the real IEC bus untouched.

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
| `C:new=old` copy | Also joins files: `C:new=a,b,c` (up to 8); the new file has the first source's type |

### Not yet supported
| Feature | Notes |
|---|---|
| `V` validate | Accepted, does nothing |
| Plain `.prg` files loaded straight from the card | Mount an image instead |
| File names longer than 16 characters | Hidden in host listings |
| Tape (device 1) | Its code space was reused by the patch |

## Copying files between drives: `VDCOPY`
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

## Device numbers: automatic (default 8 = SD, 9 = USB) or fixed — no rebuild, no reflash
Automatic by default: on the first disk access of a run (the first `LISTEN` / `TALK` / `LOAD` / `SAVE`, so after the program has been
loaded) the KERNAL probes the real serial bus at 8, 9, 10, ... and the SD drive takes the first free number, the USB drive the next
(only units whose card / stick is present). With no other drive attached that is 8 and 9, and the probe costs a few milliseconds; with a
physical drive at 8 the SD drive becomes 9, and so on. `PEEK(56854)` (SD) and `PEEK(56855)` (USB) show the current numbers (0 = none;
defaults until the first disk access), the directory header id shows it too (`0 "SD:/" 09 2A`), and the serial console prints the numbers
at start-up. Nothing is probed when nothing needs a number (no card, or both numbers fixed). A drive that has not powered up yet when the
first access happens looks like a gap, so fix the numbers if some device boots slowly (e.g. a Wi-Fi drive).

To fix a number, use the same command real drives use (stored in EEPROM, 2 bytes `eepAdVDriveDevs`; 0 or an invalid value = automatic):
```
OPEN15,8,15,"U0>"+CHR$(10):CLOSE15     the SD drive is now device 10 (stored immediately; 8 goes back to the real bus)
OPEN15,9,15,"U0>"+CHR$(11):CLOSE15     the USB drive becomes 11
OPEN15,10,15,"U0>"+CHR$(0):CLOSE15     CHR$(0) makes that unit automatic again
```
Accepted range 8-30, and not the other unit's number (otherwise status `33,SYNTAX ERROR`). A fixed number is never handed out to the other
unit by the scan; every other device number keeps using the real IEC code. The KERNAL image contains no device numbers, so one image
serves every setup. Build-time defaults of the automatic numbers are `VD_DEV0`/`VD_DEV1` in `d64/vd_cfg.h`; `vdrive_set_devices()` is the
C API. The EEPROM bytes are carved out of the reserved area (`eepAdNext` moves from 4235 to 4237, reset-to-defaults zeroes them);
`eepMagicNum` is not bumped because 0/0xFF/garbage all mean automatic.

## VDrive Config (the list editor)
`VDrive Config.prg` (built by `make util` into `build/util/`; copy it to the card and start it from the TeensyROM menu with the VDrive handler) edits the Alt-button lists (next section) on the C64 and shows / changes what is mounted:
```
RETURN mount the entry   B browse the card, mount any image   U unmount
A add (browse, colour, flashes)   E edit   D delete   +/- move down/up
T switch SD / USB list   R refresh   Q quit        (left arrow cancels an entry field)
```
The top lines show the image mounted on each drive; `*` marks the list entry that is mounted. Changes to the list are saved immediately in `vdrive-config.txt`. Details: `docs/DESIGN.md` section 23.

## Changing the disk with the Alt button
Without a computer: set the TR+ Alt button action to **None** (TR settings), and list the images you want in `vdrive-config.txt` in the root of the SD card (and, for the USB stick, in the root of the stick):
```
# path on that drive, border colour (0-15), number of flashes
/GAMES/ZORK.D64, 5, 1
/GAMES/BIG DISK.D81, 3, 2
/UTIL/TOOLS.D64
```
While a program runs with the VDrive handler: one tap = next image of the SD list (the first if nothing is mounted), two taps within one second = next image of the USB list. The border flashes the chosen colour that many times (250 ms on, 750 ms off) once the disk is changed; a missing card/stick, a missing, empty or unusable list flashes red for 2 s. Images that no longer exist are skipped. The change waits for a quiet moment on the drive, like `trvdrive`. Details: `docs/DESIGN.md` section 22.

## Changing the disk while a program runs (remote control)
With the VDrive handler active, the TeensyROM's remote-control port (TCP 2112, or the USB serial port) accepts two extra commands that work while the C64 runs a program: `0x64E3` reads what a unit has mounted, `0x64E4` mounts an image (or unmounts with an empty path). The swap waits for a quiet moment on the C64's drive, closes the unit's open channels and mounts, like `CD:`. Details and framing are in `docs/DESIGN.md` section 20. Directory listings (`trls`) are also allowed while a program runs, after the same wait, so you can browse for an image; the other file commands (`trget`, `trput`, ...) still answer `Busy!`. The `trvdrive` tool in `teensyrom-rcp` wraps get and set:

```
trvdrive -H tr.local                      # show SD and USB
trvdrive -H tr.local sd /GAMES/DISK2.D64  # mount
trvdrive -H tr.local sd -u                # unmount
```

With the TR menu showing (no program running) the same commands show and change the image each unit will mount at the next start (the `/.vdrive` file on its card; the reply says `VDrive not running`). A program launched from a disk image opened in the menu still mounts that image instead.


## Building and testing
Needs `cc65` (`ca65`/`ld65`), VICE (`x64sc`, `c1541`), a C/C++ compiler and Python 3.

```bash
make test
```

The KERNAL image is built from your own stock ROM, so no Commodore ROM bytes are in this repository:

```bash
make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin
```


## Documentation
* [firmware/README.md](firmware/README.md): how the firmware glue and the KERNAL patches work.
* [docs/DESIGN.md](docs/DESIGN.md): the design, one section per milestone, including alternatives considered (section 19).
* [CLAUDE.md](CLAUDE.md): project notes, status and gotchas for contributors.

## License
MIT License, see [LICENSE](LICENSE), the same license as the TeensyROM firmware this patches. A firmware build combines both, so keep both copyright notices when distributing one.
