# Changelog

## v0.9.0.49

### Added
- **The banner's drive numbers with a VDrive KERNAL in the machine itself** (EPROM, MEGA65 core): `VDRIVE SD 8 USB 9` instead of the
  stock line. The menu prints the banner itself before the program loads, while its own handler answers IO1, so the firmware leaves
  the same two unit bytes it writes into a served image in four unused menu registers (`$DE3C-$DE3F`: tag `$D6 $64`, SD, USB), and
  clears them for a launch with another Special IO. `docs/DESIGN.md` section 46; py65 and host tests; not on hardware.

### Changed
- `tools/build.py`'s check of INITMS never ran (it looked for a segment the banner does not have); it runs now, along with a check of
  the stock ` 64K RAM SYSTEM  ` text at `$E49A`, which the stock line now prints from the ROM. **Rebuild `kernal-tr.bin`.**

## v0.9.0.48

### Added
- **A VDrive KERNAL in the machine itself**: an EPROM in the KERNAL socket, or a MEGA65 C64 core with `kernal-tr.bin` as its KERNAL,
  works with the regular firmware; no separate build. The firmware sees that the C64's own KERNAL is already this build's, serves
  nothing and answers the drives; a resident VDrive KERNAL of another revision gets its drives too, but no bus probe in the menu.
  `docs/DESIGN.md` section 45; py65, host and VICE tests; not on a MEGA65 or EPROM yet.

### Changed
- The KERNAL touches the mailbox only while a VDrive Special IO answers (`$DE19` reads `$D6`). Without one (no TR, another Special
  IO, the TR menu's own handler) serial calls go to the real bus at once, instead of waiting a ~1 s time-out per call, and no longer
  write the IO1 registers at `$DE11`/`$DE15`, which under the TR menu are stored settings. **Rebuild `kernal-tr.bin`** for `/VDrive/kernal.bin`.
- With an unknown KERNAL (or a failed DMA read) the menu no longer runs its bus probe before the launch; it had nothing to give then.

## v0.9.0.47

### Added
- Card files and folders with names over 16 characters (or with characters the C64 cannot type) are listed under an alias with an
  up arrow, which `CD:`, `LOAD` and paths accept: `The-Pumpkins-Breadbin-of-Horror-side1.D64` shows as `THE-PU↑SIDE1.D64`. They used
  to be hidden. Two names with the same alias are both left out (rename one). Firmware only (`docs/DESIGN.md` section 44); host and
  glue tests; works on a real C64.

### Changed
- `trvdrive` get and the `/.vdrive` sidecar keep the case of an image path as it was given (`/GAME.D64`, was `/game.d64`).

## v0.9.0.46

### Added
- **Drive activity LED**: the TR+'s LED blinks (75 ms phases) while a VDrive unit services requests and gets its previous state back
  180 ms after the last one; commands handed back to a real drive on the bus do not count. Contributed by ziggystar12, the creator of
  Mean Hamster Engine, from his own VDrive build. Host tests (`tests/fw_host`); builds; works on a real C64 (`docs/DESIGN.md` section 43).

Firmware only: `kernal-tr.bin` is unchanged.

## v0.9.0.45

### Changed
- **SwiftLink sends a byte at once when nothing is in flight** (Nagle's rule ahead of the 2 ms / 20 ms timers), so programs that wait for
  an echo or reply (CCGMS text send, XMODEM ACKs) no longer pay 2 ms per exchange. The Tx buffer now takes its 256 bytes of RAM2 only
  while SwiftLink runs and is emptied when the connection changes. Matches the revised upstream PR #80; builds; works on a real C64.

### Upstream
- PR #80 merged into TeensyROM (`6518b966`); the patch now applies on that commit without its own copy of the SwiftLink fix. Same firmware.

## v0.9.0.44

### Changed
- **SHIFT+RUN/STOP loads from disk**: it types `LOAD"*",8,1` and `RUN` (stock: `LOAD` and `RUN`, from the tape, whose code the VDrive
  KERNAL reclaimed), so it starts the first file on the disk in device 8, loaded to the address in its file. The line shows on the screen
  as if typed. VICE on the five KERNAL ROMs the firmware patches; works on a real C64 (`docs/DESIGN.md` section 42).

Firmware 0.9.0.44 (the KERNAL patch it lays over the C64's ROM) **or** a rebuilt `kernal-tr.bin` for `/VDrive/kernal.bin`.


## v0.9.0.43

### Added
- **`VDCOPY` copies whole disks** (`D` in its new start menu): a `.d64` image onto a real floppy, a floppy into a new `.d64`, or between
  any two units, all 683 sectors through the drives' own block commands (`U1`, `B-P`, `U2`). Mounts a source image by path, makes the
  destination image with `N:`, or formats a real destination first with the source's disk name and ID. A sector map shows the progress
  and read errors (counted; the sector is written as read). For running disks whose loaders need their own drive code from a real 1541.
  Host tests (py65 against the drive code) and VICE with a true-drive 1541; not on hardware (`docs/DESIGN.md` section 40).

### Changed
- `VDCOPY` takes the TeensyROM menu's colours (as VDrive Config does), starts with a menu (`F` files, `D` disk, `Q` quit; RETURN at the
  file selection goes back to it) and keeps its tables at `$2800-$3FFF`.
- **No KERNAL file to build or select any more.** The VDrive Special IOs no longer use the `<Shift-K>` KERNAL setting (it stays the plain
  "Kernal Replace" Special IO's). At each launch the firmware reads the C64's own KERNAL ROM by DMA and patches it in memory, when it is one
  the patch has been tested on: 901227-03, 901227-02, 251104-04 (SX-64), 325179-01 and 325182-01 (a C128's C64 mode). A
  `/VDrive/kernal.bin` on the SD card (a `kernal-tr.bin` built on a PC, for any other ROM) is used instead whenever it exists. With an unknown
  ROM and no such file the drives stay off (the serial console says why). `docs/DESIGN.md` section 40.
- **`SYS64738` returns to the TR menu** while a VDrive Special IO runs (so does any program that jumps to the RESET vector), writing back what
  is not saved yet. Two new read-only mailbox registers, `$DE19/$DE1A` (`$D6 $64`), let the KERNAL tell this from the reset of a launch.
  Tape transfers with PLAY pressed now restart the machine instead of crashing (their code space holds the new routine).

Host tests (py65, firmware glue against the 901227-03 ROM), VICE (`basic`, and the five ROMs with `make test-variants`); works on a real C64 (`SYS64738` on a C128 not checked).
Firmware 0.9.0.43 **and a new KERNAL**: built in by the firmware, or rebuild `kernal-tr.bin` for `/VDrive/kernal.bin`.


## v0.9.0.42

### Fixed
- Directory listings (and other reads from the virtual drives) came through with lost or wrong bytes on a C64 since 0.9.0.37. Upstream's
  C128 HIRAM sense (TeensyROM PR 78) put a test in front of every IO1/IO2 answer that made the bus handler save registers on each cycle,
  and the answers came late. Rebased on upstream `136baa83`, which picks the C64 or C128 check once per launch. Firmware only; the KERNAL
  image is unchanged.

## v0.9.0.41

### Changed
- The Ethernet loop poll (Kernal Replace with Ethernet initialized: `fnet_poll()` from `loop()` instead of the 1 ms PIT interrupt) is now
  carried as upstream [TeensyROM PR 85](https://github.com/SensoriumEmbedded/TeensyROM/pull/85), byte for byte (`firmware/kernal-fnet-poll.patch`,
  applied first by `tools/make_tr_patch.sh`); `kernalreplace-fixes.patch` keeps only `KernalFast` and the `LATE_ISR_TRACE` debug trace. The VDrive
  Special IOs join the poll list, and the AT-command PIT unmask also builds under `Fab04_VDrive`. Plain SwiftLink on the stock KERNAL no
  longer switches to the loop poll, as in the PR (it was never measured). Firmware builds for TR+ and TR; not on hardware.

## v0.9.0.40

### Changed
- `VDrive Config` takes the TR menu's colour theme and look: a reverse banner, keys in reverse in the option colour with their meaning in
  the sources colour, paths in the name colour, messages in the time colour. The colours come as a third `!STATE` line (the seven colour
  references as hex digits, which the Teensy keeps after handing over to the VDrive handler); without it, the menu's defaults. Host tests
  (py65, firmware glue); not on hardware.

## v0.9.0.39

### Added
- **Deleting image files on the card:** with nothing mounted, `S:NAME.D64` (also `.D71` / `.D81`, a comma list, a folder path or a CMD
  path) deletes image files. Exact names only (`33` for a pattern), images only (`64`), no such name `34`; a file with the FAT read-only
  attribute stays (`26`). Answers `01,FILES SCRATCHED,nn` as on a disk (`docs/DESIGN.md` section 39). `L:` for card images is not
  possible (SdFat 2.1.2 cannot set the attribute). VDSUITE gets section 8 (clean-up) and 186 checks. Unit, glue and VICE tests; not on hardware.

## v0.9.0.38

### Changed
- The VDrive menu remap in `IOHandlers.ino` is now a macro defined before `IOHandlerSelectInit()` instead of a case inserted above
  upstream's menu case, so the patch also applies on top of TeensyROM PR 81 (which rewrites that line and adds a GEOS case first);
  the VDrive+SwiftLink remap leaves GEOS's handlers alone. No change in behaviour. Not on hardware.

## v0.9.0.37

### Changed
- Rebased on upstream TeensyROM `fa509421`, which includes [PR 78](https://github.com/SensoriumEmbedded/TeensyROM/pull/78): Kernal Replace
  senses HIRAM by DMA on a C128, and its early return when HIRAM is known to be RAM replaces the hunk `kernalreplace-fixes.patch` carried.
  After the menu's bus probe the DMA sense state is reset when `KernalCheck` is hooked back in. The patch applies on upstream main and on
  open PRs 79, 82 and 83. VDrive has not been run on a C128. This build and the next four corrupted directory listings on a C64
  (late bus answers; fixed in 0.9.0.42).

## v0.9.0.36

### Changed
- The power-on banner shows the virtual drives' device numbers on the second line, as wide as the stock one:
  `64K RAM  38911 FREE  VDRIVE SD 8 USB 9` (`SD10 USB11`, `NOSD` / `NOUSB` for a missing card or stick; the stock line without the VDrive firmware). The top line is the stock `COMMODORE 64 BASIC V2` again (the
  `TRVDRIVE` bring-up text moved to `bringup/banner.s`, still built as `kernal-banner.bin`). The firmware writes each unit's number (fixed, or what the menu's
  bus probe gave), or "absent", into the KERNAL copy it serves before the C64 starts; a KERNAL launched without the menu probe probes
  the real bus itself at power-on for automatic units, as the first disk access will (`docs/DESIGN.md` section 38). Firmware 0.9.0.36 and **a rebuilt `kernal-tr.bin`** (either
  alone works, without the numbers). Host tests (py65 with a fake bus, firmware glue) and VICE with true-drive 1541s; not on hardware.

### Fixed
- A command or `OPEN` that succeeds now resets an error nobody had read, as a 1541 answers `00, OK` after any successful command: a stale
  `62` from a `LOAD` of a missing file came back after a `CD:/` that worked. Found by VDSUITE.

### Added
- **VDSUITE** (`tests/basic/vdsuite.bas`, on `vdtest.d64`): the hardware checklist as one BASIC menu program. It makes a scratch folder with a
  D64, a D71 and a D81, runs the SEQ, DOS, block and REL checks on each, bad sectors from an error table, plain files and `MD:` / `RD:`, and
  the CMD path syntax, and lists the checks that need a person. `make testdisk` also writes `vdbad.d64` and `vdplain.prg` for the card root.

Also checked on hardware (owner, 2026-10-30): the first-access bus scan adds no noticeable delay; single and double taps of the Alt button switch disks.

## v0.9.0.35

Works on hardware (owner, 2026-10-30).

### Added
- **Device numbers settled before the program starts.** The TR menu now probes the serial bus itself before every launch with a
  VDrive Special IO (any PRG start: menu, remote launch, hot keys, auto-launch, "Exit to BASIC"), with the C64's own KERNAL, so
  `PEEK(56854)` / `PEEK(56855)` (`$DE16/$DE17`) give the SD / USB numbers from the program's first instruction. Programs no longer need
  a bus access of their own first. The firmware holds the replacement KERNAL back during the probe; the KERNAL's own scan on the first
  disk access stays as the fallback (`docs/DESIGN.md` section 37). Both numbers fixed with `U0>`, or no card and no stick: no probe.
  The patch now changes the menu too (`MainMenu.asm`, routine in `firmware/vdrive_probe.asm`, `TeensyROMC64.h` rebuilt by
  `tools/make_tr_patch.sh` with the TR's own tool, so regenerating the patch needs ACME 0.97 and node).
- **Back to the TR menu from a program:** mailbox command 16 (`POKE 56848,16`) writes back what is unsaved and resets into the TR
  menu, as the menu button does. VDrive Config's `Q` uses it ("Exit to Menu"); without an answer it returns to BASIC as before.
- `VDrive Config` is in the TR menu's Utilities (Teensy memory), run with the user's default VDrive Special IO (its REU / SwiftLink
  kept).

### Changed
- `VDrive Config.prg` is now machine code (`util/vdconfig.s`, assembled by `make util`; no petcat needed) instead of BASIC: the same
  screen and keys, but reading the list, the directory listings of the browser and every redraw no longer crawl through BASIC string
  handling. The screen is in the lower-case character set: paths show as they are on the card, messages in mixed case. The list
  header and the start-up message say which Alt-button gesture switches that list (1 tap SD, 2 taps USB). It reads the device numbers
  the menu's probe left (before this firmware it made one bus access first).

Tests: unit and firmware glue host tests; `tests/test_vdconfig.py` runs the real PRG in py65; the menu's probe routine runs in VICE
with true-drive 1541s (`make test-menuprobe`, needs ACME). Not on a C64 yet: the probe through the real menu and Teensy, VDrive Config
on screen, the return to the menu.

## v0.9.0.34

### Fixed
- SwiftLink transmit could hang the whole firmware: NativeEthernet's `client.write()` waits for send-buffer space, and with the
  Ethernet stack polled from `loop()` (0.9.0.14) a full buffer stopped `loop()` for good (an 8 KB HTTP POST stalled after a lost
  segment). Bytes now go into a 256-byte buffer, sent after a 2 ms pause, every 20 ms or when full, never more than
  `availableForWrite()`; while it is full the ACIA's transmitter stays "not empty" (`firmware/swiftlink-tx-fix.patch`, meant
  for upstream). Works on hardware (owner): the stalled uploads complete.

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
