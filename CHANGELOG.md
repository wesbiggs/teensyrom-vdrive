# Changelog

## v0.9.0.28

### Added
- "VDrive+Swift+REU" Special IO: the virtual drive, SwiftLink and a 128 KB REU at the same time (firmware 0.9.0.28, `docs/DESIGN.md` section 32). With it as the default Special IO, menu programs paired with SwiftLink or the REU get it. Host tests, VICE, a firmware build, and works on a real C64.

- `MD:NAME` / `RD:NAME` make and remove (empty) folders on the card when nothing is mounted (CMD syntax; firmware 0.9.0.26, `docs/DESIGN.md` section 31). With an image mounted they answer 31 (no D81 partitions). Unit and glue host tests, not on hardware.
- Plain files on the card: with nothing mounted, `LOAD"GAME.PRG",8` (also `"GAME"`, `"GA*"`, `"DIR/GAME.PRG"`) loads the file itself; reading only (firmware 0.9.0.25, `docs/DESIGN.md` section 30). Unit and glue host tests, not on hardware.
- `N:NAME.D64,ID` (or `.D71` / `.D81`) with nothing mounted creates a new blank image in the current folder (or `N:/DIR/NAME.D64`), mounts it and formats it; an existing file is never replaced (firmware 0.9.0.24, `docs/DESIGN.md` section 29). Unit and glue host tests, not on hardware: time a D81 on a USB stick against the KERNAL mailbox wait.
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
- `VDCOPY` (`util/`, `make util`, also on `vdtest.d64`): copies files from a real drive on the serial bus to a vdrive unit.
  BASIC front end (directory list, `A` / `1,3,5-8` selection, optional replace) with a 2 KB-chunk machine-code copy loop;
  RUN/STOP aborts cleanly and removes the partial file. Tested in VICE with a true-drive 1541; not yet on hardware.

### Changed
- The KERNAL's IO2 window is now 128 bytes at `$DF80` (was 256 at `$DF00`), so the REU's registers fit beside it; fast LOAD / SAVE move 128-byte blocks, and names of 128 characters or more open through the byte-level path. **Rebuild `kernal-tr.bin` together with firmware 0.9.0.28**: an older image does not match.

### Fixed
- Real IEC drives never received UNTALK: the UNLSN hook at `$EDFE` overwrote the operand of the `BIT` that stock UNTLK
  falls through, so the drive kept talking and later byte reads (`GET#`, `INPUT#`) were garbled. `hk_untlk` now spells out
  the whole routine. KERNAL image only; rebuild `kernal-tr.bin`; `firmware/REV` stays 14.
- Real IEC drives (e.g. a 1541-II as device 9): every byte read through the ACPTR hook ran `LDA $20 / STA $EE` instead of
  the stock `STA $A5`, because the hook jumped into the operand byte (`$EE17`) rather than the instruction (`$EE16`).
  `$EE` is in the screen line-link table, so `LOAD"$",9` left earlier screen lines shifted/indented, and the clock-line
  setup (`JSR $EE85`) was skipped. KERNAL image only (`patches/vdrive.s`); no firmware change, so `firmware/REV` stays 14.

## v0.9.0.14
- First public version
