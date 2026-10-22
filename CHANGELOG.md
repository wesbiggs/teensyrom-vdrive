# Changelog

## Unreleased

### Added
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
