# CLAUDE.md — TR+ virtual-drive KERNAL

## What this is
A replacement C64 KERNAL (901227-03 + patches) that the **TeensyROM+ (TR+)** serves live, plus the Teensy-side firmware that makes
the SD card and the USB drive behave as IEC drives backed by `.d64` / `.d71` / `.d81` images. KERNAL serial calls for those devices go to
a mailbox (IO1 `$DE10` registers + a 128-byte IO2 window at `$DF80`) instead of the bit-banged bus; any other device number still uses the
real IEC code, so physical drives keep working. LOAD/SAVE have block-transfer fast paths. Device numbers are automatic (SD first, USB
second, the first free numbers from 8 up: the KERNAL probes the real bus on the first disk access of a run) or fixed from BASIC with
`OPEN15,8,15,"U0>"+CHR$(n)` (kept in EEPROM, `CHR$(0)` = automatic, no reflash). Names and `CD` take paths: `LOAD"$/GAMES/G.D64",8`.

Full design and per-milestone notes: `docs/DESIGN.md` (sections 1-34, in order of construction).
Firmware integration and usage: `firmware/README.md`.

## Status (read this first)
* **Works on hardware (real C64, TR+ with this patch).** `make test` also passes on the host: C unit tests via ctypes against `c1541`, the 6502 patches in VICE through a
  monitor-driven mailbox mock, and the firmware glue compiled as C++ against host stubs. Toolchain for the firmware: `arduino-cli`, `teensy:avr@1.61.0`,
  `CRC32@2.0.1`; do NOT use Teensyduino 1.62.0 (SD read stalls). Build/fit: RAM1 padding −976 B, flash +15.7 KB, RAM2 +4.5 KB vs stock.
* **Verified on hardware by the owner (SD and USB):** handler listing and SD/USB detection, mailbox registers, stock and banner KERNAL control (banner shows
  `TRVDRIVE 64 BASIC V2` on F2), mounting with `CD:`, directory/`LOAD` of small and 6000-byte files, `SAVE`, `VERIFY`, the error channel, repeated directory listings,
  device 9 (USB stick), `U0>` renumbering and its EEPROM persistence, `CD:/`, the no-handler time-out, `SEQTEST`, `GETRAW`, `SOAK`, `DOSTEST`, `CDTEST`, `RENUM`,
  scratch / rename / `N:`, paths (`LOAD"$/dir/image.d64"`, `$==`), auto-mount and remembered mounts (`/.vdrive` sidecar), VDrive+SwiftLink, remote get/set with
  `trvdrive` while a program runs, `trls` while a program runs, the image swap, and the sidecar reporting with the TR menu showing (`"running":false`).
  Also: *Quest for the King* (`VAULTTEST` clean, plays and loads/saves) and CCGMS over TCP (see the Kernal Replace fixes below).
  `.d81` images on the card (mounted through the TR menu). The automatic bus scan with a real 1541-II on the bus, DIP-switched to several device numbers in turn: the SD/USB drives took the free numbers as documented.
* **`VDCOPY` (util/, `make util`):** pure machine code (`util/vdcopy.s`; first a BASIC program with a machine-code chunk loop, rewritten because the BASIC directory read and per-file work were slow) copying files in 2 KB chunks between any two units (real drive, SD, USB; either direction); tested in VICE with a true-drive 1541 at 8 and the SD drive at 9
  (`vdrive_mock.py scan`: copy all, skip existing, replace, list/range selection, SD to USB, SD to 1541 and back, 1541 to USB; byte-exact PRG and SEQ), **on hardware (owner): the BASIC version worked vdrive to vdrive and vdrive to 1541; the machine-code rewrite is not on hardware yet**. Building it found a real-drive bug: the UNLSN hook at `$EDFE` overwrote the
  operand of the `BIT` that stock UNTLK falls through, so the real drive never got UNTALK (garbled bytes in `GET#` after the first `CLRCHN`). `hk_untlk` now spells the whole routine out. Any KERNAL image built before
  that fix must be rebuilt (`make build/kernal-tr.bin`). Lesson: a hook that overwrites bytes at an entry point inside another routine's operand needs every fall-through into those bytes checked.
* **Alt-button disk switching (0.9.0.17, `docs/DESIGN.md` 22): host-tested only (`tests/fw_host`, the 6502 routine under py65 in a scratch script), NOT on hardware, not run in VICE (no ROM in the cloud session).**
  Taps on the TR+ Alt button (setting "None" only) cycle `/vdrive-config.txt` (SD: one tap, USB: two taps within 1 s); border flash through the new `BORDER` register `$DE18` and `irq_border` in the KERNAL IRQ routine. Rebuild `kernal-tr.bin` and re-copy it with the firmware.
* **VDrive Config.prg (0.9.0.18, `util/vdconfig.bas`, docs/DESIGN.md 23): NOT run on a C64 or in VICE (no ROM in the cloud session); host-tested only** in `tests/test_vdconfig.py` (a small BASIC V2 interpreter, `tests/basic_interp.py`,
  against a drive model), plus the new `"!CONFIG"` / `"!STATE"` host text channels in `tests/test_vio.py` and `tests/fw_host`. First hardware run: check cursor keys, the colour cells, the browser listing of a large folder and `GET#` of the listing.
* **Not yet checked on hardware:** the time the bus scan adds to the first access,
  `.d71` at all, `STATUS` compared with a real drive, IRQ/STOP-key interplay during loads.
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
* **Kernal Replace fixes (not VDrive bugs; any Kernal Replace image was affected):** (1) *Quest for the King* (`../q4k/dist/q4k.prg`) failed at random: the game keeps
  its map in the RAM under the I/O area and the KERNAL and switches `$01` to `$34` all the time, and `KernalCheck` disturbed RAM reads there (`VAULTTEST`, tests/asm/vaulttest.s;
  docs/UPSTREAM-kernal-replace-ram-under-rom.md). Fixed in 0.9.0.13 (`firmware/kernalreplace-fixes.patch`: no `/GAME` assert when HIRAM is known to be RAM; the image is served from a DTCM copy).
  (2) With Ethernet initialized (no cable needed) NativeEthernet runs the whole stack from a 1 ms PIT timer ISR, which made ~39 phi2 interrupts/s late; the first read after a `$01` write
  (the HIRAM sense) then answered late and q4k and CCGMS+SwiftLink over TCP glitched, NMI'd or froze. Masking the ENET interrupt did nothing. 0.9.0.14 masks `IRQ_PIT` and calls
  `fnet_poll()` from `loop()` while a Kernal Replace/VDrive/SwiftLink handler runs (`ProcessATCommand` turns PIT back on while it runs, since `connect` etc. block). 0.9.0.15 starts that
  poll in `PreStart` (until then ~15 late ISRs in the first 0.4 s after a launch crashed q4k; found with `LATE_ISR_TRACE`, a compile-time debug trace, see firmware/README.md); steady state is 0-1 late ISRs/s. CCGMS still glitches early in a BBS receive (not crashing; cause open).
  Tried and dropped: a partial-serve map (broke disk access). Ideas left: an AT command to stop Ethernet; find the rest of the late ISRs.
* **Feature notes (all hardware-verified unless listed above as not checked):**
  * VDrive+SwiftLink (0.9.0.4): mailbox moved to `$DE10` so the SwiftLink ACIA keeps `$DE00-$DE07`; Special IO "VDrive+SwiftLink" (`firmware/IOH_VDriveSwift.c`) runs both, and
    menu programs paired with SwiftLink (CCGMS...) get it when VDrive is the default Special IO. **KERNAL images built before this change (mailbox `$DE00`) no longer match the firmware: rebuild and re-copy `kernal-tr.bin`.**
  * 0.9.0.7: automatic device numbers by a lazy bus scan (SCANREQ token `$3C`, `bus_scan` in `patches/vdrive.s`, `VIO_SCAN`/`VIO_FREE`, `DEV0`/`DEV1` registers `$DE16/$DE17`; docs/DESIGN.md 16),
    `.d71`/`.d81` images (17), paths in `CD` and in names (18): `LOAD"$/dir/image.d64"` only lists (selection restored on close), `LOAD"$==path"` selects like `CD`, `UI`/`UJ`/`U9` (status 73),
    `U1`.. now `31`. A `U0>` number is a *fixed* number (0 = automatic); `vdrive_set_devices()` takes pins.
  * 0.9.0.8/0.9.0.9: disk changes from outside while a program runs: remote-control commands `0x64E3` (get) / `0x64E4` (set) on TCP 2112, swap at a quiet point, `trls` allowed after the quiet wait
    (`VDrive_RemoteStatus` / `VDrive_RemoteSet` in `firmware/IOH_VDrive.c`, dispatcher text in `firmware/vdrive_remote.inc`, docs/DESIGN.md 20). No 6502-side flag: channels live on the Teensy. Client: `trvdrive` in `../teensyrom-rcp`.
  * 0.9.0.12: slashes collapsed on mount and in `VD_PathFor` (the menu hands USB paths over with a doubled slash); with the menu showing, get reports the unit's `/.vdrive` image (`"running":false`)
    and set validates an image and rewrites the sidecar, so the next start mounts it.
  * Auto-mount (0.9.0.2): a program launched from inside a `.d64`/`.d71`/`.d81` opened in the TR menu (the menu browses all three; owner tried `.d64` and `.d81`) (`DriveDirPath` ends in `*`, menu source SD/USB) gets that image mounted by `VDrive_PreStart()` on the matching unit (SD=8, USB=9), its folder as the `CD:` directory.
  * Remembered mounts (0.9.0.3): each unit keeps `/.vdrive` (dot-named, so TR 0.9+ and our listing hide it) on its own card, holding the last mounted image path. Written on every successful mount, removed on any unmount (`CD:_`, `CD:/`), read in `VDrive_PreStart()` when the launch did not come from a disk image on that unit. A missing/invalid image just starts unmounted.
* **Bus timing margin (found with `tests/asm/bustest.s`, BUSTEST on the test disk):** on a real C64 the IO1/IO2 handlers answered late about once in 8000 (IO1) / 30000 (IO2)
  reads, giving bit-mixed or `$00` bytes (corrupt `SEQTEST`/`GETRAW`/directory listings); the stock ROM path was clean, so the cause is the number of cycles before the
  data lines are driven. 0.9.0.11 reads IO1 registers straight from the struct (about 10x fewer errors) and the KERNAL reads ST/DATA, the fast-LOAD header and every
  window byte twice in a row until two reads agree, and reads back window writes. `VD_TRACE` is off by default (it prints right after the answer, but was not the cause).
  Keep new bus-handler code as short as possible and never add work before `DataPortWriteWait`.
* 0.9.0.16: direct sector access (`OPEN n,d,n,"#"`, `U1`/`U2`/`UA`/`UB`, `B-R`/`B-W` as aliases, `B-P`, `B-A`, `B-F`; docs/DESIGN.md 21). Unit tests (`tests/test_vdrive.py`) and `BLKTEST` in VICE pass; **not on hardware yet**. No `M-R`/`M-W`/`M-E`.
* 0.9.0.19: `C:new=old[,old2...]` copy / join on the mounted image (`d64_copy`, docs/DESIGN.md 24). Unit tests and `DOS2TEST` in VICE pass; **not on hardware yet**.
* 0.9.0.20: REL files (`,L,`+length, `P`, one record per `PRINT#`, side sectors; D81 super side sectors; `C` and `S` handle them; docs/DESIGN.md 25, `tests/test_rel.py`). Unit tests and `RELTEST` in VICE pass; **not on hardware yet**.
* 0.9.0.21: append (`,A`; `d64_append`, a disk full restores the file; docs/DESIGN.md 26). Unit tests and `DOS2TEST` in VICE; not on hardware.
* 0.9.0.22: `V`alidate rebuilds the BAM (`d64_validate`, splats deleted, REL side sectors kept; byte-identical to `c1541 -validate`; docs/DESIGN.md 27). Unit tests and `DOS2TEST` in VICE; not on hardware.
* 0.9.0.23: `L:name` (toggle lock) and `W-1`/`W-0` (write protect, `d64_t.ro`), CMD syntax (docs/DESIGN.md 28). Unit tests and `DOS2TEST` in VICE; not on hardware.
* 0.9.0.24: new images, `N:NAME.D64,ID` (or `.D71`/`.D81`) with nothing mounted (host callback `create`, glue `VD_Create`, `d64_format_blank`; docs/DESIGN.md 29). Unit and glue host tests; **works on hardware (owner)**. The time a D81 takes on a slow USB stick against the KERNAL's ~1.6 s mailbox wait is not measured.
* 0.9.0.25: plain files on the card with nothing mounted (`LOAD"GAME.PRG"`, `"GAME"`, patterns, folder paths; host callbacks `plain_open`/`plain_read`/`plain_close`, channel mode 9; docs/DESIGN.md 30). Reading only. Unit and glue host tests.
* 0.9.0.26: `MD:`/`RD:` folders on the card with nothing mounted (host `mkdir`/`rmdir`; docs/DESIGN.md 31). Unit and glue host tests.
* 0.9.0.28: Special IO "VDrive+Swift+REU" (`firmware/IOH_VDriveSwiftREU.c`, docs/DESIGN.md 32): drive + SwiftLink + a 128 KB REU (RAM1 image + leftover RAM2). The IO2 window shrank to
  128 bytes at `$DF80` for every VDrive handler (REU registers at `$DF00-$DF7F`); names of 128+ characters fall back to the byte-level open. The REU's `$FF00` trigger would
  replace `KernalCheck` on `fBusSnoop`: `IO2Hndlr_VDriveSwiftREU` hands it back (`VDREU_Snoop`). Host tests, VICE, firmware build; **works on a real C64 (owner, 2026-10-23)**. **KERNAL images built before
  this change no longer match the firmware: rebuild and re-copy `kernal-tr.bin`.**
* 0.9.0.31: Special IO "VDrive+REU" (`firmware/IOH_VDriveREU.c`, docs/DESIGN.md 34): drive + the full 512 KB REU, no SwiftLink. Holds the IO2 split and `VDREU_Snoop` that
  VDrive+Swift+REU now shares (`IO2Hndlr_VDriveREU`). With any VDrive default, REU menu programs get it. Host tests, firmware build; **not on hardware yet**.
* Default device 8 for `LOAD`/`SAVE`/`VERIFY` without a number, as on the SX-64 (`P_E1DA` in `patches/vdrive.s`, docs/DESIGN.md 33). KERNAL only; VICE `basic` run; **not on hardware yet**. Rebuild `kernal-tr.bin`.
* Known open risks: C128 not supported (see above); tape (device 1) intentionally broken (its code space was reclaimed).

## Layout
```
d64/            portable C99 library, also compiled as C++ by the firmware (keep it C++-clean!)
  d64.c/.h        D64/D71/D81 engine: geometry, BAM, directory, file chains, create/write/scratch/rename/format, BASIC listing builder
  vdrive.c/.h     virtual IEC drive: channel pool, OPEN/LOAD/SAVE/command channel, CD:/path walking/mount hooks, U0> pins, bus-scan numbers
  vdrive_io.c/.h  mailbox register engine (ISR-side O(1) accessors + vio_poll), the fast LOAD/SAVE commands and the bus-scan protocol
  vd_cfg.h        build knobs: VD_COLD (FLASHMEM), VD_NSLOT, VD_DEV0/VD_DEV1 defaults
util/            VDCOPY: vdcopy.s (all machine code, BASIC SYS stub at $0801, tables BSS at $2000; tools/make_util.py assembles it);
                VDrive Config: vdconfig.bas (BASIC, tokenised with petcat by tools/make_util.py into build/util/"VDrive Config.prg")
patches/        ca65 sources overlaid on the stock KERNAL by tools/build.py; segment name P_XXXX = load address
  vdrive.s        serial entry hooks, fast LOAD ($F4C8) / SAVE ($F605), bus scan, code placed in reclaimed tape space $F8E2-$FB8D (666 of 683 bytes used; `irq_border` for the border flash included, the build asserts the limit)
  banner.s        proof patch (changes the power-on banner)
firmware/       TeensyROM integration: IOH_VDrive.c (Special IO), IOH_VDriveSwift.c / IOH_VDriveREU.c / IOH_VDriveSwiftREU.c (combined Special IOs) and
                teensyrom-vdrive.patch (git apply onto TeensyROM)
tools/          build.py (patch overlay), make_tr_patch.sh (regenerate the TeensyROM patch), vdrive_mock.py (VICE mock),
                d64py.py (ctypes wrappers incl. register-level VIO), xref.py (reference finder over a disassembly)
tests/          test_d64*.py, test_vdrive.py, test_vio.py (unit), fw_host/ (glue vs host stubs), devcfg.c,
                test_vdconfig.py + basic_interp.py (VDrive Config in a BASIC interpreter model), test_irq_border.py (py65)
build/          generated, git-ignored (kernal images derived from the ROM, libd64.dylib, disassembly)
```

## Commands
* `make test` — everything (rebuilds `build/libd64.dylib`, runs unit tests, device-number check, firmware host test, two VICE runs: `basic` and
  `scan`, the latter with true-drive 1541s at 8 and 10; about 4 minutes).
* `make test-border` — the `irq_border` 6502 routine in py65 (skipped without `pip install py65`; also part of `make test`).
* `make util` — `build/util/vdcopy.prg`, the file copy utility between any two units (also on the test disk), and `build/util/VDrive Config.prg` (needs petcat).
* `make test-vdconfig` — VDrive Config in the BASIC interpreter model (pure Python; also part of `make test`).
* `make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin` — hardware image (mailbox `$DE10`, window `$DF80`).
  `STOCK` defaults to VICE's bundled ROM under Homebrew. `make build/kernal-banner.bin`, `build/kernal-stock.bin` are bring-up images.
* `tools/make_tr_patch.sh /path/to/TeensyROM` — regenerate `firmware/teensyrom-vdrive.patch` after changing `firmware/IOH_VDrive.c`
  or anything in `d64/` (the patch embeds copies of those files). Always re-run it, then `git apply --check` on a clean checkout. Run it against a *pristine* upstream tree (`git archive HEAD` of the TeensyROM checkout), not the owner's already-patched one: its edits assert on unpatched text. It also reads `firmware/vdrive_remote.inc` (the remote-control dispatcher inserted into `SerUSBIO.ino`).
* Requires: `ca65/ld65` (cc65), VICE (`x64sc`, `c1541`), a C/C++ compiler, Python 3.

## Testing in the cloud (Linux container, no Mac)
The full `make test`, VICE runs included, works in a cloud session; do not skip the VICE part for want of a ROM.
* `tools/cloud_setup.sh` once per container: installs `vice cc65 xvfb clang` and `py65`, and puts the ROMs that VICE's upstream source tree ships
  (`data/C64`, `data/DRIVES`, from the VICE source mirror on GitHub) in `~/.local/share/vice`, where `x64sc` finds them. Ubuntu's `vice` package is the
  DFSG build without ROMs, so `apt` alone is not enough. The ROMs stay outside the repository (no Commodore ROM bytes in the repo).
* Then: `eval "$(tools/cloud_setup.sh --env)"; xvfb-run -a make test STOCK="$STOCK"`. `STOCK` = VICE's `kernal-901227-03.bin` is the base for every
  KERNAL image (`kernal-vd.bin`, `kernal-tr.bin`), as the Homebrew default is on the owner's Mac. `xvfb-run` gives `x64sc` a display. About 7 minutes.
* The `--env` line also puts a `c++` → `clang++` link first on `PATH`: with GCC, `tests/fw_host` fails to compile on existing glue code
  (`-Wextra` enum/`-Wformat-truncation` under `-Werror`); the owner's compiler is clang.
* Still say in the pull request what was not run: a VICE run is not a hardware run.
* **GitHub Actions** (`.github/workflows/test.yml`) runs the same setup and `make test` on every push to `main`/`bleeding-edge` and every pull request,
  plus `make build/kernal-tr.bin` and `make util`. `cloud_setup.sh` uses `sudo` when not root; py65 comes from `actions/setup-python` (`python3 -I` skips the user site).

## Gotchas
* **Firmware version stamp:** our builds are `<stock>.<REV>` (stock 0.9 -> `0.9.0.1`), written into `TRVersion` by `tools/make_tr_patch.sh` from
  `firmware/REV`. **Bump `firmware/REV` (and regenerate the patch) on any commit that changes firmware behaviour** (`d64/`, `firmware/`, the
  patch edits). It shows on the C64 menu title, in the hex file name (`TeensyROM+_0.9.0.1_full.hex`, never confused with stock
  `0.9_full.hex`) and in the flash tool. Keep it digits-and-dots: `tools/lib/trserial.mjs` parses `TeensyROM+? v[\d.]+`.
* **Keep the patch hunks away from lines upstream is changing.** `tools/make_tr_patch.sh` places edits so they apply on current `main` *and* on the open TeensyROM PRs that touch the same files
  (written for #39 and #61-#70, all merged by `5728ddc9`; open at that rebase: #60, #62, #63): the VDrive entries in the `enumIOHandlers` / `IOHandler[]` lists sit *before* the Kernal Replace entries (upstream appends GeoRAM after `IOH_Debug`),
  the Busy-refusal bypass in `SerUSBIO.ino` is a block after the `ASIDAllowed` statement (not a rewrite of the `if`), the menu remap in `IOHandlers.ino` is a case ahead of upstream's call, and `FnetPollHold = false` follows the `#endif`
  in `SetUpMainMenuROM()`. Test a regenerated patch with `git apply --check` on `git archive origin/main` and on `git merge-tree --write-tree origin/main pr/N` trees. The Special IO list order is VDrive, VDrive+Swift,
  VDrive+Swift+REU, VDrive+REU, Kernal Replace, Debug (0.9.0.31): a stored default Special IO (`eepAdNextIOHndlr`, an index) of Kernal Replace from an older build now selects a VDrive entry.
* **Published KERNAL hash:** the README's Installing section lists the SHA-256 of `kernal-tr.bin` built from the 901227-03 ROM; `make test-hash` (part of `make test`) fails when a patch or `tools/build.py` changes the image and the "from this revision" row was not updated. Update the "last run on a real C64" row only when the owner reports a hardware run of that image.
* **No Commodore ROM bytes in the repo.** Only patches. `build/` is ignored for that reason; keep it that way.
* **The ctypes tests load `build/libd64.dylib`.** After editing `d64/*.c` run `make build/libd64.dylib` (or `make test`) first, or you
  test a stale library.
* The firmware compiles `.c` files inside the `.ino` as **C++**: no implicit `void*` conversions, etc. `make` runs a `-x c++`
  syntax check on the library, and `tests/fw_host/run.py` compiles the glue with `-Wall -Wextra -Werror`.
* Anything not reachable from the bus ISR must be `VD_COLD` (flash). Only `vio_io1_read/write` and `vio_io2_read/write` stay in RAM1.
* **Bus-handler data must be in fast RAM.** The registers and the 128-byte window are a separate small `vio_hot_t` (138 B, plain `static`
  = DTCM); only the main loop touches the big `vio_t` (RAM2/`DMAMEM`). A late answer from slow RAM makes the C64 read the previous bus
  byte. STAT is a token handshake (`$80` busy, `$5A` answered, `$A5` not mine; never a bare flag) so a floating-bus misread (`$DE`)
  can't look like "done". The 6502 aborts a load/save with an error if any answer fails mid-transfer.
* `U0>` is parsed before the CR/LF strip because devices 10 and 13 are LF and CR.
* The KERNAL stubs write `$A5-$A7` (vcall) and `$A3/$A4` (fast paths); these are serial-bus scratch bytes, free outside bit-banging. Nothing else in
  zero page is free once a user program runs (the bus scan keeps its state on the stack).
* **The mailbox is not live during BASIC init:** the TeensyROM's own IO1 handler runs until the program has loaded, so nothing may call the mailbox at reset
  (the first design put the scan in the reset routine; it moved to the first transaction, see docs/DESIGN.md 16). A probe must be `LISTEN` + `SECOND`: with any
  drive on the bus `LISTEN` alone never fails.
* The device-number model: `pin[]` (user, EEPROM) and `dev[]` (current, 0 = none). Tests that call `vdrive_set_devices()` / `VIO.set_devices()` set pins.
  The Teensy glue calls `vio_arm_scan()` after attaching the units; leave it out in tests that do not want the SCANREQ answer.
* Names are PETSCII throughout the C code; the glue converts to/from ASCII for the host file system.
* PETSCII quirk when building fixtures with `c1541`: give it lower-case names to get unshifted (default-mode upper-case) PETSCII.

## Next steps
1. **Finish bring-up**: the "Not yet checked on hardware" list under Status (`.d71`, `STATUS`, IRQ/STOP-key during loads). The core path has passed, so the items below can
   start once the owner agrees; mind that the patch changes under any build in progress.
2. **Create new images**: done in 0.9.0.24 (`N:FILE.D64,ID` with nothing mounted; docs/DESIGN.md 29), works on hardware; still to check: the time a D81 takes
   on a USB stick against the KERNAL's ~1.6 s mailbox wait.
3. **BASIC utility** (~40 lines, kept as `.bas` text and converted with `petcat` in the Makefile): list the current directory, pick by number (mount an
   image, open a folder, `..`), "new image", "unmount", and show the device numbers (`PEEK(56854)`). Much of what it would do is now one `LOAD"$path"`.
4. **TeensyROM menu integration** (last, only if the BASIC utility feels clumsy): a `Shift-D` hotkey like the existing `Shift-K`
   (KERNAL image) / `Shift-R` (REU) that sets the highlighted file as the disk for device 8/9. Needs changes to the 6502 menu
   (`Source/C64/MainMenuCRT`, ACME) and regenerated headers, and probably an EEPROM map extension that upstream must agree to.
5. Smaller follow-ups: D81 sub-directories/partitions, writing plain files to the card, file names > 16
   characters in host listings, a scan that re-runs after a device is switched on later.
