# CLAUDE.md — TR+ virtual-drive KERNAL

## What this is
A replacement C64 KERNAL (901227-03 + patches) that the **TeensyROM+ (TR+)** serves live, plus the Teensy-side firmware that makes
the SD card and the USB drive behave as IEC drives backed by `.d64` / `.d71` / `.d81` images. KERNAL serial calls for those devices go to
a mailbox (IO1 `$DE10` registers + a 256-byte IO2 window at `$DF00`) instead of the bit-banged bus; any other device number still uses the
real IEC code, so physical drives keep working. LOAD/SAVE have block-transfer fast paths. Device numbers are automatic (SD first, USB
second, the first free numbers from 8 up: the KERNAL probes the real bus on the first disk access of a run) or fixed from BASIC with
`OPEN15,8,15,"U0>"+CHR$(n)` (kept in EEPROM, `CHR$(0)` = automatic, no reflash). Names and `CD` take paths: `LOAD"$/GAMES/G.D64",8`.

Full design and per-milestone notes: `DESIGN.md` (sections 1-14, in order of construction). Hardware plan: `docs/BRINGUP.md`.
Firmware integration and usage: `firmware/README.md`.

## Status (read this first)
* **Host-verified only.** `make test` passes: C unit tests via ctypes against `c1541`, the 6502 patches in VICE through a
  monitor-driven mailbox mock, and the firmware glue compiled as C++ against host stubs.
* **Never built against the real Teensy toolchain, never run on a TR+** as of the first commit. The owner is doing the first build
  and flash now (toolchain: `arduino-cli`, `teensy:avr@1.61.0`, `CRC32@2.0.1`; do NOT use Teensyduino 1.62.0 — SD read stalls).
  Follow `docs/BRINGUP.md` stage by stage and record results in its table; update this section as stages pass.
* **First version works on hardware (real C64, TR+ + this patch).** Verified by the owner: build/fit (RAM1 padding −976 B, flash +15.7 KB,
  RAM2 +4.5 KB vs stock), handler listing and SD/USB detection, mailbox registers, stock and banner KERNAL control (banner shows
  `TRVDRIVE 64 BASIC V2` on F2), then the real image: mounting a `.d64` with `CD:`, directory/`LOAD` of small and 6000-byte files, `SAVE`,
  `VERIFY`, the error channel, and repeated directory listings without corruption. Not yet checked on hardware: device 9 (USB stick),
  `U0>` renumbering and its EEPROM persistence, `CD:/`, the no-handler time-out, soak tests, the scope stage, sequential files, scratch /
  rename / `N:`.
* **BASIC test programs** (`tests/basic/`, built into `build/testdisk/vdtest.d64` by `make testdisk`) run inside VICE in `make test` and on the C64; they
  found two real bugs that unit tests missed: `CIOUT` must return `A` = the byte sent (CHROUT tail-jumps into it; BASIC's PRINT broke after a
  number), and `U0>` must also be accepted with PRINT#'s trailing CR.
* **Lessons from bring-up (keep):** (1) a flat NTSC C128 in C64 mode did not reliably serve the replacement KERNAL: use a real C64, and
  verify "is it really served" with the banner image or `PEEK(58483+I)` at `$E473` (a stock image can't show it). (2) All SD/USB probing
  must happen in `VDrive_PreStart()` while the C64 waits, not at handler start. (3) Bus-handler data in fast RAM1, token STAT handshake,
  abort on a failed block (see Gotchas). (4) `FILE_WRITE` creates missing files on SD/USB: always probe with `FILE_READ` first.
  (5) `npm run flash` needs the Teensy program button (the TR exposes no soft reboot); copying the `.hex` to the card and pressing
  RETURN on it in the TR menu is easier. (6) The TeensyROM checkout may hold the owner's own edits (`TeensyROM.h`, `old*.hex`): never
  `git stash -u`/`clean` it; restore only the patch-owned files and re-apply `firmware/teensyrom-vdrive.patch`.
* **VDrive+SwiftLink (firmware 0.9.0.4, host-tested only):** mailbox moved to `$DE10` so the SwiftLink ACIA keeps `$DE00-$DE07`; new Special IO
  "VDrive+SwiftLink" (`firmware/IOH_VDriveSwift.c`) runs both, and menu programs paired with SwiftLink (CCGMS...) get it when VDrive is the
  default Special IO. Builds with the real toolchain (`node tools/build-firmware.mjs --target tr-plus`, RAM1 padding unchanged at 12,840). **KERNAL images built before this change (mailbox `$DE00`) no longer match the firmware: rebuild and re-copy
  `kernal-tr.bin`.** Check on hardware: BRINGUP.md stage 8.
* **0.9.0.7 (host + VICE tested only, nothing of it on hardware):** automatic device numbers by a lazy bus scan (SCANREQ token `$3C`, `bus_scan` in
  `patches/vdrive.s`, `VIO_SCAN`/`VIO_FREE`, `DEV0`/`DEV1` registers `$DE16/$DE17`; DESIGN.md 16), `.d71`/`.d81` images (17), paths in `CD` and
  in names (18): `LOAD"$/dir/image.d64"` only lists (selection restored on close), `LOAD"$==path"` selects like `CD`, `UI`/`UJ`/`U9` (status 73), `U1`.. now `31`. A `U0>` number is a *fixed* number now (0 = automatic);
  `vdrive_set_devices()` takes pins. To check on hardware: the scan with a real drive at 8 (SD must become 9; `PEEK(56854)`), a first access that is a
  `LOAD`, the time of the first access, `.d81` mounting on SD, paths.
* Known open risks:
  C128 not supported (see above); `File::seek/read/write/flush` semantics on the
  real SD/USB classes, IO1/IO2 ISR timing, tape (device 1) intentionally broken (its code space was reclaimed).

* **Auto-mount (firmware 0.9.0.2, host-tested only; remembered mounts are 0.9.0.3 and also host-only):** if a program is launched from inside a `.d64` opened in the TR menu
  (`DriveDirPath` ends in `*`, menu source SD/USB), `VDrive_PreStart()` mounts that image on the matching unit (SD=8, USB=9) with its
  folder as the `CD:` directory. Not yet checked on hardware.
* **Remembered mounts (0.9.0.3):** each unit keeps `/.vdrive` (dot-named, so TR 0.9+ and our listing hide it) on its own card, holding the last
  mounted image path. Written on every successful mount, removed on any unmount (`CD:_`, `CD:/`), read in `VDrive_PreStart()` when the
  launch did not come from a disk image on that unit. A missing/invalid image just starts unmounted. Not yet checked on hardware.

## Layout
```
d64/            portable C99 library, also compiled as C++ by the firmware (keep it C++-clean!)
  d64.c/.h        D64/D71/D81 engine: geometry, BAM, directory, file chains, create/write/scratch/rename/format, BASIC listing builder
  vdrive.c/.h     virtual IEC drive: channel pool, OPEN/LOAD/SAVE/command channel, CD:/path walking/mount hooks, U0> pins, bus-scan numbers
  vdrive_io.c/.h  mailbox register engine (ISR-side O(1) accessors + vio_poll), the fast LOAD/SAVE commands and the bus-scan protocol
  vd_cfg.h        build knobs: VD_COLD (FLASHMEM), VD_NSLOT, VD_DEV0/VD_DEV1 defaults
patches/        ca65 sources overlaid on the stock KERNAL by tools/build.py; segment name P_XXXX = load address
  vdrive.s        serial entry hooks, fast LOAD ($F4C8) / SAVE ($F605), bus scan, code placed in reclaimed tape space $F8E2-$FB8D (578 of 683 bytes used)
  banner.s        proof patch (changes the power-on banner)
firmware/       TeensyROM integration: IOH_VDrive.c (Special IO) and teensyrom-vdrive.patch (git apply onto TeensyROM)
tools/          build.py (patch overlay), make_tr_patch.sh (regenerate the TeensyROM patch), vdrive_mock.py (VICE mock),
                d64py.py (ctypes wrappers incl. register-level VIO), xref.py (reference finder over a disassembly)
tests/          test_d64*.py, test_vdrive.py, test_vio.py (unit), fw_host/ (glue vs host stubs), devcfg.c
docs/BRINGUP.md staged hardware bring-up checklist
build/          generated, git-ignored (kernal images derived from the ROM, libd64.dylib, disassembly)
```

## Commands
* `make test` — everything (rebuilds `build/libd64.dylib`, runs unit tests, device-number check, firmware host test, two VICE runs: `basic` and
  `scan`, the latter with true-drive 1541s at 8 and 10; about 4 minutes).
* `make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin` — hardware image (mailbox `$DE00`, window `$DF00`).
  `STOCK` defaults to VICE's bundled ROM under Homebrew. `make build/kernal-banner.bin`, `build/kernal-stock.bin` are bring-up images.
* `tools/make_tr_patch.sh /path/to/TeensyROM` — regenerate `firmware/teensyrom-vdrive.patch` after changing `firmware/IOH_VDrive.c`
  or anything in `d64/` (the patch embeds copies of those files). Always re-run it, then `git apply --check` on a clean checkout.
* Requires: `ca65/ld65` (cc65), VICE (`x64sc`, `c1541`), a C/C++ compiler, Python 3.

## Gotchas
* **Firmware version stamp:** our builds are `<stock>.<REV>` (stock 0.9 -> `0.9.0.1`), written into `TRVersion` by `tools/make_tr_patch.sh` from
  `firmware/REV`. **Bump `firmware/REV` (and regenerate the patch) on any commit that changes firmware behaviour** (`d64/`, `firmware/`, the
  patch edits). It shows on the C64 menu title, in the hex file name (`TeensyROM+_0.9.0.1_full.hex`, never confused with stock
  `0.9_full.hex`) and in the flash tool. Keep it digits-and-dots: `tools/lib/trserial.mjs` parses `TeensyROM+? v[\d.]+`.
* **No Commodore ROM bytes in the repo.** Only patches. `build/` is ignored for that reason; keep it that way.
* **The ctypes tests load `build/libd64.dylib`.** After editing `d64/*.c` run `make build/libd64.dylib` (or `make test`) first, or you
  test a stale library.
* The firmware compiles `.c` files inside the `.ino` as **C++**: no implicit `void*` conversions, etc. `make` runs a `-x c++`
  syntax check on the library, and `tests/fw_host/run.py` compiles the glue with `-Wall -Wextra -Werror`.
* Anything not reachable from the bus ISR must be `VD_COLD` (flash). Only `vio_io1_read/write` and `vio_io2_read/write` stay in RAM1.
* **Bus-handler data must be in fast RAM.** The registers and the 256-byte window are a separate small `vio_hot_t` (263 B, plain `static`
  = DTCM); only the main loop touches the big `vio_t` (RAM2/`DMAMEM`). A late answer from slow RAM makes the C64 read the previous bus
  byte. STAT is a token handshake (`$80` busy, `$5A` answered, `$A5` not mine; never a bare flag) so a floating-bus misread (`$DE`)
  can't look like "done". The 6502 aborts a load/save with an error if any answer fails mid-transfer.
* `U0>` is parsed before the CR/LF strip because devices 10 and 13 are LF and CR.
* The KERNAL stubs write `$A5-$A7` (vcall) and `$A3/$A4` (fast paths); these are serial-bus scratch bytes, free outside bit-banging. Nothing else in
  zero page is free once a user program runs (the bus scan keeps its state on the stack).
* **The mailbox is not live during BASIC init:** the TeensyROM's own IO1 handler runs until the program has loaded, so nothing may call the mailbox at reset
  (the first design put the scan in the reset routine; it moved to the first transaction, see DESIGN.md 16). A probe must be `LISTEN` + `SECOND`: with any
  drive on the bus `LISTEN` alone never fails.
* The device-number model: `pin[]` (user, EEPROM) and `dev[]` (current, 0 = none). Tests that call `vdrive_set_devices()` / `VIO.set_devices()` set pins.
  The Teensy glue calls `vio_arm_scan()` after attaching the units; leave it out in tests that do not want the SCANREQ answer.
* Names are PETSCII throughout the C code; the glue converts to/from ASCII for the host file system.
* PETSCII quirk when building fixtures with `c1541`: give it lower-case names to get unshifted (default-mode upper-case) PETSCII.

## Next steps
1. **Finish bring-up** (`docs/BRINGUP.md`, stages 7-9): the unchecked items listed under Status (USB device 9, `U0>`, `CD:/`, time-out, soak, sequential
   files, scratch/rename/`N:`) and now the 0.9.0.7 items (bus scan with a real drive, `.d81`, paths). The core path has passed, so the items below can
   start once the owner agrees; mind that the patch changes under any build in progress.
2. **Create new images** (agreed design, not started): with nothing mounted, `N:FILE.D64,ID` (or `.D71`/`.D81`) creates a blank image on the host file
   system and mounts it (glue + `vdrive.c` `N` command; today `N` only formats the mounted disk, and nothing creates an image file). Remembering mounts
   in a `/.vdrive` sidecar is done (see Status).
3. **BASIC utility** (~40 lines, kept as `.bas` text and converted with `petcat` in the Makefile): list the current directory, pick by number (mount an
   image, open a folder, `..`), "new image", "unmount", and show the device numbers (`PEEK(56854)`). Much of what it would do is now one `LOAD"$path"`.
4. **TeensyROM menu integration** (last, only if the BASIC utility feels clumsy): a `Shift-D` hotkey like the existing `Shift-K`
   (KERNAL image) / `Shift-R` (REU) that sets the highlighted file as the disk for device 8/9. Needs changes to the 6502 menu
   (`Source/C64/MainMenuCRT`, ACME) and regenerated headers, and probably an EEPROM map extension that upstream must agree to.
5. Smaller follow-ups: append mode (`,A`), `V`alidate, REL files, D81 sub-directories/partitions, plain `.prg` loading straight from the card, file names > 16
   characters in host listings, `MD:`/`RD:`, a scan that re-runs after a device is switched on later.
