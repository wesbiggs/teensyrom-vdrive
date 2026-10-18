# TeensyROM+ integration: "VDrive KERNAL"

Turns the SD card and the USB drive into virtual IEC drives backed by `.d64` / `.d71` / `.d81` images, using the TR+ live
KERNAL replacement. They answer as devices 8 (SD) and 9 (USB), or the next free numbers when real drives are on the bus. Status: **host-tested only** — see "What is and isn't verified" at the end.


## VDrive+SwiftLink
`IOH_VDriveSwift.c` ("VDrive+SwiftLink") runs the virtual drive and the SwiftLink / Turbo232 modem together: IO1 `$DE00-$DE0F` goes to the
SwiftLink handler, `$DE10-$DE17` to the drive mailbox (this is why the mailbox moved off `$DE00`), IO2 to the drive window, the per-cycle
callback to SwiftLink, and init/polling run both. When the default Special IO (EEPROM) is VDrive or VDrive+SwiftLink, a built-in menu program
paired with plain SwiftLink (CCGMS, Retromate, Simple SwiftLink Term) is started with this handler instead (`VDriveSwift_Remap()` in
`KERNALPreStart()` and `IOHandlerSelectInit()`), so launching it does not drop the drive. `.crt` SwiftLink/Turbo232 images are left alone:
they run with the C64's own KERNAL. The handler pays SwiftLink's normal start-up cost (about 355 KB of RAM2 buffers, drive menu freed), so it
is a separate Special IO rather than part of "VDrive KERNAL".

## Install
1. Build the patched KERNAL from your own 901227-03 dump (the ROM itself is not in this repo):
   ```
   make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin     # 8192 bytes, mailbox at $DE10, window at $DF00
   ```
   Copy it to the SD card or USB drive.
2. Patch the firmware source (TeensyROM checkout, FW ≥ 0.9 layout) and build the TR+ target:
   ```
   cd /path/to/TeensyROM && git apply /path/to/c64exp/firmware/teensyrom-vdrive.patch
   ```
   The patch adds `Fab04_VDrive` (enabled next to `Fab04_KernalReplace` in `Common_Defs.h`), a new Special IO
   `IOH_VDrive` ("VDrive KERNAL") and `IOH_VDriveSwift` ("VDrive+SwiftLink", see below), the library under `MinimalBoot/Common/vdrive/`, and one condition in
   `KERNALPreStart()` so the KERNAL image is loaded when this Special IO is selected.
3. On the C64: in the TR file browser select the image and press `<Shift-K>`; in Settings choose Special IO
   **VDrive KERNAL** (`a`/`A`); then start BASIC (`F2`) or launch a PRG.

## Use
```
LOAD"$",8                      lists the SD card (or the mounted image)
OPEN15,8,15,"CD:GAME.D64"      mount an image (CD:SUBDIR / CD:.. move around; CD:_ unmounts; CD:/ unmounts and goes to the card root)
OPEN15,8,15,"CD:/GAMES/G.D81"  paths work: absolute (leading /) or relative, folders and an image in one go
LOAD"$",8 : LIST               directory of the image;  LOAD"NAME",8,1   SAVE"@0:NAME",8   etc.
OPEN15,8,15,"S:OLD*"           scratch;  "N:NAME,ID" format;  "R:NEW=OLD" rename;  "UI" or "UJ" reset (status 73)
```
Paths also work inside names. A directory listing by path only *lists*: nothing stays selected afterwards (the folder and image selected
before are restored when the listing is closed, and `$==` below is what selects):
```
LOAD"$/GAMES/GAME.D64",8       list the image (so does LOAD"GAME.D64",8 when nothing is mounted)
LOAD"$GAMES/",8                list a folder;  LOAD"$/GAMES/GAME.D64/S*",8   only the matching files of the image
LOAD"GAMES/GAME.D64/START",8,1 load a file from the image: this one leaves the folder and image selected, so the program's own LOADs find its files
```
`$==path` (also `$:==path`, `$0:==path`) does the `CD:` and lists: it always means a path (even without a `/` or an image suffix) and the folder / image
stay selected. `LOAD"$==GAMES",8` enters the folder, `LOAD"$==DEMO.D64",8` mounts the image, `LOAD"$==/GAMES/DEMO.D64/S*",8` mounts it and lists
matching files, and a folder called `T` or `P` is
`$==T` (a single `=`, as in `$=T` / `$=P`, stays a CBM listing modifier and is not a path).
With an image mounted, a bare name is a file inside it; leave with `CD:_` (or use a path containing a `/`).

Image formats: `.d64` (35/40 tracks), `.d71` (1571, 70 tracks) and `.d81` (1581; root directory only, no sub-directories) are recognised by file size and extension; files may carry the trailing error-info bytes.
Opening a .d64 in the TeensyROM menu and launching a program from it mounts that image automatically on device 8 (SD) or 9 (USB), so the program's own LOADs find its files; `CD:_` unmounts it. The last mounted image of each drive is also remembered in a hidden `/.vdrive` file on that card and mounted again at the next start (any unmount forgets it).
The USB drive works the same. Any other device number uses the real IEC bus untouched.
Supported: LOAD/SAVE (fast path, ~255 B per mailbox transaction), VERIFY and OPEN/PRINT#/INPUT# (byte path),
PRG/SEQ/USR, wildcards, `@0:` replace, error channel, directory with `$:pattern`.
Not supported: REL, append (`,A`), `C` copy, `V` validate (accepted, no-op), loading plain `.prg` files straight from the
card, file names longer than 16 characters (hidden in host listings), tape (device 1, see below).

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
`eepMagicNum` is not bumped because 0/0xFF/garbage all mean automatic. Upgrading from 0.9.0.3: a number you set with `U0>` is
now a fixed number (an explicit 8 stays 8).

## Changing the disk while a program runs (remote control)
With the VDrive handler active, the TeensyROM's remote-control port (TCP 2112, or the USB serial port) accepts two extra commands that work while the C64 runs a program: `0x64E3` reads what a unit has mounted, `0x64E4` mounts an image (or unmounts with an empty path). The swap waits for a quiet moment on the C64's drive, closes the unit's open channels and mounts, like `CD:`. Details and framing are in `docs/DESIGN.md` section 20. The `trvdrive` tool in `teensyrom-rcp` wraps them:

```
trvdrive -H tr.local                      # show SD and USB
trvdrive -H tr.local sd /GAMES/DISK2.D64  # mount
trvdrive -H tr.local sd -u                # unmount
```

## What changed in the KERNAL (`patches/vdrive.s`)
* The 8 serial entry points (`LISTEN TALK SECOND TKSA CIOUT ACPTR UNLSN UNTLK`) `JMP` into stubs that ask the mailbox
  whether the transaction is for one of the virtual drives; "not mine" replays the displaced instructions into the stock routine.
* `LOAD` (`$F4C8`) and `SAVE` (`$F605`) get block-transfer fast paths (any device is offered to the Teensy first; ~one extra mailbox round trip when the device is a real drive); `VERIFY` takes the stock path.
* The first LISTEN / TALK / LOAD / SAVE of a run may be answered with the token `$3C`: `vcall` then runs the bus scan (`bus_scan`, ~100 bytes) and repeats the command.
* The mailbox answer is a token (`$5A` / `$A5`); a bad or missing answer in the middle of a transfer aborts it with an error. ~1 s mailbox time-out at the start of a transaction falls back to the stock IEC code, so the image is harmless if the handler isn't active.
* Space comes from the cassette bit-timing code `$F8E2-$FB8D`: **tape I/O (device 1) no longer works.**

See [docs/BRINGUP.md](../docs/BRINGUP.md) for the staged hardware bring-up plan (build `-DVD_TRACE` for a per-command serial trace).

## What is and isn't verified
Verified on the host (`make test`): the D64 / D71 / D81 engine against `c1541` (contents, BAM equality with `c1541 -validate`,
formats), the bus scan in VICE with true-drive 1541s at 8 and 10 (the SD drive lands on 9, the USB drive on 11), the register-level engine, the 6502 patches in VICE (via a RAM mailbox + monitor-driven mock), and
`IOH_VDrive.c` compiled as C++ with `-Wall -Wextra -Werror` against **host stubs** of the Teensy/TeensyROM API,
driving real files through the IO1/IO2 entry points.

**Not verified:** the bus scan on a real C64 with real drives (it assumes the VDrive handler is not yet active during BASIC init, taken from the TeensyROM sources, and that real drives behave like VICE's emulation), D71/D81 on the card, a build against the real Teensy 4.1 toolchain (no `arduino-cli` was available), anything on real
hardware, bus timing of the IO1/IO2 handlers, real USB/SD media behaviour, IRQ/STOP-key interplay during loads,
RAM1 budget (the drive state is now ~4.6 KB and lives in RAM2 via `DMAMEM`, so RAM1 only pays for ~130 bytes of ISR-side code plus the
glue's small tables; `-DVD_ALLOC_HEAP` selects the malloc heap instead, `VD_NSLOT` in `d64/vd_cfg.h` trades open channels for RAM; the
actual link result is not known). Also unconfirmed: whether `IO2`'s `$DF00` window is free while this Special IO is active
(it should be: handlers are exclusive), and that `FILE_WRITE` + `seek()` gives random-access writes on both file systems.
