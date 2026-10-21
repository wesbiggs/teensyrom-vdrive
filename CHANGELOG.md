# Changelog

## Unreleased

### Added
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
