# CLAUDE.md — TR+ virtual-drive KERNAL

## What this is
A replacement C64 KERNAL (the machine's own ROM, read by DMA and patched in memory by the firmware; or `sd:/VDrive/kernal.bin`) that the **TeensyROM+ (TR+)** serves live, plus the Teensy-side firmware that makes
the SD card and the USB drive behave as IEC drives backed by `.d64` / `.d71` / `.d81` images. KERNAL serial calls for those devices go to
a mailbox (IO1 `$DE10` registers + a 128-byte IO2 window at `$DF80`) instead of the bit-banged bus; any other device number still uses the
real IEC code, so physical drives keep working. LOAD/SAVE have block-transfer fast paths. Device numbers are automatic (SD first, USB
second, the first free numbers from 8 up: the KERNAL probes the real bus on the first disk access of a run) or fixed from BASIC with
`OPEN15,8,15,"U0>"+CHR$(n)` (kept in EEPROM, `CHR$(0)` = automatic, no reflash). Names and `CD` take paths: `LOAD"$/GAMES/G.D64",8`.

Full design and per-milestone notes: `docs/DESIGN.md` (sections 1-52, in order of construction).
Firmware integration and usage: `firmware/README.md`.

## Status (read this first)
* **Works on hardware (real C64, TR+ with this patch).** `make test` also passes on the host: C unit tests via ctypes against `c1541`, the 6502 patches in VICE through a
  monitor-driven mailbox mock, and the firmware glue compiled as C++ against host stubs. Toolchain for the firmware: `arduino-cli`, `teensy:avr@1.61.0`,
  `CRC32@2.0.1`; do NOT use Teensyduino 1.62.0 (SD read stalls). Build/fit (0.9.0.40 vs stock `fa509421`, TR+ main image; 0.9.0.42 and 0.9.0.43 on `136baa83`, 0.9.0.45 on `6518b966`: the same RAM1): RAM1 variables +13.7 KB (free for locals 66 KB), RAM1 code +1.6 KB (padding 25.4 KB left), flash code +43 KB, RAM2 +7 KB. 0.9.0.53: RAM2 +0 (no VDrive symbol there), RAM1 free for locals 56.8 KB.
  `node tools/build-firmware.mjs --target tr-plus` (in a TeensyROM tree with the patch applied) writes `build/firmware/TeensyROM+_<version>_full.hex`; `--force` to overwrite.
  On the owner's Mac `ARDUINO_DIRECTORIES_USER=~/code/3p/Arduino` (set in the shell profile; CRC32 lives in its `libraries/`), not the default `~/Documents/Arduino`:
  Documents is iCloud Drive and an evicted `CRC32.h` made builds fail with "Operation timed out". A shell started before the profile change lacks the variable: pass it on the command.
* **Verified on hardware by the owner (SD and USB):** handler listing and SD/USB detection, mailbox registers, stock and banner KERNAL control (the bring-up banner image showed
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
  **Whole-disk copy (`D` in its new start menu, docs/DESIGN.md 40): NOT on hardware.** A `.d64` onto a real floppy or a floppy into a new `.d64` (any two units): 683 sectors through `#` buffer channels with `U1` / `B-P` + `U2`, no firmware change;
  a real destination may be formatted first with the source's name and ID. TR menu colours as in VDrive Config (third `!STATE` line), upper-case set kept. `tests/test_vdcopy.py` (py65 against `vdrive.c` on both units, `make test-vdcopy`)
  and `vdrive_mock.py scan` (SD image onto the true-drive 1541 with format and back into a new USB image, byte-identical). Tables moved to `$2800` (the code passed `$1FFF`). First hardware run: time a disk, and a `23` sector on a real floppy.
* **Alt-button disk switching (0.9.0.17, `docs/DESIGN.md` 22): works on hardware (owner, 2026-10-30: single and double tap);** host-tested in `tests/fw_host` and the 6502 routine under py65 (`make test-border`); not run in VICE.
  Taps on the TR+ Alt button (setting "None" only) cycle `/vdrive-config.txt` (SD: one tap, USB: two taps within 1 s); border flash through the new `BORDER` register `$DE18` and `irq_border` in the KERNAL IRQ routine. Rebuild `kernal-tr.bin` and re-copy it with the firmware.
* **VDrive Config.prg (0.9.0.18; machine code since, `util/vdconfig.s`, docs/DESIGN.md 23): works on hardware (owner, 2026-10-30, the 0.9.0.35 build from the TR menu); not run in VICE;** host-tested in `tests/test_vdconfig.py` (the real PRG in py65, `tests/c64_sim.py`
  answering its KERNAL calls, against a drive model), plus the new `"!CONFIG"` / `"!STATE"` host text channels in `tests/test_vio.py` and `tests/fw_host`.
  Since 0.9.0.35: in the TR menu's Utilities (Teensy memory, `vdconfig.prg.h` generated by `make_tr_patch.sh`), the lower-case character set (`PS` swaps letter case at assembly time; `CMDS` strings for the drive are not swapped: keep it that way),
  `Q` = back to the TR menu (`VIO_MENU`), no bus access of its own at start (the menu's probe settled the numbers).
  0.9.0.40: the TR menu's colour theme and look (reverse banner, keys in reverse option colour): the colours come as a third `!STATE` line (7 hex digits from `IO1[rwRegColorRefStart..]`;
  the menu's defaults without it). py65 and `tests/fw_host` tests; **not on hardware yet**.
* **0.9.0.35: the TR menu probes the bus before every VDrive launch (docs/DESIGN.md 37): host tests and the 6502 routine in VICE with true-drive 1541s (`make test-menuprobe`); works on hardware (owner, 2026-10-30).** `firmware/vdrive_probe.asm` is inserted into
  `MainMenu.asm` by `make_tr_patch.sh`, which also rebuilds `TeensyROMC64.h` with the TR's `tools/build-c64.mjs` (needs `acme` 0.97 and `node`). No new menu registers: the PreStart handshake is reused (`rwRegScratch`, `VDrive_ProbeStep()` first in `KERNALPreStart()`); `fBusSnoop` is
  held back while the probe runs. Also `VIO_MENU` (mailbox command 16): back to the TR menu.
* **Bus scan time on the first access:** no noticeable delay on hardware (owner, 2026-10-30).
* **`VDSUITE` on hardware (owner, 2026-11-01, real C64, 0.9.0.42): all checks pass on the SD and the USB unit** (sections `1`-`9`: `N:` of D64/D71/D81, the 43 image checks on each
  of the three formats, bad sectors, card files, CMD paths, the `S:`/`RD:` clean-up). Its `10` "checks by hand" are not done yet.
  **Also on an NTSC C128 in C64 mode (owner, 2026-11-01, 0.9.0.42): `9` "run all" passes**; the hand checks are not done there either. A second run of section `6`
  without the clean-up `8` in between failed `rd, folder not empty (63)` with 62: a suite bug (the left-over `T2/E.D64` made `N:` give 63 and mount nothing, so the
  following `CD:_` went up a folder), fixed by an `S:E.D64` before the `N:`; `make test`'s `basic` run now runs `6` twice.
* **Not yet checked on hardware** (VDSUITE's hand checks): `STATUS` compared with a real drive, IRQ/STOP-key interplay during loads, `LOAD` with no device number,
  VDrive+REU (REU-checker 512 KB with an image mounted), the VDCOPY machine-code rewrite (SD to 1541 and back), `S:` on a read-only image file (26);
  the same hand checks on a C128 in C64 mode (its "run all" passes, see above).
* **BASIC test programs** (`tests/basic/`, built into `build/testdisk/vdtest.d64` by `make testdisk`) run inside VICE in `make test` and on the C64; they
  found two real bugs that unit tests missed: `CIOUT` must return `A` = the byte sent (CHROUT tail-jumps into it; BASIC's PRINT broke after a
  number), and `U0>` must also be accepted with PRINT#'s trailing CR.
  `VDSUITE` (tests/basic/vdsuite.bas, the "Not yet checked" items in one menu program; 186 checks with the clean-up (`8`, `S:`/`RD:`), all pass in the `basic` VICE run and on hardware (SD and USB, see above)) also needs `vdbad.d64` and
  `vdplain.prg` from `make testdisk` (`tools/suite_files.py`, shared with the mock) in the card root. It found a drive bug, fixed in 0.9.0.36: a successful command did not reset an error nobody
  read (a 1541 says `00, OK` after it), so a stale `62` from an earlier `LOAD` came back after a good `CD:/`. Now every OPEN and command starts at `00` (`VD_UNLSN` in `vdrive.c`; CLOSE keeps its own error).
* **Lessons from bring-up (keep):** (1) a flat NTSC C128 in C64 mode did not reliably serve the replacement KERNAL (the Skoe HIRAM sense cannot work there; upstream's DMA sense, in since 0.9.0.37, fixes it: VDSUITE's "run all" passes on an NTSC C128 with 0.9.0.42): before that, use a real C64, and
  verify "is it really served" with the banner (`kernal-tr.bin` from 0.9.0.36 prints `VDRIVE SD n USB n` on the second line, `kernal-banner.bin` `TRVDRIVE` on the first; a stock image can't show either). (2) All SD/USB probing
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
  `fnet_poll()` from `loop()` while a Kernal Replace/VDrive handler runs (`ProcessATCommand` turns PIT back on while it runs, since `connect` etc. block; plain SwiftLink on the stock KERNAL dropped from the list in 0.9.0.41, see below). 0.9.0.15 starts that
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
  That includes the bus snoop (`KernalCheck`), which runs ahead of every IO1/IO2 answer: 0.9.0.37-0.9.0.41 corrupted directory listings on a C64 (owner, bisected: 0.9.0.36 clean,
  0.9.0.37 not) because upstream PR 78's one extra test there made GCC save six registers on every cycle (`push {r3-r7, lr}`; compare `objdump -d` of `Teensy.ino.elf`
  from a `--keep-work` build). Upstream `136baa83` hooks a C64 or C128 check once per launch (`KernalCheckArm`); fixed in 0.9.0.42. Check that disassembly after any change near it.
* 0.9.0.16: direct sector access (`OPEN n,d,n,"#"`, `U1`/`U2`/`UA`/`UB`, `B-R`/`B-W` as aliases, `B-P`, `B-A`, `B-F`; docs/DESIGN.md 21). Unit tests (`tests/test_vdrive.py`) and `BLKTEST` in VICE pass; on hardware through VDSUITE (2026-11-01). No `M-R`/`M-W`/`M-E`.
* 0.9.0.19: `C:new=old[,old2...]` copy / join on the mounted image (`d64_copy`, docs/DESIGN.md 24). Unit tests and `DOS2TEST` in VICE pass; on hardware through VDSUITE.
* 0.9.0.20: REL files (`,L,`+length, `P`, one record per `PRINT#`, side sectors; D81 super side sectors; `C` and `S` handle them; docs/DESIGN.md 25, `tests/test_rel.py`). Unit tests and `RELTEST` in VICE pass; on hardware through VDSUITE.
* 0.9.0.21: append (`,A`; `d64_append`, a disk full restores the file; docs/DESIGN.md 26). Unit tests and `DOS2TEST` in VICE; on hardware through VDSUITE.
* 0.9.0.22: `V`alidate rebuilds the BAM (`d64_validate`, splats deleted, REL side sectors kept; byte-identical to `c1541 -validate`; docs/DESIGN.md 27). Unit tests and `DOS2TEST` in VICE; on hardware through VDSUITE.
* 0.9.0.23: `L:name` (toggle lock) and `W-1`/`W-0` (write protect, `d64_t.ro`), CMD syntax (docs/DESIGN.md 28). Unit tests and `DOS2TEST` in VICE; on hardware through VDSUITE.
* 0.9.0.24: new images, `N:NAME.D64,ID` (or `.D71`/`.D81`) with nothing mounted (host callback `create`, glue `VD_Create`, `d64_format_blank`; docs/DESIGN.md 29). Unit and glue host tests; **works on hardware (owner)**. A D81 `N:` on a slow (10+ year old) USB stick takes about 0.8 s on hardware (owner, 2026-11-01), half the KERNAL's ~1.6 s mailbox wait.
* 0.9.0.25: plain files on the card with nothing mounted (`LOAD"GAME.PRG"`, `"GAME"`, patterns, folder paths; host callbacks `plain_open`/`plain_read`/`plain_close`, channel mode 9; docs/DESIGN.md 30). Reading only. Unit and glue host tests; on hardware through VDSUITE `6`.
* 0.9.0.26: `MD:`/`RD:` folders on the card with nothing mounted (host `mkdir`/`rmdir`; docs/DESIGN.md 31). Unit and glue host tests; on hardware through VDSUITE `6`.
* 0.9.0.28: Special IO "VDrive+Swift+REU" (`firmware/IOH_VDriveSwiftREU.c`, docs/DESIGN.md 32): drive + SwiftLink + a 128 KB REU (RAM1 image + leftover RAM2). The IO2 window shrank to
  128 bytes at `$DF80` for every VDrive handler (REU registers at `$DF00-$DF7F`); names of 128+ characters fall back to the byte-level open. The REU's `$FF00` trigger would
  replace `KernalCheck` on `fBusSnoop`: `IO2Hndlr_VDriveSwiftREU` hands it back (`VDREU_Snoop`). Host tests, VICE, firmware build; **works on a real C64 (owner, 2026-10-23)**. **KERNAL images built before
  this change no longer match the firmware: rebuild and re-copy `kernal-tr.bin`.**
* 0.9.0.31: Special IO "VDrive+REU" (`firmware/IOH_VDriveREU.c`, docs/DESIGN.md 34): drive + the full 512 KB REU, no SwiftLink. Holds the IO2 split and `VDREU_Snoop` that
  VDrive+Swift+REU now shares (`IO2Hndlr_VDriveREU`). With any VDrive default, REU menu programs get it. Host tests, firmware build; **not on hardware yet**.
* 0.9.0.33: CMD HD / sd2iec path syntax (`[n]//dir/:name`, `[n]/dir/:name`) in names, `$`, `CD`, `MD`/`RD`, `N` (`cmd_path` in `vdrive.c`, docs/DESIGN.md 36). A path before a colon is CMD (single `/` = here), after the colon or without one ours (leading `/` = root); `CD/X` changed to relative. Unit tests; VICE and hardware through VDSUITE `7`.
* 0.9.0.47: aliases for long host names (`THE-PU^SIDE1.D64`, `vd_alias` in `d64/vdrive.c`, `VD_Resolve` in the glue; collisions are dropped, not numbered; docs/DESIGN.md 44). Unit and glue host tests; **works on a real C64 (owner, 2026-11-05)**.
* 0.9.0.48: **a VDrive KERNAL in the machine itself** (an EPROM, a MEGA65 C64 core's KERNAL file; asked for the MEGA65 by Boris Schneider-Johne; docs/DESIGN.md 45). No separate build.
  KERNAL: `vcall` writes nothing to the mailbox unless `vd_here` reads ID0 `$DE19` = `$D6` (4 tries, so a late answer does not hand a transaction to the real bus);
  hooks leave ARG in `$A7` and `vcall` writes `MBX_ARG` after the check; `open_named` checks before filling the window/LEN. Reason beyond speed: under the TR menu's handler
  `$DE11`/`$DE15` are `rwRegMIDISettings`/`rwRegTimezone`, which a write stores in EEPROM. Firmware: `VD_KernalPatchPresent` (DMA image already holds every patch run) = this build's
  KERNAL resident: nothing served, menu probe kept; banner tag but other bytes = another revision: nothing served, no menu probe (`vdNoMenuProbe`); unknown ROM / failed DMA: no probe either.
  py65 (`make test-gate`), `tests/fw_host`, VICE `basic` (no ID: no mailbox command); **works on a MEGA65 (Boris, reported 2026-11-08)**; not yet with an EPROM in a real C64. Rebuild `kernal-tr.bin`.
* 0.9.0.49: the banner's drive numbers for a resident VDrive KERNAL (docs/DESIGN.md 46). Every launch (F2 too) runs `XferCopyRun` in `MainMenu.asm`: PreStart, our probe, then the KERNAL's
  init routines and INITMS called in place (no reset) under the menu's IO1 handler, which answers plain reads with `IO1[n]`. So `VD_BannerRegs` puts tag `$D6 $64` + the two `VDUNITS:` bytes
  in menu IO1 offsets 60-63 (`$DE3C-$DE3F`, unused between 52 and `StartSIDRegs`), updated when the probe ends and cleared by every other PreStart (`VDrive_ProbeStep`). `vd_head` copies the image bytes
  to `RES` ($FC/$FD), else the registers when the tag reads right; the stock line prints the ROM's own text at `$E49B` (all five ROMs have it; build.py checks; its INITMS check had never run, `E444` -> `E43D`).
  Banner area 312 of 314 bytes. py65 (`test_banner.py`), `tests/fw_host`; **not on hardware**. Note: RAM does not survive a MEGA65 reset (Boris), but a launch is no reset.
* 0.9.0.50: VDrive Config's browser lists folders in 128-byte blocks (`fopen`/`fget`/`mcall` in `util/vdconfig.s`: `FLOAD_OPEN` with the `$/path/` name in the window, then `FLOAD_BLK` until EOI;
  `rdb` picks `fget` or `CHRIN`), falling back to `OPEN` + `CHRIN` when not answered (`$A5`, `$3C`, time-out, a name of 128+). Reported slow by Boris (MEGA65, a full card; CBM FileBrowser, which uses `LOAD`, was fast).
  `tests/c64_sim.py` answers the two commands from the drive model (`fast=False`: not mine); `test_vdconfig.py` block/fallback/big-folder/error cases. PRG ends at `$1FA3` (BSS `$2000`). **Not on hardware.**
* **DMATEST** (docs/DESIGN.md 48): can the TR+ DMA-write C64 RAM while our KERNAL is served (the question behind a DMA LOAD)? The source says yes (the ISR skips `fBusSnoop` during a transfer;
  `KernalCheck` asserts `/GAME` only on CPU reads). `DMATEST` on the test disk waits while `tools/dmatest.py -H <ip>` writes 28.4 KB a round with `WriteC64Mem` (needs teensyrom-rcp's `trlib.py`),
  then checks every byte (under ROM too), three guard pages and the served KERNAL's sum. py65 (`make test-dmatest`); **passes on a C128 in C64 mode (owner, 2026-11-08; not yet on a C64)**:
  250 rounds, only the `--selftest` byte counted. A 4 KB `WriteC64Mem` once timed out *receiving* over TCP (no `fnet_poll()` in the remote command's wait while a VDrive handler runs; DESIGN 48).
* 0.9.0.51: remote commands keep the Ethernet stack polled while they wait for input bytes (`CmdInputPoll()` in `SerUSBIO.ino`, added to `kernal-fnet-poll.patch` and to the
  PR #85 branch, pushed 2026-11-08): with the PIT masked (0.9.0.14 / #85) a wait could not receive the rest of a payload. Builds; fw_host passes. The DMATEST time-outs it was
  meant for turned out to be WiFi loss on the host's path (DESIGN 48: `--nodelay` 300 rounds passed from the Mac, steady from a wired Pi); not shown to fix anything seen.
* 0.9.0.53: **no VDrive data in RAM2** (docs/DESIGN.md 50). Our ~9.4 KB of `DMAMEM` statics shrank the heap the REU takes its 8 KB banks from: 63 banks, rounded to 256 KB, under VDrive+REU *and* the plain "512KB REU" (owner, 0.9.0.51; stock 512 KB). Now plain `static` (DTCM). **Never add `DMAMEM` statics to VDrive code.**
  `REUTEST` (`tests/asm/reutest.s`, test disk): the REU's real size by bank wrap plus a full data check; VICE 128/256/512/none; on hardware 0.9.0.51 VDrive+Swift+REU 128 KB clean. On hardware (owner, 0.9.0.53): plain "512KB REU" 512 KB again; VDrive+REU still 256 KB (62 banks); fixed in 0.9.0.55, below.
  0.9.0.54: VDrive frees Kernal Replace's 8 KB heap `KernalBin` after copying it to `KernalFast` (one bank); prints `VDrive: RAM2 free before the REU`. On hardware (owner): 512 KB from F2, still 256 KB (63 banks) when launched from the SD browser.
  0.9.0.55: the menu's 64 KB directory listing (`DriveDirMenu`), freed by `AllocRAMBanks()` only after VDrive's PreStart had opened its `File`s above it, left a hole; `KERNALPreStart()` now frees it first for the two REU handlers (`make_tr_patch.sh`). `RAM2BytesFree()` only measures above the heap top. **Works on hardware (owner, 2026-11-09): VDrive+REU 512 KB launched from the SD browser.**
  0.9.0.56: the 0.9.0.54 free left `KernalBin` NULL, which upstream's `InitHndlr_KernalReplace()` reads as "nothing loaded", so it never re-hooked `KernalCheck` after the PRG transfer: every VDrive menu launch ran on the C64's own KERNAL (VDrive Config hung after going lower case; owner, 0.9.0.55). Now a `KernalFastOnly` flag counts as loaded. Firmware builds, fw_host passes; **works on hardware (owner, 2026-11-09): VDrive Config launches from the TR menu again.**
* 0.9.0.52: **DMA LOAD** (docs/DESIGN.md 49): the KERNAL's LOAD loop sends `VIO_FLOAD_DMA` (17) with the address in ARG/LEN; the Teensy writes up to 2 KB (`VD_DmaPart`) into RAM by
  DMA and the new end into `$AE/$AF`, or answers a window block where DMA is not used (below `$0200`, `$D000-$DFFF`). A failed DMA: DEVICE NOT PRESENT. VERIFY / SAVE unchanged.
  `test_vio`, `fw_host`, VICE `basic` with DMA on (monitor writes); **on hardware (owner, 2026-11-08, C128 in C64 mode): a 48 KB LOAD in 13-14 jiffies against 85 on 0.9.0.51** (about 6-8x); the other checks of DESIGN 49 not done yet. Rebuild `kernal-tr.bin`; an older firmware fails every LOAD with this KERNAL.
  Compiled in only with `Fab04_VDriveDMALoad` (defined by `make_tr_patch.sh`; without it: window blocks). The KERNAL's request is `VD_DMA_LOAD` in `patches/vdrive.s` (0 = the 0.9.0.51 loop, byte-identical).
  **Keep both out of upstream TeensyROM PRs** (Travis: no DMA in the first VDrive firmware): `VD_DMA_LOAD=0 tools/make_tr_patch.sh <tree>` writes `build/teensyrom-vdrive-nodma.patch` without either.
* 0.9.0.57: **DMA SAVE and VERIFY** (docs/DESIGN.md 51). SAVE: the loop sends `VIO_FSAVE_DMA` (18) first; the Teensy
  DMA-reads `$AC-$AF`, then up to 2 KB from `$AC`, writes the new `$AC/$AD` and appends to the file (DATA 0 = ask again, DATA 1 / not mine = the window block as before).
  VERIFY: `fl_hook` opens with `VIO_FVERIFY_OPEN` (19), taken only with DMA reads and the whole file in `$0200-$CFFF` (`vio_dma_rd_ok`; the length by a read-through, SA from `$A4`, `$C3/$C4`),
  else not mine = the stock VERIFY; then each `VIO_FLOAD_DMA` compares (ST `$10`), DATA always 0. KERNAL: 9 bytes, 7 won back from the error exits (`P_F8E2` 678 of 683), all under `VD_DMA_LOAD`
  (`kernal-tr-nodma.bin` still byte-identical). Glue: `VD_DmaRead`, under `Fab04_VDriveDMALoad` as the write. `test_vio`, `fw_host` (both builds; run it as a non-root user: as root the
  read-only `S:` check fails and stops the run, before this change too), VICE `basic` (monitor reads). **Not on hardware**: DMA reads of a running program are the open risk (remote-read note below).
* **Plain TeensyROM (no TR+) build, started 2026-11-05, VICE only** (docs/DESIGN.md 52, branch `plain-tr-vectors`): `vector/vdvec.s` hooks the KERNAL's RAM vectors
  (OPEN..CLALL, LOAD, SAVE, IMAIN for re-hooking after RESTOR) on the **stock** KERNAL and chains to the old vectors on "not mine"; same mailbox commands, so the Teensy side is unchanged.
  `vcall`/`bus_scan` now live in `patches/vcall.inc` (+ `patches/mailbox.inc`), shared with the KERNAL patch (images byte-identical). VICE: `make test-vector` (`vector`, `vecscan` with true-drive 1541s) passes.
  1,124 bytes; where it lives on hardware (Teensy-served IO pages with banking vs. a RAM copy) waits on an IO-execution soak test. No firmware gate, no menu install yet.
  The owner's TR+ shorted out (2026-11-05), replacement pending: no hardware runs until then.
* Default device 8 for `LOAD`/`SAVE`/`VERIFY` without a number, as on the SX-64 (`P_E1DA` in `patches/vdrive.s`, docs/DESIGN.md 33). KERNAL only; VICE `basic` run; **not on hardware yet**. Rebuild `kernal-tr.bin`.
* 0.9.0.32: bad sectors from the image's error table (the trailing error-info bytes): reads of a sector marked bad fail with its DOS code (`d64_t.errtab`, `D64_E_BADSEC`, `d64_t.bad`, `read_err()` in `vdrive.c`; the firmware keeps a RAM2 copy only when the table marks something bad; docs/DESIGN.md 35). Write errors (25/26/28) not emulated; a write does not repair a bad sector. Unit and glue host tests; VICE and hardware through VDSUITE `5` (reads only).
* 0.9.0.34: SwiftLink Tx fix to upstream (was `firmware/swiftlink-tx-fix.patch`; merged upstream as PR #80 in `6518b966`, patch dropped). NativeEthernet's `client.write()` busy-waits for send-buffer space;
  with the stack polled from `loop()` (0.9.0.14) a full buffer hung `loop()` for good (no retransmits; SkinnyAI's 8 KB POST stalled after a lost segment, `~/tr.pcap`). Tx bytes now go
  into a 256-byte buffer, sent after a 2 ms pause / 20 ms / full, never more than `availableForWrite()`; while it is full Tx stays not empty. **Works on hardware (owner, 2026-10-29)**: the stalled 8 KB uploads complete.
* 0.9.0.36: the banner's second line shows the drives, ` 64K RAM  38911 FREE  VDRIVE SD 8 USB 9` (38 wide like stock; `SD10`, `NOSD`, `NOUSB`; `patches/vdbanner.s`, docs/DESIGN.md 38); the top line is stock again (the `TRVDRIVE` text is `bringup/banner.s`, only in `kernal-banner.bin`).
  The firmware writes per unit `$FE` absent / fixed number / `$FF` automatic (`0 0` as built = not written: the stock line) after the tag `VDUNITS:` in `KernalFast` (`VD_BannerUnits`, in PreStart and again with the probed numbers in `VD_ProbeFinish`); without the menu probe the KERNAL probes the real bus for automatic units with the hook-free `iec_listen`/`iec_second`/`iec_unlsn`.
  py65 (`tests/test_banner.py`), `tests/fw_host`, VICE (`basic`: `SD 8 NOUSB`; `scan`: `SD 9 USB11`); **not on hardware yet**. Rebuild `kernal-tr.bin`.
* 0.9.0.37: rebased on upstream with [TeensyROM PR 78](https://github.com/SensoriumEmbedded/TeensyROM/pull/78) merged (Kernal Replace senses HIRAM by DMA on a C128: the Skoe sense's A14 never reaches
  the PLA there). The PR carries the same early `HIRAM_State_RAM` return as our fix, so that hunk left `kernalreplace-fixes.patch` (which keeps `KernalFast`, the Ethernet poll and the trace);
  `VD_ProbeFinish` also resets the DMA sense state (`KDS_State`) when it re-hooks `KernalCheck`. Firmware builds (`TeensyROM+_0.9.0.37_full.hex`); the DMA sense is confirmed on a C128 by the TR maintainer; VDrive on an NTSC C128 in C64 mode from 0.9.0.42 (VDSUITE "run all").
* 0.9.0.39: `S:NAME.D64` (`.D71`/`.D81`, a comma list, a path or CMD path) with nothing mounted deletes image files on the card (host callback `scratch`, glue `VD_Scratch`, `cmd_scratch_images` in `vdrive.c`;
  docs/DESIGN.md 39). Exact names only (33 for a pattern), images only (64); a file with the FAT read-only attribute stays (26: SdFat will not open it for writing). `L:` on a card image
  is **not possible**: Teensyduino 1.61's SdFat 2.1.2 has no call that sets the attribute (set it on a PC). Unit, glue host and VICE (VDSUITE `8`) tests; on hardware through VDSUITE `8` (the read-only case is a hand check, not done).
* 0.9.0.41: the Ethernet poll from `loop()` is now upstream [TeensyROM PR 85](https://github.com/SensoriumEmbedded/TeensyROM/pull/85), carried byte for byte as `firmware/kernal-fnet-poll.patch`
  (applied first by `make_tr_patch.sh`; drop it when #85 merges). `kernalreplace-fixes.patch` keeps only `KernalFast` and the `LATE_ISR_TRACE` trace, on top of it. Differences from 0.9.0.40:
  the poll code is a `FLASHMEM` function (`FnetLoopPollService`), guarded by `Fab04_KernalReplace`; plain SwiftLink on the stock KERNAL no longer switches to the loop poll (never measured);
  `make_tr_patch.sh` adds the VDrive Special IOs (`VD_IsHandler`) to the poll list and compiles the `ProcessATCommand` PIT unmask under `Fab04_VDrive` as well (#85 has it only under
  `Fab04_GlobalKernalReplace`), since VDrive+SwiftLink runs AT commands with the poll on. Firmware builds (TR+ main image RAM1 free for locals 66172, padding 25592); fw_host passes; not on hardware.
* 0.9.0.42: rebased on upstream `136baa83` (Kernal Replace hooks `KernalCheck` on a C64, `KernalCheckDMA` on a C128, chosen once per launch by `KernalCheckArm()`), which fixes the
  corrupted directory listings of 0.9.0.37-0.9.0.41 (see Bus timing margin). `kernal-fnet-poll.patch` (#85 rebased: its PreStart hunk sits after `fBusSnoop = KernalCheckArm();`) and
  `kernalreplace-fixes.patch` regenerated on it; `VD_ProbeFinish` re-hooks through `KernalCheckArm()` instead of restoring the saved pointer with `HIRAM_State` Unknown. **Works on hardware (owner, 2026-11-01): directory loads clean again.** VDSUITE "run all" also passes on an NTSC C128 in C64 mode (owner, 2026-11-01).
* 0.9.0.43: **the firmware patches the C64's own KERNAL** (docs/DESIGN.md 40). The VDrive Special IOs no longer use the `<Shift-K>` file (it stays the plain
  Kernal Replace's): `VD_KernalImage` (`IOH_VDrive.c`, through the new `KernalImageSource` hook in `IOH_KernalReplace.c`, set by `KERNALPreStart()`) serves
  `sd:/VDrive/kernal.bin` as it is when present, else reads `$E000-$FFFF` by DMA (`PerformDMA`, 8 x 1 KB) and, if its CRC-32 is on `VD_KnownKernals`
  (901227-03, -02, SX-64 251104-04, C128 C64-mode 325179-01 / 325182-01), lays `VD_KernalPatch` over it (`vd_kpatch.h` from `tools/build.py --table`:
  our bytes only, shipped in the patch). Anything else: nothing served, drives off, serial message. Also **`SYS64738` back to the TR menu**:
  `P_FCE2` jumps to `rst_hook` (33 bytes at `$F875-$F8CF`, the cassette IRQ set-up), which sends `VIO_MENU` only after the new read-only
  ID registers `$DE19/$DE1A` read `$D6 $64` (the menu's handler has SID init bytes there during a launch reset). No off switch (owner's call).
  py65 (`make test-reset`), `test_vio`, `fw_host` (the DMA read patched == `kernal-tr.bin` from the same ROM, unknown / patched / changed ROMs refused,
  `/VDrive/kernal.bin` first), VICE (`basic` ends with `SYS64738` and a reset without the IDs; `make test-variants` runs the five ROMs); firmware builds
  (TR+ main image RAM1 free for locals 66172, padding 25592: unchanged). **Works on a real C64 (owner, 2026-11-02)**: the DMA read of the real chip, the launch reset
  under the menu handler, `SYS64738`; `SYS64738` on a C128 not checked. `KernalCheck` disassembly checked (no push on entry).
* 0.9.0.44: SHIFT+RUN/STOP types `LOAD"*",8,1` CR `RUN` CR (stock: `LOAD` CR `RUN` CR, the tape). `P_E5EE` jumps to `shift_run` (after `rst_hook` in `$F875-$F8CF`), which echoes `LOAD"*`
  through the editor's `$E716` and puts the other 9 keys in the 10-key buffer (no overrun into `$0281+`, unlike the SX-64's 15). docs/DESIGN.md 42. VICE (`basic` and the five ROMs in `variant`);
  **works on a real C64 (owner, 2026-11-02)**. KERNAL patch, so the firmware's table changed (REV 44); rebuild `kernal-tr.bin` for `/VDrive/kernal.bin`.
* 0.9.0.45: `swiftlink-tx-fix.patch` updated to PR #80 after Travis's review: `SwiftTxOut` malloc'd in `InitHndlr_SwiftLink()` / freed in `FreeSwiftlinkBuffs()` (no static RAM2),
  cleared on a connection change, and sent at once when nothing is unacknowledged (`availableForWrite() >= FNET_SOCKET_DEFAULT_SIZE`, Nagle). Builds; **works on a real C64 (owner, 2026-11-02)**.
  Rebased on upstream `6518b966` (#80 merged, byte-identical to our patch): `swiftlink-tx-fix.patch` dropped; the patched tree is identical, so REV stays 45. Builds (`TeensyROM+_0.9.0.45_full.hex`); fw_host passes.
* 0.9.0.46: drive activity LED, contributed by ziggystar12 (creator of Mean Hamster Engine; credited in README and CHANGELOG): the TR's LED (pin 34) blinks 75 ms phases while VDrive services mailbox commands for our units (`VD_LedAccess` after `vio_poll`, not on `VIO_NOTMINE`) and gets its earlier state back 180 ms after the last (`VD_LedPoll` every pass of `PollingHndlr_VDrive`; docs/DESIGN.md 43). Main loop only, no ISR change. `tests/fw_host`; builds; **works on a real C64 (owner, 2026-11-03)**.
* **Remote commands while a program runs can crash it or corrupt data (owner, 2026-11-03; not VDrive bugs, stock firmware too).** Two separate causes:
  (1) **Any USB serial write from the TeensyROM** (a command's reply, a ping's "Busy!") goes through `schedule_transfer()` in the Teensy core's `usb.c`, which runs with
  all interrupts off while it polls USB registers. The phi2 interrupt then answers late (>250 ns, counted with teensyrom-snoop's late-interrupt counter, `trsid.py --isr`),
  enough to crash a timing-tight cartridge (*Ms. Pac-Man* crashes at once under pings over USB; a USB port that is only held open does nothing). VDrive's IO1/IO2 answers
  are as exposed (see Bus timing margin). Experiment, not yet on hardware: `tools/teensy_core_usb_basepri.py` in teensyrom-snoop masks only priority 32 and lower
  there (`BASEPRI`), so the phi2 interrupt at 16 keeps running; it patches the installed Teensyduino core, so a vdrive build made while it is applied gets it too.
  TCP replies do not have this problem.
  (2) **Remote memory reads by DMA** (`ReadC64MemToken`, e.g. `trmon` peeking at the screen) crash or corrupt some programs **over TCP as well**, so it is the DMA reads
  themselves, not the transport. Cause not investigated yet (the CPU is paused for the read; timing-tight code, VDrive transfers in flight and the Kernal Replace HIRAM
  sense are the suspects). Until then: no `trmon`-style peeks during disk access or in timing-tight programs.
* Known open risks: the C128 has passed VDSUITE "run all" (NTSC, C64 mode) but not its hand checks, and a PAL C128 is untested; tape (device 1) intentionally broken (its code space was reclaimed: $F8E2-$FB8D, $FB97-$FCD0).

## Layout
```
d64/            portable C99 library, also compiled as C++ by the firmware (keep it C++-clean!)
  d64.c/.h        D64/D71/D81 engine: geometry, BAM, directory, file chains, create/write/scratch/rename/format, BASIC listing builder
  vdrive.c/.h     virtual IEC drive: channel pool, OPEN/LOAD/SAVE/command channel, CD:/path walking/mount hooks, U0> pins, bus-scan numbers
  vdrive_io.c/.h  mailbox register engine (ISR-side O(1) accessors + vio_poll), the fast LOAD/SAVE commands and the bus-scan protocol
  vd_cfg.h        build knobs: VD_COLD (FLASHMEM), VD_NSLOT, VD_DEV0/VD_DEV1 defaults
util/            VDCOPY: vdcopy.s (all machine code, BASIC SYS stub at $0801, tables BSS at $2800; tools/make_util.py assembles it);
                VDrive Config: vdconfig.s (all machine code, same stub/BSS scheme, BSS $2000-$4FFF; tools/make_util.py assembles it into build/util/"VDrive Config.prg")
vector/         the plain-TeensyROM build: vdvec.s (vector hooks, docs/DESIGN.md 52), vdvec.cfg (PRG at ld65 -S; VICE build at $C000)
patches/        ca65 sources overlaid on the stock KERNAL by tools/build.py; segment name P_XXXX = load address
  vdrive.s        serial entry hooks, fast LOAD / VERIFY ($F4C8, window or DMA) / SAVE ($F605, window or DMA), bus scan, code placed in reclaimed tape space $F8E2-$FB8D (678 of 683 bytes used; `irq_border` for the border flash and the mailbox gate `vd_here` included, the build asserts the limit);
                  the cold-start hook `rst_hook` (`JMP` at $FCE2, code at $F875-$F8CF, 33 of 91 bytes: SYS64738 back to the TR menu)
  vdbanner.s      the drive numbers in the power-on banner (INITMS: `JSR` at $E42D, `JMP` at $E43D, banner string cut at $E49A; code in reclaimed tape routines $FB97-$FCD0, 312 of 314 bytes)
bringup/banner.s  proof patch (`TRVDRIVE 64 BASIC V2`), only in `make build/kernal-banner.bin`
firmware/       TeensyROM integration: IOH_VDrive.c (Special IO), IOH_VDriveSwift.c / IOH_VDriveREU.c / IOH_VDriveSwiftREU.c (combined Special IOs) and
                teensyrom-vdrive.patch (git apply onto TeensyROM); vdrive_probe.asm (the TR menu's launch-time bus probe, ACME, inserted into MainMenu.asm)
tools/          build.py (patch overlay; `--table` writes the patches as `vd_kpatch.h` for the firmware), make_tr_patch.sh (regenerate the TeensyROM patch), vdrive_mock.py (VICE mock),
                d64py.py (ctypes wrappers incl. register-level VIO), xref.py (reference finder over a disassembly)
tests/          test_d64*.py, test_vdrive.py, test_vio.py (unit), fw_host/ (glue vs host stubs), devcfg.c,
                test_vdconfig.py + c64_sim.py (VDrive Config's PRG in py65, KERNAL calls answered in Python), test_vdcopy.py (VDCOPY in py65 against vdrive.c),
                test_irq_border.py, test_banner.py, test_reset_hook.py, test_vcall_gate.py, test_dmatest.py (py65),
                menuprobe_vice.py (the menu's probe routine in VICE with true-drive 1541s), variants_vice.py (the patch on each whitelisted ROM in VICE)
build/          generated, git-ignored (kernal images derived from the ROM, libd64.dylib, disassembly)
```

## Commands
* `make test` — everything (rebuilds `build/libd64.dylib`, runs unit tests, device-number check, firmware host test, VDrive Config in py65, three VICE runs: `basic`,
  `scan` (true-drive 1541s at 8 and 10, with two VDCOPY whole-disk runs over tracks 17-19, about 2 minutes of it) and the menu's bus probe; about 13 minutes in all on the owner's Mac, measured 2026-10-31).
* `make test-gate` — the mailbox gate `vd_here` in py65 (no handler: no IO write, the real bus at once; late ID reads; skipped without py65; also part of `make test`).
* `make test-vector` — the plain-TeensyROM vector hooks (`build/vdvec-vice.prg`) on the stock KERNAL in VICE, mock scenarios `vector` and `vecscan` (part of `make test`).
* `make test-reset` — the cold-start hook `rst_hook` (SYS64738 back to the menu) in py65 (skipped without py65; also part of `make test`).
* `make test-variants` — the patch on every ROM in `VD_KnownKernals`, in VICE (mock `variant` scenario); the ROMs are looked up next to `STOCK`
  (VICE's `C64/` and `C128/` data folders), missing ones skipped. Part of `make test`. `make test-fw` passes `STOCK` so the glue's in-memory patch is compared with `kernal-tr.bin`.
* `make test-dmatest` — DMATEST (`tests/asm/dmatest.s`, the TR+ DMA-write check run with `tools/dmatest.py`) in py65 (skipped without py65; also part of `make test`).
* `make test-border` — the `irq_border` 6502 routine in py65 (skipped without `pip install py65`; also part of `make test`).
* `make test-banner` — the banner's `vd_banner` in py65 with a fake serial bus (skipped without py65; also part of `make test`).
* `make util` — `build/util/vdcopy.prg`, the file and whole-disk copy utility between any two units (also on the test disk), and `build/util/VDrive Config.prg`.
* `make test-vdconfig` — VDrive Config's PRG in py65 against a drive model (skipped without py65; also part of `make test`).
* `make test-vdcopy` — VDCOPY's PRG in py65 against the real drive code on both units (skipped without py65; also part of `make test`).
* `make test-menuprobe` — the TR menu's bus probe routine in VICE, 1541s at 8 and 10 (skipped without `acme`; also part of `make test`).
* `make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin` — hardware image (mailbox `$DE10`, window `$DF80`). `build/kernal-tr-nodma.bin`: the same without the DMA LOAD, SAVE and VERIFY requests (upstream's).
  `STOCK` defaults to VICE's bundled ROM under Homebrew. `make build/kernal-banner.bin`, `build/kernal-stock.bin` are bring-up images.
* `tools/build_firmware.sh tr|tr-plus|tr-plus-dma [outdir] [--install-toolchain]` — release hex: fetches the TeensyROM commit in `firmware/UPSTREAM`, applies
  `firmware/teensyrom-vdrive.patch`, commits it (clean tree) and runs TeensyROM's `tools/build-firmware.mjs` (node >= 24). `tr`/`tr-plus` are built without DMA LOAD
  (the define dropped and `vd_kpatch.h` rebuilt with `-D VD_DMA_LOAD=0`: the same tree as `VD_DMA_LOAD=0 make_tr_patch.sh`, checked for 0.9.0.52); `tr-plus-dma` keeps
  it and stamps the version `<ver>-DMA`. **Update `firmware/UPSTREAM`
  whenever the patch is rebased** onto another upstream commit. The plain `tr` hex has no VDrive yet (`Fab04_VDrive` sits under `Fab04_Features`; a TR port is on its own branch).
* `tools/make_tr_patch.sh /path/to/TeensyROM` — regenerate `firmware/teensyrom-vdrive.patch` after changing `firmware/IOH_VDrive.c`
  or anything in `d64/` (the patch embeds copies of those files). Always re-run it, then `git apply --check` on a clean checkout. Run it against a *pristine* upstream tree (`git archive HEAD` of the TeensyROM checkout), not the owner's already-patched one: its edits assert on unpatched text. It also reads `firmware/vdrive_remote.inc` (the remote-control dispatcher inserted into `SerUSBIO.ino`) and `firmware/vdrive_probe.asm`, and rebuilds the menu header with the TR's own `tools/build-c64.mjs` (needs `acme` 0.97 and `node`).
* Requires: `ca65/ld65` (cc65), VICE (`x64sc`, `c1541`), a C/C++ compiler, Python 3; `acme` 0.97 for `make test-menuprobe` and, with `node`, for `make_tr_patch.sh`.

## Testing in the cloud (Linux container, no Mac)
The full `make test`, VICE runs included, works in a cloud session; do not skip the VICE part for want of a ROM.
* `tools/cloud_setup.sh` once per container: installs `vice cc65 acme xvfb clang` and `py65`, and puts the ROMs that VICE's upstream source tree ships
  (`data/C64`, `data/DRIVES`, from the VICE source mirror on GitHub) in `~/.local/share/vice`, where `x64sc` finds them. Ubuntu's `vice` package is the
  DFSG build without ROMs, so `apt` alone is not enough. The ROMs stay outside the repository (no Commodore ROM bytes in the repo).
* Then: `eval "$(tools/cloud_setup.sh --env)"; xvfb-run -a make test STOCK="$STOCK"`. `STOCK` = VICE's `kernal-901227-03.bin` is the base for every
  KERNAL image (`kernal-vd.bin`, `kernal-tr.bin`), as the Homebrew default is on the owner's Mac. `xvfb-run` gives `x64sc` a display. About 7 minutes when last timed, before the menu-probe and VDCOPY whole-disk runs were added; expect longer now.
* The `--env` line also puts a `c++` → `clang++` link first on `PATH`: with GCC, `tests/fw_host` fails to compile on existing glue code
  (`-Wextra` enum/`-Wformat-truncation` under `-Werror`); the owner's compiler is clang.
* Still say in the pull request what was not run: a VICE run is not a hardware run.
* **GitHub Actions** (`.github/workflows/test.yml`) runs the same setup and `make test` on every push to `main`/`bleeding-edge` and every pull request,
  plus `make build/kernal-tr.bin` and `make util`. `cloud_setup.sh` uses `sudo` when not root; py65 comes from `actions/setup-python` (`python3 -I` skips the user site).
* **Releases** (`.github/workflows/release.yml`): publishing a GitHub release builds `tr`, `tr-plus` and `tr-plus-dma` with `tools/build_firmware.sh` and attaches
  `TeensyROM_<ver>_full.hex`, `TeensyROM+_<ver>_full.hex` and `TeensyROM+_<ver>-DMA_full.hex`; a tag `v<ver>` must match the built version (bump `firmware/REV` first). A manual run, or a push touching `firmware/`, `d64/` or the build script, builds all three as an artifact only.

## Gotchas
* **Firmware version stamp:** our builds are `<stock>.<REV>` (stock 0.9 -> `0.9.0.1`), written into `TRVersion` by `tools/make_tr_patch.sh` from
  `firmware/REV`. **Bump `firmware/REV` (and regenerate the patch) on any commit that changes firmware behaviour** (`d64/`, `firmware/`, the
  patch edits). It shows on the C64 menu title, in the hex file name (`TeensyROM+_0.9.0.1_full.hex`, never confused with stock
  `0.9_full.hex`) and in the flash tool. Keep it digits-and-dots: `tools/lib/trserial.mjs` parses `TeensyROM+? v[\d.]+`.
* **CHANGELOG with every user-visible change, in the same commit.** Every firmware version (`firmware/REV`) gets its own section,
  `## v0.9.0.<REV>`, newest first, written in the commit that bumps REV; no `Unreleased` section. A change that needs no REV of its own
  (CI, tools, docs, tests) goes into the section of the next version: write that section header early if needed. Entries go under
  `### Added` / `### Changed` / `### Fixed` / `### Build`, with what the user sees, the test status in the same words as here
  (host/VICE/**hardware**) and the `docs/DESIGN.md` section. When the owner reports a hardware result, update the entry as well as this
  file. A fix to a regression names the versions that had the bug, and the version that caused it says so. Entries written afterwards
  from memory get missed (0.9.0.51/0.9.0.52 needed a catch-up commit). Before every commit: `git diff --cached --stat` shows
  `CHANGELOG.md` whenever `firmware/REV` is in it.
* **Keep the patch hunks away from lines upstream is changing.** `tools/make_tr_patch.sh` places edits so they apply on current `main` *and* on the open TeensyROM PRs that touch the same files
  (written for #39 and #61-#70, all merged by `5728ddc9`; checked at the 0.9.0.38 rebase on `fa509421` against open #79, #81-#83; #80, our SwiftLink Tx fix, merged in `6518b966`; #85 is carried as `kernal-fnet-poll.patch` until it merges, 0.9.0.41 checked against #81-#83): the VDrive entries in the `enumIOHandlers` / `IOHandler[]` lists sit *before* the Kernal Replace entries (upstream appends GeoRAM after `IOH_Debug`),
  the Busy-refusal bypass in `SerUSBIO.ino` is a block after the `ASIDAllowed` statement (not a rewrite of the `if`), the menu remap in `IOHandlers.ino` is an `IOHandlerInit` macro defined before `IOHandlerSelectInit()` and `#undef`'d after it (0.9.0.38: #81 rewrites the function's first lines; the old case ahead of upstream's call also bypassed its GEOS case), and `FnetPollHold = false` follows the `#endif`
  in `SetUpMainMenuROM()`. Test a regenerated patch with `git apply --check` on `git archive origin/main` and on `git merge-tree --write-tree origin/main pr/N` trees. The Special IO list order is VDrive, VDrive+Swift,
  VDrive+Swift+REU, VDrive+REU, Kernal Replace, Debug (0.9.0.31): a stored default Special IO (`eepAdNextIOHndlr`, an index) of Kernal Replace from an older build now selects a VDrive entry.
* **Published KERNAL hash:** the README's Installing section lists the SHA-256 of `kernal-tr.bin` built from the 901227-03 ROM; `make test-hash` (part of `make test`) fails when a patch or `tools/build.py` changes the image and the "from this revision" row was not updated. Update the "last run on a real C64" row only when the owner reports a hardware run of that image.
* **No Commodore ROM bytes in the repo.** Only patches. `build/` is ignored for that reason; keep it that way. The firmware's `vd_kpatch.h` holds our segments only
  (`tools/build.py --table` never reads a ROM); the firmware's ROM whitelist is CRC-32s. Adding a ROM to `VD_KnownKernals` means adding it to `tests/variants_vice.py` too (it checks both lists agree).
* **Two background VICE runs collide**: every mock run uses VICE's binary monitor on port 6502, and a second mock connects to the first one's VICE.
  Run `vdrive_mock.py` / `variants_vice.py` / `menuprobe_vice.py` one after another. Another checkout's tests (a worktree session) can hold 6502 too: `VD_MONITOR_PORT=6510 make test` keeps out of their way.
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
1. **Finish bring-up**: VDSUITE's "checks by hand" (`10`; the "Not yet checked on hardware" list under Status). The core path has passed, so the items below can
   start once the owner agrees; mind that the patch changes under any build in progress.
2. **Create new images**: done in 0.9.0.24 (`N:FILE.D64,ID` with nothing mounted; docs/DESIGN.md 29), works on hardware, a D81 on a slow USB stick included
   (about 0.8 s against the KERNAL's ~1.6 s mailbox wait).
3. **BASIC utility** (~40 lines, kept as `.bas` text and converted with `petcat` in the Makefile): list the current directory, pick by number (mount an
   image, open a folder, `..`), "new image", "unmount", and show the device numbers (`PEEK(56854)`). Much of what it would do is now one `LOAD"$path"`.
4. **TeensyROM menu integration** (last, only if the BASIC utility feels clumsy): a `Shift-D` hotkey like the existing `Shift-K`
   (KERNAL image) / `Shift-R` (REU) that sets the highlighted file as the disk for device 8/9. Needs changes to the 6502 menu
   (`Source/C64/MainMenuCRT`, ACME) and regenerated headers, and probably an EEPROM map extension that upstream must agree to.
5. Smaller follow-ups: D81 sub-directories/partitions, writing plain files to the card,
   a scan that re-runs after a device is switched on later.
