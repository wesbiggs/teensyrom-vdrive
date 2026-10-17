# CLAUDE.md — TR+ virtual-drive KERNAL

## What this is
A replacement C64 KERNAL (901227-03 + patches) that the **TeensyROM+ (TR+)** serves live, plus the Teensy-side firmware that makes
devices 8 (SD) and 9 (USB) behave as IEC drives backed by `.d64` images. KERNAL serial calls for those devices go to a mailbox
(IO1 `$DE00` registers + a 256-byte IO2 window at `$DF00`) instead of the bit-banged bus; any other device number still uses the real
IEC code, so physical drives keep working. LOAD/SAVE have block-transfer fast paths. Device numbers default to 8/9 and are changed
from BASIC with `OPEN15,8,15,"U0>"+CHR$(n)` (kept in EEPROM, no reflash).

Full design and per-milestone notes: `DESIGN.md` (sections 1-14, in order of construction). Hardware plan: `docs/BRINGUP.md`.
Firmware integration and usage: `firmware/README.md`.

## Status (read this first)
* **Host-verified only.** `make test` passes: C unit tests via ctypes against `c1541`, the 6502 patches in VICE through a
  monitor-driven mailbox mock, and the firmware glue compiled as C++ against host stubs.
* **Never built against the real Teensy toolchain, never run on a TR+** as of the first commit. The owner is doing the first build
  and flash now (toolchain: `arduino-cli`, `teensy:avr@1.61.0`, `CRC32@2.0.1`; do NOT use Teensyduino 1.62.0 — SD read stalls).
  Follow `docs/BRINGUP.md` stage by stage and record results in its table; update this section as stages pass.
* **Stage 0-1 done (build + fit):** the patched `[main]` image builds and links with the real toolchain. Cost vs stock: RAM1 padding
  −976 B (13,816 → 12,840), flash code +15.7 KB, RAM2 +4.5 KB (the drive state). RAM1 code is +976 B, more than the ~130 B hot-code
  estimate: the two `FASTRUN` handlers plus the glue's sector callbacks/helpers; could move those to `FLASHMEM` if RAM1 ever gets tight.
  **Stage 2-3 passed on hardware** (handler listed, drive detection for all SD/USB combinations, mailbox registers and the fast `$` open
  via BASIC `POKE`/`PEEK`). **Stage 5 (stock KERNAL control) passed** once the C128 was booted in C64 mode (a C128 in 128 mode cannot use a
  C64 KERNAL replacement). Stage 6 banner test **cannot work via F2**: the TR menu prints the banner before the KERNAL hook goes live, so use
  `SYS 58402` ($E422) to reprint it from the served image. Observed: with the banner image the keyboard died; suspected cause was SD/USB
  probing in `InitHndlr_VDrive` while the C64 already runs from the replacement KERNAL (SD reads mask interrupts). Fixed by moving all
  storage access into `VDrive_PreStart()`, called from `KERNALPreStart()` while the C64 still waits. **Needs re-test on hardware.**
  Stages 4 and 6-8 are still to do.
* Known open risks: `File::seek/read/write/flush` semantics on the
  real SD/USB classes, IO1/IO2 ISR timing, tape (device 1) intentionally broken (its code space was reclaimed).

## Layout
```
d64/            portable C99 library, also compiled as C++ by the firmware (keep it C++-clean!)
  d64.c/.h        D64 engine: geometry, BAM, directory, file chains, create/write/scratch/rename/format, BASIC listing builder
  vdrive.c/.h     virtual IEC drive: channel pool, OPEN/LOAD/SAVE/command channel, CD:/mount hooks, U0> renumber
  vdrive_io.c/.h  mailbox register engine (ISR-side O(1) accessors + vio_poll) and the fast LOAD/SAVE commands
  vd_cfg.h        build knobs: VD_COLD (FLASHMEM), VD_NSLOT, VD_DEV0/VD_DEV1 defaults
patches/        ca65 sources overlaid on the stock KERNAL by tools/build.py; segment name P_XXXX = load address
  vdrive.s        serial entry hooks, fast LOAD ($F4C8) / SAVE ($F605), code placed in reclaimed tape space $F8E2-$FB8D
  banner.s        proof patch (changes the power-on banner)
firmware/       TeensyROM integration: IOH_VDrive.c (Special IO) and teensyrom-vdrive.patch (git apply onto TeensyROM)
tools/          build.py (patch overlay), make_tr_patch.sh (regenerate the TeensyROM patch), vdrive_mock.py (VICE mock),
                d64py.py (ctypes wrappers incl. register-level VIO), xref.py (reference finder over a disassembly)
tests/          test_d64*.py, test_vdrive.py, test_vio.py (unit), fw_host/ (glue vs host stubs), devcfg.c
docs/BRINGUP.md staged hardware bring-up checklist
build/          generated, git-ignored (kernal images derived from the ROM, libd64.dylib, disassembly)
```

## Commands
* `make test` — everything (rebuilds `build/libd64.dylib`, runs unit tests, device-number check, firmware host test, VICE run).
* `make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin` — hardware image (mailbox `$DE00`, window `$DF00`).
  `STOCK` defaults to VICE's bundled ROM under Homebrew. `make build/kernal-banner.bin`, `build/kernal-stock.bin` are bring-up images.
* `tools/make_tr_patch.sh /path/to/TeensyROM` — regenerate `firmware/teensyrom-vdrive.patch` after changing `firmware/IOH_VDrive.c`
  or anything in `d64/` (the patch embeds copies of those files). Always re-run it, then `git apply --check` on a clean checkout.
* Requires: `ca65/ld65` (cc65), VICE (`x64sc`, `c1541`), a C/C++ compiler, Python 3.

## Gotchas
* **No Commodore ROM bytes in the repo.** Only patches. `build/` is ignored for that reason; keep it that way.
* **The ctypes tests load `build/libd64.dylib`.** After editing `d64/*.c` run `make build/libd64.dylib` (or `make test`) first, or you
  test a stale library.
* The firmware compiles `.c` files inside the `.ino` as **C++**: no implicit `void*` conversions, etc. `make` runs a `-x c++`
  syntax check on the library, and `tests/fw_host/run.py` compiles the glue with `-Wall -Wextra -Werror`.
* Anything not reachable from the bus ISR must be `VD_COLD` (flash). Only `vio_io1_read/write` and `vio_io2_read/write` stay in RAM1.
* `U0>` is parsed before the CR/LF strip because devices 10 and 13 are LF and CR.
* The KERNAL stubs write `$A5-$A7` (vcall) and `$A3/$A4` (fast paths); these are serial-bus scratch bytes, free outside bit-banging.
* Names are PETSCII throughout the C code; the glue converts to/from ASCII for the host file system.
* PETSCII quirk when building fixtures with `c1541`: give it lower-case names to get unshifted (default-mode upper-case) PETSCII.

## Next steps
1. **Finish first-version bring-up** on the TR+ (`docs/BRINGUP.md` stages 0-8); fix whatever the real toolchain/hardware turn up.
   Do not start the items below until the owner reports stage 7 passing, so the patch isn't changing under a build in progress.
2. **Create new images + remember mounts** (agreed design, not started):
   * With nothing mounted, `N:FILE.D64,ID` creates a blank 35-track image on the host file system and mounts it (glue + `vdrive.c`
     `N` command; today `N` only formats the mounted disk, and nothing creates a `.d64` file).
   * Persist the last mounted image path per unit in a small sidecar file on the card and re-mount at start-up (EEPROM only has ~49
     reserved bytes — not enough for paths). Tests: mount/create via the host-op callbacks in `tests/test_vio.py` and the stub
     filesystem in `tests/fw_host/`.
3. **BASIC utility** (~40 lines, kept as `.bas` text and converted with `petcat` in the Makefile): list the current directory, pick by
   number (mount an image, open a folder, `..`), "new image", "unmount". No menu or KERNAL changes needed.
4. **TeensyROM menu integration** (last, only if the BASIC utility feels clumsy): a `Shift-D` hotkey like the existing `Shift-K`
   (KERNAL image) / `Shift-R` (REU) that sets the highlighted file as the disk for device 8/9. Needs changes to the 6502 menu
   (`Source/C64/MainMenuCRT`, ACME) and regenerated headers, and probably an EEPROM map extension that upstream must agree to.
5. Smaller follow-ups: append mode (`,A`), `V`alidate, REL files, plain `.prg` loading straight from the card, file names > 16
   characters in host listings, refusing to claim a device number that a physical drive already uses.
