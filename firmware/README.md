# TeensyROM+ integration: "VDrive KERNAL"

Turns SD (device 8) and USB (device 9) into virtual IEC drives backed by `.d64` images, using the TR+ live
KERNAL replacement. Status: **host-tested only** — see "What is and isn't verified" at the end.

## Install
1. Build the patched KERNAL from your own 901227-03 dump (the ROM itself is not in this repo):
   ```
   make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin     # 8192 bytes, mailbox at $DE00, window at $DF00
   ```
   Copy it to the SD card or USB drive.
2. Patch the firmware source (TeensyROM checkout, FW ≥ 0.9 layout) and build the TR+ target:
   ```
   cd /path/to/TeensyROM && git apply /path/to/c64exp/firmware/teensyrom-vdrive.patch
   ```
   The patch adds `Fab04_VDrive` (enabled next to `Fab04_KernalReplace` in `Common_Defs.h`), a new Special IO
   `IOH_VDrive` ("VDrive KERNAL"), the library under `MinimalBoot/Common/vdrive/`, and one condition in
   `KERNALPreStart()` so the KERNAL image is loaded when this Special IO is selected.
3. On the C64: in the TR file browser select the image and press `<Shift-K>`; in Settings choose Special IO
   **VDrive KERNAL** (`a`/`A`); then start BASIC (`F2`) or launch a PRG.

## Use
```
LOAD"$",8                      lists the SD card (or the mounted image)
OPEN15,8,15,"CD:GAME.D64"      mount an image (CD:SUBDIR / CD:.. move around; CD:_ unmounts; CD:/ unmounts and goes to the card root)
LOAD"$",8 : LIST               directory of the image;  LOAD"NAME",8,1   SAVE"@0:NAME",8   etc.
OPEN15,8,15,"S:OLD*"           scratch;  "N:NAME,ID" format;  "R:NEW=OLD" rename
```

Opening a .d64 in the TeensyROM menu and launching a program from it mounts that image automatically on device 8 (SD) or 9 (USB), so the program's own LOADs find its files; `CD:_` unmounts it. The last mounted image of each drive is also remembered in a hidden `/.vdrive` file on that card and mounted again at the next start (any unmount forgets it).
Device 9 works the same on the USB drive. Any other device number uses the real IEC bus untouched.
Supported: LOAD/SAVE (fast path, ~255 B per mailbox transaction), VERIFY and OPEN/PRINT#/INPUT# (byte path),
PRG/SEQ/USR, wildcards, `@0:` replace, error channel, directory with `$:pattern`.
Not supported: REL, append (`,A`), `C` copy, `V` validate (accepted, no-op), loading plain `.prg` files straight from the
card, file names longer than 16 characters (hidden in host listings), tape (device 1, see below).

## Changing the device numbers (default 8 = SD, 9 = USB) — no rebuild, no reflash
The numbers live in EEPROM (2 bytes, `eepAdVDriveDevs`; 0 or an invalid value means "default") and are changed from BASIC with the
same command real drives use:
```
OPEN15,8,15,"U0>"+CHR$(10):CLOSE15     the SD drive is now device 10 (stored immediately; 8 goes back to the real bus)
OPEN15,9,15,"U0>"+CHR$(11):CLOSE15     the USB drive becomes 11
OPEN15,10,15,"U0>"+CHR$(0):CLOSE15     CHR$(0) restores that unit's default
```
Accepted range 8-30, and not the other unit's number (otherwise status `33,SYNTAX ERROR`). `LOAD"$",<dev>` with nothing mounted shows
the current number in the header (`0 "SD:/" 10 2A`). The firmware also prints it at start-up on the serial console. Pick numbers no
physical drive on the bus uses; every other device number keeps using the real IEC code. The KERNAL image contains no device numbers, so
one image serves every setup. Build-time defaults are `VD_DEV0`/`VD_DEV1` in `d64/vd_cfg.h`; `vdrive_set_devices()` is the C API.
The EEPROM bytes are carved out of the reserved area (`eepAdNext` moves from 4235 to 4237, reset-to-defaults zeroes them); `eepMagicNum`
is not bumped because 0/0xFF/garbage all fall back to the defaults.

## What changed in the KERNAL (`patches/vdrive.s`)
* The 8 serial entry points (`LISTEN TALK SECOND TKSA CIOUT ACPTR UNLSN UNTLK`) `JMP` into stubs that ask the mailbox
  whether the transaction is for device 8/9; "not mine" replays the displaced instructions into the stock routine.
* `LOAD` (`$F4C8`) and `SAVE` (`$F605`) get block-transfer fast paths (any device is offered to the Teensy first; ~one extra mailbox round trip when the device is a real drive); `VERIFY` takes the stock path.
* The mailbox answer is a token (`$5A` / `$A5`); a bad or missing answer in the middle of a transfer aborts it with an error. ~1 s mailbox time-out at the start of a transaction falls back to the stock IEC code, so the image is harmless if the handler isn't active.
* Space comes from the cassette bit-timing code `$F8E2-$FB8D`: **tape I/O (device 1) no longer works.**

See [docs/BRINGUP.md](../docs/BRINGUP.md) for the staged hardware bring-up plan (build `-DVD_TRACE` for a per-command serial trace).

## What is and isn't verified
Verified on the host (`make test`): the D64 engine against `c1541` (contents, BAM equality with `c1541 -validate`,
formats), the register-level engine, the 6502 patches in VICE (via a RAM mailbox + monitor-driven mock), and
`IOH_VDrive.c` compiled as C++ with `-Wall -Wextra -Werror` against **host stubs** of the Teensy/TeensyROM API,
driving real files through the IO1/IO2 entry points.

**Not verified:** a build against the real Teensy 4.1 toolchain (no `arduino-cli` was available), anything on real
hardware, bus timing of the IO1/IO2 handlers, real USB/SD media behaviour, IRQ/STOP-key interplay during loads,
RAM1 budget (the drive state is now ~4.6 KB and lives in RAM2 via `DMAMEM`, so RAM1 only pays for ~130 bytes of ISR-side code plus the
glue's small tables; `-DVD_ALLOC_HEAP` selects the malloc heap instead, `VD_NSLOT` in `d64/vd_cfg.h` trades open channels for RAM; the
actual link result is not known). Also unconfirmed: whether `IO2`'s `$DF00` window is free while this Special IO is active
(it should be: handlers are exclusive), and that `FILE_WRITE` + `seek()` gives random-access writes on both file systems.
