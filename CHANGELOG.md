# Changelog

## Unreleased

### Changed
- `VDrive Config.prg` is now machine code (`util/vdconfig.s`, assembled by `make util`; no petcat needed) instead of BASIC: the same screen and
  keys, but reading the list, the directory listings of the browser and every redraw no longer crawl through BASIC string handling.
  It now reads the device numbers after a first bus access (as VDCOPY does), so with a real drive on the bus it talks to the numbers the
  bus scan gave the SD / USB units, not their defaults. `tests/test_vdconfig.py` runs the real PRG in py65 (`tests/c64_sim.py` answers its
  KERNAL calls) against the same drive model; not on a C64 or in VICE yet.

## v0.9.0.33

### Added
- CMD HD / sd2iec path syntax in names, listings and `CD`/`MD`/`RD`/`N`: a path before the colon, `//` = card root, a single `/` =
  current folder (`LOAD"//GAMES/G.D64/:START"`, `LOAD"$//GAMES/:*"`, `CD//GAMES`, `CD/SUB/:INNER`, `CD0//:GAMES`,
  `MD//GAMES/:NEW`, `N//GAMES/:NEW.D64,ID`; `docs/DESIGN.md` section 36). `MD`/`RD` with a path leave the current folder as it was.
  Unit tests; not in VICE or on hardware.

### Changed
- **`CD/NAME` (no colon) is now relative**, as on a CMD drive; before it started at the card root. `CD:/NAME`, `CD//NAME` and
  `LOAD"$/GAMES/..."` still start at the root. `CD/` alone is now the current folder (nothing happens) instead of the root.

## v0.9.0.32

### Added
- Bad sectors from the image's error table: a `.d64` / `.d71` / `.d81` with the trailing error-info bytes (175,531 / 197,376 /
  351,062 / 822,400 bytes) now fails reads of the sectors it marks bad, as a real drive does, so copy protection that reads a bad
  sector with `U1` and checks the error channel works. Each byte maps to the DOS error (`2`..`6`, `9`, `11`, `15` = `20`..`24`, `27`,
  `29`, `74`); the error channel shows it with the bad sector's track and sector (`23,READ ERROR,17,00`). A `U1` of a bad sector still
  fills the buffer with the image's data, as VICE does. A `LOAD` or `GET#` stops at the bad sector (fast `LOAD`: `ST` = `$42`), and a bad
  directory or BAM sector cuts the listing or fails the write. The write errors in the table (`25`, `26`, `28`) are not emulated, and
  writing to a bad sector does not repair it. The firmware keeps the table in RAM2 (683 to 3,200 bytes) only when it marks a sector bad
  (`docs/DESIGN.md` section 35). Unit and firmware glue host tests; not in VICE or on hardware.

## v0.9.0.31

### Added
- Special IO **VDrive+REU**: the virtual drive and the full **512 KB** REU together (VDrive+Swift+REU only has room for 128 KB;
  `docs/DESIGN.md` section 34). With any VDrive Special IO as the default, menu programs paired with the REU now start with it
  instead of the plain REU (or the 128 KB one of VDrive+Swift+REU), so they keep the drive. Host tests and firmware build; not on hardware.

### Changed
- The Special IO list is VDrive, VDrive+SwiftLink, VDrive+Swift+REU, VDrive+REU, Kernal Replace, Debug:
  **if your stored default Special IO was Kernal Replace, choose it again in Settings.**

## v0.9.0.30

### Changed
- The TeensyROM patch is rebased onto upstream `5728ddc9` (196 commits, mostly GEOS: GeoRAM Special IO, RAM bank refactor,
  USB mouse, TR block I/O). Only a context line moved in `firmware/kernalreplace-fixes.patch` (`DMAWaitAssertReady` in `ISRs.c`);
  the patch still applies with the open upstream PRs (#60, #62, #63) merged. GeoRAM sits after Debug in the Special IO list, so
  stored VDrive defaults keep their index. Firmware build only; not run on hardware.

## v0.9.0.29

### Added
- `LOAD`, `SAVE` and `VERIFY` without a device number use device 8, as on the SX-64 (the stock default, tape, only hung at
  PRESS PLAY ON TAPE since its code space is reused; `docs/DESIGN.md` section 33). `OPEN` keeps its own default. KERNAL only:
  **rebuild and re-copy `kernal-tr.bin`**. Tested in VICE; not on hardware.
- The README's Installing section publishes the SHA-256 of the stock ROM, of `kernal-tr.bin` built from this revision and of the
  last image run on a real C64, so you can check that you built the tested image. `make test-hash` (part of `make test` and CI) checks the row.

### Changed
- The TeensyROM patch is rebased onto upstream `57a9d76` and placed so it also applies with the open upstream PRs merged
  (#39, #61-#70). The Special IO list order is now VDrive, VDrive+SwiftLink, VDrive+Swift+REU, Kernal Replace, Debug:
  **if your stored default Special IO was Kernal Replace, it now selects a VDrive entry; choose it again in Settings.**
  Firmware build only; not run on hardware.

## v0.9.0.28

### Added
- Direct sector access (firmware 0.9.0.16, `docs/DESIGN.md` section 21): `OPEN n,d,n,"#"` buffer channels, `U1`/`UA` and `U2`/`UB`
  (`B-R`/`B-W` as aliases), `B-P`, `B-A` / `B-F`; D64, D71 and D81. Unit tests and `BLKTEST` in VICE; not on hardware.
- "VDrive+Swift+REU" Special IO: the virtual drive, SwiftLink and a 128 KB REU at the same time (firmware 0.9.0.28, `docs/DESIGN.md` section 32). With it as the default Special IO, menu programs paired with SwiftLink or the REU get it. Host tests, VICE, a firmware build, and works on a real C64.

- `MD:NAME` / `RD:NAME` make and remove (empty) folders on the card when nothing is mounted (CMD syntax; firmware 0.9.0.26, `docs/DESIGN.md` section 31). With an image mounted they answer 31 (no D81 partitions). Unit and glue host tests, not on hardware.
- Plain files on the card: with nothing mounted, `LOAD"GAME.PRG",8` (also `"GAME"`, `"GA*"`, `"DIR/GAME.PRG"`) loads the file itself; reading only (firmware 0.9.0.25, `docs/DESIGN.md` section 30). Unit and glue host tests, not on hardware.
- `N:NAME.D64,ID` (or `.D71` / `.D81`) with nothing mounted creates a new blank image in the current folder (or `N:/DIR/NAME.D64`), mounts it and formats it; an existing file is never replaced (firmware 0.9.0.24, `docs/DESIGN.md` section 29). Unit and glue host tests; works on a real C64. Still to check: the time a D81 takes on a USB stick against the KERNAL mailbox wait.
- `L:name` toggles the lock flag of matching files, `W-1` / `W-0` turn write protection of the mounted image on / off (CMD drive syntax; firmware 0.9.0.23, `docs/DESIGN.md` section 28). Unit tests only, not in VICE or on hardware.
- `V` (validate) rebuilds the BAM from the files, REL side sectors included, and deletes unclosed files; it used to do nothing (firmware 0.9.0.22, `docs/DESIGN.md` section 27). Byte-identical to `c1541 -validate` in the unit tests; not in VICE or on hardware.
- Append mode: `OPEN n,d,n,"NAME,A"` (or `"NAME,S,A"`) adds to the end of an existing file; a disk full puts the file back as it was (firmware 0.9.0.21, `docs/DESIGN.md` section 26). Unit tests only, not in VICE or on hardware.
- REL (relative) files: `OPEN n,d,n,"NAME,L,"+CHR$(len)`, `P` positioning, one record per `PRINT#`, reads up to the last non-zero byte; D64/D71 up to 720 blocks, D81 with super side sectors; `C` and `S` handle them (firmware 0.9.0.20, `docs/DESIGN.md` section 25). Unit tests only, not in VICE or on hardware.
- `C:new=old[,old2...]` copies a file on the mounted image, or joins up to 8 files into one (firmware 0.9.0.19, `docs/DESIGN.md` section 24). A failed copy leaves the disk unchanged. Unit tests only, not in VICE or on hardware.
- `VDrive Config.prg` (`util/vdconfig.bas`, built by `make util`; firmware 0.9.0.18, `docs/DESIGN.md` section 23): a text-screen BASIC utility to
  add / edit / delete / reorder the entries of `/vdrive-config.txt` for the SD and USB drives, show what each drive has mounted, and mount
  any image (from the list, or by browsing the card). New channel names `"!CONFIG"` (read, `,W` write) and `"!STATE"` give a C64 program the
  list file and the mounted image path (`vd_host_t` hooks `hf_*`). Tested in a small BASIC interpreter model on the host
  (`tests/test_vdconfig.py`), not on a C64 yet.
- Alt-button disk switching (firmware 0.9.0.17, `docs/DESIGN.md` section 22). With the TR's Alt button setting on "None", one tap
  mounts the next image listed in `/vdrive-config.txt` on the SD card, two taps within 1 s the next one from the same file on
  the USB stick (`/path/IMAGE.D64, colour, flashes` per line; missing images are skipped, nothing mounted = first entry), and
  the border flashes the entry's colour that many times (250 ms on, 750 ms off); a missing card or stick, a missing or empty
  list or nothing usable flashes red for 2 s. Needs the new KERNAL image (`irq_border` in `patches/vdrive.s`, new `BORDER`
  mailbox register `$DE18`) together with the new firmware; an old KERNAL image only lacks the flash.
- `VDCOPY` (`util/`, `make util`, also on `vdtest.d64`): copies files in 2 KB chunks between any two units (real drive, SD, USB;
  either direction). Directory list, `A` / `1,3,5-8` selection, optional replace; RUN/STOP aborts cleanly and removes the partial file.
  First a BASIC program with a machine-code copy loop (worked on a real C64: vdrive to vdrive and vdrive to a 1541), now all machine code
  (tested in VICE with a true-drive 1541 in every direction; not yet on hardware).
- `make test` runs on GitHub Actions for every push and pull request, and in cloud sessions with VICE's own ROMs (`tools/cloud_setup.sh`).

### Changed
- The Ethernet stack is polled from the main loop from `PreStart` on (firmware 0.9.0.15), not only once the handler runs: the late
  interrupts in the first 0.4 s after a launch had crashed *Quest for the King*.
- The KERNAL's IO2 window is now 128 bytes at `$DF80` (was 256 at `$DF00`), so the REU's registers fit beside it; fast LOAD / SAVE move 128-byte blocks, and names of 128 characters or more open through the byte-level path. **Rebuild `kernal-tr.bin` together with firmware 0.9.0.28**: an older image does not match.

### Fixed
- With an image mounted, a `/` inside a file name is literal (a legal CBM name such as `A/B`); only a leading `/`, a component
  with an image suffix or `..` makes it a path (firmware 0.9.0.27). Before, `OPEN"A/B"` tried to enter a folder `A` and failed.
- Real IEC drives never received UNTALK: the UNLSN hook at `$EDFE` overwrote the operand of the `BIT` that stock UNTLK
  falls through, so the drive kept talking and later byte reads (`GET#`, `INPUT#`) were garbled. `hk_untlk` now spells out
  the whole routine. KERNAL image only; rebuild `kernal-tr.bin`; `firmware/REV` stays 14.
- Real IEC drives (e.g. a 1541-II as device 9): every byte read through the ACPTR hook ran `LDA $20 / STA $EE` instead of
  the stock `STA $A5`, because the hook jumped into the operand byte (`$EE17`) rather than the instruction (`$EE16`).
  `$EE` is in the screen line-link table, so `LOAD"$",9` left earlier screen lines shifted/indented, and the clock-line
  setup (`JSR $EE85`) was skipped. KERNAL image only (`patches/vdrive.s`); no firmware change, so `firmware/REV` stays 14.

## v0.9.0.14
- First public version
