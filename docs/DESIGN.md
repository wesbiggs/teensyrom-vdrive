# TR+ Virtual-Drive KERNAL — Design Concept

Goal: a replacement C64 KERNAL, loaded by the TeensyROM+ (live KERNAL replacement, firmware ≥ 0.8), in which
serial-bus traffic for configurable devices for SD and USB (defaults 8 and 9) never touches the IEC port. Instead it is
served by firmware on the Teensy from `.d64` images (and plain `.prg`/`.p00` files). Other device
numbers still fall through to the real bit-banged IEC code, so real drives keep working.

## 1. Where to intercept

| Level | Hooks | Pros | Cons |
|---|---|---|---|
| High (LOAD/SAVE/OPEN/CLOSE/CHKIN/CHKOUT/BASIN/BSOUT, via RAM vectors `$0314-$0333`) | many | fast bulk LOAD/SAVE | programs that bypass vectors or use `$FFxx` jump-table directly with their own channel logic see a different world; must re-implement ST/file-table handling |
| **Low (serial layer)** — `LISTEN $ED0C`, `TALK $ED09`, `SECOND $EDB9`, `TKSA $EDC7`, `CIOUT $EDDD`, `ACPTR $EE13`, `UNLSN $EDFE`, `UNTLK $EDEF` | 8 | everything above (OPEN, LOAD, SAVE, `$` directory, command channel 15, ST) works unmodified and is protocol-compatible | byte-at-a-time over the mailbox is slower than bulk, so add a fast path (below) |

**Recommendation: low-level intercept + a high-level fast path for LOAD/SAVE** (hook `ILOAD`/`ISAVE` target
routines `$F4A5`/`$F5ED` only when device is 8/9 and secondary address / name allow it).

Each stub: `if DEVICE in {8,9}` → talk to the virtual drive; otherwise `JMP` into the original routine.

## 2. Architecture

```
C64 6502                                   Teensy 4.1 (TR+ firmware)
┌───────────────────────┐   IO1/IO2 regs   ┌──────────────────────────────┐
│ patched KERNAL (ROM)  │ ───────────────► │ mailbox handler (cycle-safe) │
│  LISTEN/TALK/ACPTR…   │ ◄─────────────── │  → virtual IEC device 8 / 9  │
│  fast LOAD/SAVE       │                  │     • D64 reader/writer      │
└───────────────────────┘                  │     • CBM-DOS cmd channel    │
                                           │     • dir→BASIC listing      │
                                           │  SD (dev 8)   USB (dev 9)    │
                                           └──────────────────────────────┘
```

### Mailbox registers (proposal; confirm free space in TR+ IO1/IO2 map)
- `$DE00` CMD (write): `LISTEN, TALK, SECOND, TKSA, CIOUT, ACPTR, UNLSN, UNTLK, LOAD, SAVE`
- `$DE01` ARG / data-out; `$DE02` data-in; `$DE03` STATUS (busy, EOI, ST bits, device-not-present)
- Handshake: 6502 writes CMD, spins on STATUS.busy clear (Teensy at 600 MHz finishes within µs; the
  bus must never be stalled — handler runs from a polling loop outside the bus ISR, or the 6502 simply waits).
- Bulk: `LOAD` command passes name + SA; Teensy streams the file into C64 RAM using TR+ memory-write
  capability (or 6502 copy loop from a `$DF00` window). Result: load address end in `$AE/$AF`.

### Firmware side (fork of SensoriumEmbedded/TeensyROM)
- `vdrive.cpp`: per-unit state (mounted image path, 16 channels with buffers, error state).
- `d64.cpp`: 35/40-track geometry (zone sector counts 21/19/18/17), BAM, directory walk, file chain
  follow, allocate/free for SAVE, scratch, validate; read-only first.
- CBM-DOS subset: `$`, `$:pattern`, `N:`, `S:`, `I`, `V`, `R:`, `CD:` / `CD:..` to navigate SD folders and
  mount images (SD2IEC-style), status string `00, OK,00,00`.
- Directory generated as a tokenised BASIC program (load address `$0401`) so `LOAD"$",8` + `LIST` works.
- Plain `.prg` in the folder are also loadable by name when no image is mounted.
- Mounting UI: via `CD:` commands and/or TR menu; mounted image persisted per unit.

## 3. KERNAL side
- Base ROM: user's own dump of 901227-03 (do **not** commit Commodore's ROM). Build produces the
  replacement as `stock.bin + patches`, using a ca65 patch file and a small script that overlays
  patch segments at fixed addresses and recomputes nothing (KERNAL has no checksum).
- Space: new code goes in free/reclaimed areas (the original bit-bang routines remain for other devices,
  so reclaim little). Needs a few hundred bytes; candidates: tape-only routines (`$F6xx-$F8xx`) if tape is
  sacrificed, or place code in the TR+-supplied extra KERNAL space if the replacement mechanism permits
  >8K / banked images (to confirm).
- Toolchain here: `ca65`/`ld65` (cl65) and VICE `x64sc` are installed — good enough to assemble and test.

## 4. Test strategy
1. Prototype the virtual drive entirely in VICE first: write the 6502 stubs against a *fake* mailbox
   implemented as VICE's virtual device traps (or just use a small host-side C++ mock + monitor script) to
   validate KERNAL patching/ST semantics. Compare against VICE true-drive behaviour for `LOAD"$",8`,
   `OPEN15,8,15,"I"`, sequential file read/write.
2. Port the D64 module as a host library (unit-tested with real `.d64` fixtures), then drop into firmware.
3. Hardware bring-up on TR+: mailbox ping-pong, then directory, then LOAD, then SAVE.

## 5. Milestones
- M0 Confirm TR+ KERNAL-replacement mechanics (image size, EEPROM save, IO1/IO2 availability, firmware hook
  point) — read the TeensyROM repo docs/source.
- M1 Patch build pipeline + "hello" KERNAL (changed boot banner) loading on TR+.
- M2 Mailbox + dev 8 `LOAD"$",8` returning a canned directory.
- M3 Real read-only D64 (directory, LOAD, sequential read, command channel).
- M4 Write support (SAVE, @save, scratch, N).
- M5 Fast bulk LOAD/SAVE, dev 9 (USB), `CD:`/mount.
- M6 Compatibility pass (loaders that use `$FFxx` vectors, fastloaders will still fail by design — they
  bit-bang the drive).

## 6. Risks / open questions
- Known by design: custom fastloaders/copy protection bypass the KERNAL and won't work.
- Whether the TR+ replacement KERNAL image can be modified by the user without firmware changes, and whether
  the mailbox needs firmware changes (almost certainly yes) → project ships a firmware fork + KERNAL image.
- IO1/IO2 address collisions with other TR+ features (REU, MIDI, etc.).
- Timing: 6502 polling vs Teensy servicing latency; PAL/NTSC irrelevant at mailbox level.
- Legal: distribute patches only, not Commodore ROM bytes.

---

## 7. M0 findings (TeensyROM repo, cloned 2026-10-15 HEAD)

Source: `Source/Teensy/MinimalBoot/Common/IO_Handlers/IOH_KernalReplace.c`, `ISRs.c`, `Menu_Regs.h`, `Common_Defs.h`,
`docs/TR+NewFeatures.md`, `docs/General_Usage.md`.

**How KERNAL replacement works**
- Image is a raw **8192-byte** file (size is checked) picked with `<Shift-K>` in the file browser; the path is stored in EEPROM and read into a malloc'd buffer at PRG launch / "Exit to BASIC".
- Served live by a bus snoop (`KernalCheck`, runs from the Phi2 ISR). On a CPU read of `$E000-$FFFF` it drives the data byte with GAME asserted, and decides ROM-vs-RAM (HIRAM) by pulsing EXROM and sampling ROMH ("Skoe method"). Writes to `$0000/$0001` invalidate the HIRAM state. So the C64's own banking (`$01`) still works and RAM-under-KERNAL is untouched.
- It is its own "Special IO" handler (`IOHndlr_KernalReplace`) with **IO1/IO2/Polling handlers all NULL**. In the default build, KernalReplace therefore *excludes* any other Special IO (REU, TR_BASIC, etc.) because only one `IOHandler[]` is active. The compile flag `Fab04_GlobalKernalReplace` (commented out in `Common_Defs.h`) moves it to a separate hook `fKernRepl` that runs alongside whichever handler is active — **this is what we need**, or a new handler that contains both.

**IO1/IO2 map**
- IO1 (`$DE00-$DEFF`) offsets are defined by `IO1_Registers` for the menu handler (`IOH_TeensyROM`): 1-52 and 64-103 used, `0xFE` swap poll. Each IO handler gets its **own** IO1/IO2 handler functions, so a new handler may define `$DE00/$DF00` freely — no collision as long as it is the active handler (menu regs are not live once a PRG is running).
- IO handlers are called from the ISR → must be tiny; file/SD work must be in `PollingHndlr` (main loop). Therefore the mailbox must be *asynchronous*: 6502 writes command, Teensy ISR latches it, polling handler does the work, 6502 spins on a status register (same pattern as `rwRegStatus` busy and TR_BASIC's `TR_BASContReg/TR_BASStatReg`).
- **Prior art to copy:** `IOH_TR_BASIC.c` (custom BASIC `TLOAD/TSAVE/TDIR` — command reg, status reg `0x55` ready, filename reg, stream data reg, stream-available reg, BASIC error codes). Our mailbox can reuse that exact register shape. Also `rRegStreamData` ("auto-increments when read") gives a fast byte stream with no address setup.

**Capabilities that simplify the design**
- TR+ is a real bus master (`ReadC64Mem/WriteC64Mem`, DMA). LOAD can therefore be done as a **DMA bulk write into C64 RAM** from the Teensy with no 6502 copy loop.
- No existing D64/`.Dxx` handling in firmware (only a hand-off to an external **Meatloaf** device via `Shift-M`). The D64 module is new work. Meatloaf/SD2IEC-style behaviour is a good reference for the CBM-DOS surface.

**Constraints**
- Image must be exactly 8192 bytes → full 8K KERNAL (no more). New code must fit inside the stock layout (e.g. reclaim tape routines).
- Build RAM1 padding in the `tr-plus` firmware is tight (~15 KB measured, per `Known-Issues.md`); D64 buffers should be malloc'd on demand (DMAMEM/PSRAM) and handler code marked `FLASHMEM` where not ISR-reachable.
- Replacement takes effect only on "Exit to BASIC" (F2) or PRG launch — not at cold power-on.
- A mailbox only works while *our* handler is the active IOH; so deliver as a new Special IO "VDrive" that also includes the `KernalCheck` hook (or enable `Fab04_GlobalKernalReplace` and combine).

**Design adjustments**
1. Delivery = firmware fork/PR adding `IOH_VDrive.c` (IO1 mailbox + Polling worker + D64 engine) + an 8K KERNAL image. Not purely a ROM.
2. Extension alternative: `docs/TR_Host_Extensions.md` describes `.TRH` host extensions installable without touching core firmware — worth reading in M0b to see whether it can register an IO handler / KERNAL image (would avoid forking).
3. Mailbox register shape: copy TR_BASIC (cmd, status, filename, stream data auto-increment).

**Next:** M0b read `TR_Host_Extensions.md`/`Extension-Hosts.md`; then M1 (patch pipeline + banner-change KERNAL to prove loading).

## 8. M0b findings — can a `.TRH` host extension do this without forking?

Sources: `docs/TR_Host_Extensions.md`, `docs/Architecture/Extension-Hosts.md`, `Source/Teensy/ExampleHost/ExampleHost.ino`.

**Verdict: technically yes (it owns the whole machine), but it is a poor fit. Recommend a firmware fork/PR for the real thing.**

- A host is **not a plug-in API**. It is a complete third firmware image in a 384 KiB flash slot. The build overlays your sketch onto `MinimalBoot`, so you inherit the PHI2 ISR (`ISRs.c`), `fBusSnoop`, DMA and SD, but you **replace the top-level program**. There is no "register an IO handler / KERNAL hook" call; you would write your own `.ino` and supply `CurrentIOHandler` etc. yourself (`ExampleHost.ino` shows the minimum set of globals).
- The Kernal Replace code (`IOH_KernalReplace.c`) and the full handler list are `#ifndef MinimumBuild`; the host image is built from the minimal tree, so `KernalCheck` would have to be copied/ported into the host, and the IO1 mailbox written from scratch. The ISR hooks (`fBusSnoop`, IO1/IO2 dispatch through `IOHandler[]`) are available.
- **Hard limits for a host image:** `USB_DISABLED` (so **no USB drive → device 9 impossible**, only SD = 8), no PSRAM, 48 KiB shared stack, heap capped at 16 KiB, 64–96 KiB ITCM for code. SdFat/SD work.
- **One extension slot only** — installing ours evicts MPE. Entered by selecting a file type registered via `/VMS/<name>/manifest.vmi` on the SD card; exit is the TR menu button or C64 reset. It would not integrate with the normal "Exit to BASIC"/PRG-launch Kernal flow or the `Shift-K` selection, and the user would have to boot the KERNAL from inside the host.
- The `.TRH` route's benefit is only "install without reflashing core firmware", which the dev-loop cost doesn't justify here.

**Decision:** Build as a **fork of the core TR+ firmware** (full build, USB host available): new Special IO `IOH_VDrive` + `Fab04_GlobalKernalReplace`-style hook (`fKernRepl`) so the replacement KERNAL and the mailbox run simultaneously, aiming to upstream it. Keeps SD=8, USB=9 and the normal Shift-K / Exit-to-BASIC flow.

## 9. M2 status (done): serial-layer hooks + VICE mock

- `patches/vdrive.s` hooks all 8 serial entry points (`JMP` over the first 3 bytes of `$ED09 $ED0C $EDB9 $EDC7 $EDDD $EDEF $EDFE $EE13`). Each stub writes ARG/CMD to the mailbox and waits for `BUSY` to clear; `NOT_MINE` makes it replay the displaced instructions and continue in the stock routine, so devices other than 8/9 still use the real IEC code (tested: device 11 → `?DEVICE NOT PRESENT` from stock code). X/Y are preserved.
- Mailbox registers as built: `+0 CMD` (W, trigger), `+1 ARG` (RW), `+2 DATA` (R), `+3 STAT` (bit7 BUSY, bit6 NOT_MINE), `+4 ST` (R, OR'd into `$90`). Firmware requirement: **ARG must read back** what was written; the Teensy decides ownership (device 8/9 on LISTEN/TALK, then stays "mine" until UNLSN/UNTLK).
- Space: stubs are 165 bytes, placed in reclaimed tape bit-timing code `$F8E2-$FB8D` (no references from outside; **cassette I/O, device 1, stops working** — to be re-verified on hardware).
- `make test` builds a RAM-mailbox image (`MBX=$C800`) and `tools/vdrive_mock.py` plays the Teensy through VICE's binary monitor (store-checkpoint on CMD). Passing: `LOAD"$",8` + `LIST`, `LOAD"TEST",8`, file-not-found, command channel status read/clear, device 9 absent, non-virtual device falls through. 177 mailbox calls in the run.
- Not yet covered: SAVE, sequential write/read via OPEN/PRINT#/INPUT#, STOP-key during LOAD, IRQ/`SEI` interplay on hardware, ISR timing of the real IO1 handler.

## 10. M3 status (done): read-only D64 engine

- `d64/d64.{h,c}`: portable C99, no malloc, sector-read callback (so the same code can sit on SdFat/USB on the Teensy). Provides geometry (35/40 tracks), header + BAM free count (1541 convention, track 18 excluded), directory walk across sectors (skips scratched), CBM wildcards (`?`, `*`), file chain streaming (`d64_file_read`, EOF detection), and a BASIC-listing generator (`d64_build_dir`, load addr `$0401`, optional pattern). Chain/directory loops and out-of-range links return errors instead of hanging.
- Tests (`make test-d64`, 13 cases): fixtures are built on the fly with VICE's `c1541` (35 files incl. 1/10/254/255/1000/60000/100000-byte files, a SEQ file, a scratched entry, a second directory sector), file contents compared byte-for-byte at several read chunk sizes, block counts and blocks-free compared with `c1541 -list`, listing link-chain validated, corruption cases (self-linked sector, directory loop, bad link track), sector geometry.
- Mock (`make test`) now serves a real `.d64` through the library: `$`, `$:T*`, file load, 6000-byte multi-sector `LOAD"BIG",8,1` verified byte-for-byte in C64 RAM, error channel, device 9 absent.
- Caveats: PETSCII names are passed through unconverted (names compare as PETSCII; callers must convert if matching ASCII input). The directory listing's spacing is modelled on a real 1541 but not yet byte-diffed against a physical drive. Write support (BAM allocation, SAVE, scratch) is M4.
- Observation for M5: that 6 KB load took ~6,300 mailbox calls (one per `ACPTR` byte). Fine functionally, but it motivates the planned bulk LOAD fast path.

## 11. M4 status (done): write support + C channel layer

- `d64/d64.c` write layer: BAM allocation (data from track 17 down, then 19 up; interleave 10 for data, 3 for directory; track 18 reserved for the directory), `d64_create / d64_write / d64_close_write` (file shows as an unclosed "splat" until closed, like a real drive), `d64_scratch(_entry)`, `d64_rename`, `d64_format` (full with ID, quick without), directory extension up to the 144-entry limit, and rollback on disk-full (every allocated sector returned, entry removed).
- `d64/vdrive.{h,c}` — the firmware core. Implements the mailbox protocol (`vdrive_mailbox(cmd, arg) -> data, st, not_mine`) for units 8/9: 16 channels, OPEN parsing (`@0:` replace, `0:` prefixes, `,P/S/U` types, `,R/W`), `$` and `$:pattern` directories, sequential read/write, SAVE (channel 1) / LOAD (channel 0), the error channel (status text, cleared on read), and commands `S` (scratch, wildcards, comma lists), `N` (new/quick), `R` (rename), `I/V/U` (accepted, no-ops), anything else → `31`. CLOSE 15 closes all channels. Device 9 absent → `ST=$80`; any other device → not mine.
- **Tests (all passing, `make test`):** `test_d64.py` (13, read), `test_d64_write.py` (15: round trips at 0/1/2/253/254/255/508/509/1000/30000 bytes read back by `c1541`, BAM equality with `c1541 -validate` after every mutation, disk-full rollback, filling the disk to exactly 0 free, directory growth and the 144 limit, format BAM byte-identical to a c1541-formatted disk, read-only disk), `test_vdrive.py` (18: the exact bus sequences KERNAL OPEN/SAVE/LOAD/INPUT# generate), plus the VICE run: LOAD/LIST/`$:pattern`, SAVE of a BASIC program, collision (`63`) and `@0:` replace, sequential `OPEN…,S,W` / `INPUT#` round trip, a 6000-byte multi-sector SAVE from RAM that `c1541` reads back identical, scratch, and BAM equals `c1541 validate` at the end (12,657 mailbox calls).
- Not covered yet: append mode, REL files, `V`alidate/`C`opy commands, write-protect/locked-file toggles from the C64, GEOS, 40-track images beyond reading, STOP-key and IRQ interplay on real hardware, SD/USB file backends for the sector callbacks, and the Teensy IO1 handler itself.
- Performance note carried into M5: still one mailbox transaction per byte; BAM is read-modify-written per allocated sector (fine for the mock; on SD, cache the BAM sector and flush at close).

## 12. M5 status (done on the host, not on hardware): firmware glue + fast LOAD/SAVE

- **Mailbox engine** (`d64/vdrive_io.{h,c}`): IO1 `CMD ARG DATA STAT ST LEN` + a 256-byte IO2 window (128 bytes at `$DF80` since 0.9.0.28, section 32). The ISR-side accessors are O(1); `vio_poll()` does everything else. New commands: `FLOAD_OPEN/BLK`, `FSAVE_OPEN/BLK/CLOSE` built from the byte-level protocol (so there is one source of truth).
- **6502 fast path** (`patches/vdrive.s`): hooks the serial-open `JSR` inside KERNAL `LOAD` (`$F4C8`) and `SAVE` (`$F605`); names go through the window, data moves in ≤255-byte blocks. A 6000-byte LOAD takes 27 transactions instead of ~6000 (whole VICE suite: 12,657 → 244 mailbox calls). VERIFY and non-8/9 devices use the stock code. `vcall` now has a ~1 s time-out so a KERNAL image without an active handler falls back to real IEC instead of hanging (tested in VICE).
- **Mounting** (`vdrive.c` host ops): `CD:image.d64` mounts, `CD:_`/`CD:..` unmounts, `CD:dir` navigates, and with nothing mounted `$` lists the host directory (dirs `DIR`, images `D64`).
- **TeensyROM integration** (`firmware/`): `IOH_VDrive.c` (Special IO with the KERNAL hook, IO1/IO2/polling handlers, SD/USB sector backend with idle write-back) and `teensyrom-vdrive.patch` (`git apply --check` passes on the current checkout; regenerate with `tools/make_tr_patch.sh`). The library is also checked as C++ since the firmware compiles `.c` files inside the `.ino`.
- **Tests:** `make test` = 13 + 15 + 18 + 18 Python/ctypes unit tests, `tests/fw_host` (the glue compiled against host stubs and driven through the ISR entry points with real files, then verified with `c1541`), and the VICE run.
- **Honest limits:** no build against the real Teensy toolchain, nothing on hardware, RAM1 budget unmeasured (see `firmware/README.md`).

## 13. RAM footprint and bring-up plan (done)
- Footprint cut from ~16 KB to **4,584 B** for the whole drive state (`sizeof(vio_t)`, asserted < 5,000 in `tests/test_vio.py`): the 6 KB directory buffer is gone (listings are generated a line at a time, byte-identical to the buffered builder — tested on a 60-entry directory with and without patterns), channel state is a pool of `VD_NSLOT` (default 6) slots shared by LOAD/SAVE/OPEN instead of 16 fixed channels per unit, the error channel lives outside the pool, and `NO CHANNEL` (70) is reported on exhaustion. LOAD (ch 0) and SAVE (ch 1) still work with the other slots full.
- Placement: everything except the four IO register accessors is `VD_COLD` (= `FLASHMEM` in firmware): 132 bytes of hot code measured (arm64 `-Os`, indicative only). State is `DMAMEM` by default (`-DVD_ALLOC_HEAP` for malloc). **Not measured against the real linker.**
- New `docs/BRINGUP.md`: staged plan (baseline firmware → patched build/fit → handler registered → mailbox registers via BASIC `POKE`/`PEEK` → scope the IO1/IO2 timing → stock-KERNAL control → banner proof → real image with `-DVD_TRACE` serial trace → soak), each with pass/stop criteria, a triage table, rollback and a results table. New make targets `build/kernal-stock.bin` and `build/kernal-banner.bin` for the control and proof images.

## 14. Configurable device numbers, stored in EEPROM (done; reworked in 16)
Each unit has a *pinned* number (two EEPROM bytes, `eepAdVDriveDevs`, carved from the reserved area; 0/invalid = automatic) and a current number (`vdrive_t.dev[]`, 0 = not claimed). A pin is set at run time with the real drives' command `OPEN15,8,15,"U0>"+CHR$(n)` (8-30, not the other unit's number, 0 = automatic again), which persists through a `save_devices` host callback — so nobody recompiles or reflashes. An *automatic* unit starts on `VD_DEV0`/`VD_DEV1` (8/9, or the next number not pinned by the other unit) and is moved by the bus scan of section 16. The KERNAL patch contains no device numbers (every hook offers the transaction to the Teensy, which answers "not mine" for real drives), so one image serves every numbering. `U0>` is parsed before the CR/LF strip because devices 10 and 13 are LF and CR. Tests: virtual drives on 10/11 with 8/9 left to the real bus (fast and byte paths), renumber/restore/reject cases, pins versus the other unit's default, a compile-time override check, and the firmware glue against an EEPROM stub (erased flash, persistence across a simulated restart, garbage and duplicate values).

## 15. Coexisting with SwiftLink (done, host-tested only)
The bus ISR calls one `IOHandler[CurrentIOHandler]` only, and the SwiftLink ACIA (`$DE00-$DE03`, Turbo232 `$DE07`) overlapped the mailbox (`$DE00-$DE05`), so the two Special IOs excluded each other, and launching a SwiftLink program from the menu (e.g. CCGMS) replaced the drive. Now the mailbox sits at `$DE10-$DE15` (`MBX = $DE10` in `patches/vdrive.s`, `VD_MboxBase` in the glue), and a third handler, `IOH_VDriveSwift` ("VDrive+SwiftLink"), dispatches IO1 by address (below `$10` to SwiftLink, else the mailbox), IO2 to the drive, the cycle callback to SwiftLink, and runs both init/polling routines. `VDriveSwift_Remap()` substitutes it for plain `IOH_Swiftlink` when the default Special IO is a VDrive one (applied to the Teensy-menu association in `KERNALPreStart()` and `IOHandlerSelectInit()`; the cart-type table is untouched since carts do not use our KERNAL). Open risks: ISR time with the extra branch, SD writes masking the SwiftLink NMI at high baud rates, and the shared SD/USB file systems (SwiftLink's browser downloads). One KERNAL image serves both handlers (the mailbox moved for plain VDrive too). Tests: `tests/fw_host` covers address routing, unmapped ACIA offsets, init/polling and the remap rules.

## 16. Automatic device numbers: the bus scan (done in VICE and on the host, not on hardware)
**Problem.** A unit that claims 8 or 9 hides a physical drive (or SD2IEC, Meatloaf, ...) with the same number: the KERNAL never puts the call on the serial bus. **Rule.** Use the first free numbers from 8 up: SD first, USB second, only for units that are present, unless the user pinned a number with `U0>`.
**Who probes.** Only the 6502 can see the bus. **When.** Not at reset: the Teensy's own IO1 handler owns `$DE00-$DE3F` until the program has been loaded and the VDrive handler takes over (`rsIOHWSelInit`), so a mailbox call during BASIC init would hit the TR's menu registers. The scan is therefore lazy: `VDrive_PreStart()` calls `vio_arm_scan()` (state 1) when some present unit is automatic, and the first LISTEN / TALK / fast-LOAD-open / fast-SAVE-open of the run is answered with the token `STAT = $3C` (SCANREQ) instead of being executed. `vcall` in `patches/vdrive.s` sees it, saves the registers and runs `bus_scan`, then sends the command again.
**The scan** (`bus_scan`, about 100 bytes of the reclaimed tape space): `SCAN` (14) releases the automatic units' numbers and returns how many units need one (0 = skip everything), then for 8, 9, ... `LISTEN` + `SECOND $6F` is sent through the stock KERNAL (the Teensy answers "not mine" for the released numbers); if the KERNAL sets ST bit 7 nobody answers there and `FREE n` (15) hands the number to the next waiting unit; `UNLSN` otherwise. It stops when no unit is waiting (reply 0) or after 30. **Why SECOND:** every drive on the bus pulls DATA low as soon as ATN falls, whatever address follows, so LISTEN alone only fails on a completely empty bus (checked in VICE with true-drive 1541s: LISTEN to an absent number gives ST=0 as soon as any drive is attached, SECOND gives $80). A real drive gets `LISTEN n, SECOND $6F, UNLSN` and nothing in between, which is what `OPEN15,n,15` does anyway. The number being probed lives on the stack (no zero page is free inside a user program); ST, `$A7`, `MBX_ARG`, X and Y are saved and restored.
**Cost.** Nothing at all with no card/stick or both numbers pinned (no request is made); otherwise one failed probe per free number found, so two probes (about 3 jiffies measured in VICE including BASIC overhead, probably much less) plus a few ms per drive that answers. A KERNAL that does not know SCANREQ times out once (about 1 s, `vcall`'s usual limit) on the first call and the defaults stay: the request is made once per run (`scan_state` 1 -> 2 -> 3). An old firmware with this KERNAL never answers `$3C`, so nothing changes.
**User-visible.** `PEEK(56854)` / `PEEK(56855)` ($DE16/$DE17, registers `DEV0`/`DEV1`) give the SD / USB number now (0 = none; defaults until the first disk access), `LOAD"$",n` shows it in the header id, and the serial console prints it at start-up. Auto-assigned numbers are never written to EEPROM.
**Known limits.** A device that is not powered yet when the scan runs looks like a gap (it is only probed once per run; a late-starting ESP32 device could end up sharing a number: pin the numbers with `U0>`). Two units never get the same number, and a pinned number is never handed out to the other unit. The first call of the run costs a few ms extra.
**Tests.** `test_vio.py` (SCANREQ is answered once and only to the four transaction starters, nothing is executed meanwhile, released numbers reach the real bus, absent units get no number, pins win over defaults, stray FREE/SCAN are harmless), the firmware glue test (fake KERNAL doing the protocol with fake drives at 8/9: SD 10, USB 11, EEPROM untouched), and VICE twice: no drives (SD keeps 8) and true-drive 1541s at 8 and 10 (SD 9, USB 11, LOAD 8/10 reach the real drives, 12 is `DEVICE NOT PRESENT`).

## 17. D71 and D81 images (done on the host and in VICE)
`d64_t` carries a format (`D64_FMT_D64/D71/D81`) and the engine's geometry and BAM code is table-like instead of 1541-only: `bam_loc()` gives the count byte and bitmap of any track, `dir_track()`, `bam_tracks()`, `next_data_track()`, `chain_limit()` and the header offsets differ per format. **D71** (1571, 70 tracks = two 35-track sides): track 18 header/BAM/directory as a D64 (byte 3 of 18/0 = `$80`), side-2 free counts at 18/0 `$DD-$FF`, side-2 bitmaps in 53/0, track 53 holds no file sectors, allocation 17..1, 19..35, 52..36, 54..70, blank disk 1328 blocks free. **D81** (1581, 80 tracks x 40 sectors): header 40/0 (name `$04`, id `$16`, DOS "3D"), BAM in 40/1 (tracks 1-40) and 40/2 (41-80) at 6 bytes per track from `$10`, directory from 40/3, allocation 39..1, 41..80, blank disk 3160 blocks free. Sub-directories/partitions of the D81 are not supported (the root directory only). The format is chosen from the file size (error-info bytes after the data are ignored) by `d64_open_size()`; `CD:` and `$` recognise `.D64`, `.D71`, `.D81`, and host listings tag them `D64`/`D71`/`D81`. Tests (`test_d71_d81.py`, 28 cases, each run for both formats): files, directory growth to 72 entries, blocks free against `c1541 -list`, write/scratch/rename/disk-full rollback with the BAM identical to `c1541 -validate` after every mutation, format byte-identical to a c1541-formatted image (header, BAM, free count), geometry. One surprise: `c1541` itself refuses to write a 300 KB file to a D71 ("ILLEGAL SYSTEM T OR S"), so fixtures use smaller files.

## 18. Paths in names and `CD` (done on the host and in VICE)
One path syntax for `CD` and for names: `[/]dir/dir/image.d64[/file]`. A leading `/` starts at the card root, `..` (or `_`, the left arrow) goes up (from a mounted image it only unmounts), a component with an image suffix is mounted and ends the walk, any other component is a folder, and a component with `*` or `?` is not entered but is the pattern (`path_walk()` in `vdrive.c`, one host `chdir`/`mount` call per component). The consequences, all by the same rule: `CD:/GAMES/DEMO.D64` and `CD0:...`; `LOAD"$/GAMES/DEMO.D64",8` mounts and lists the image; `LOAD"$GAMES/",8` lists a folder; `LOAD"$/GAMES/DEMO.D64/S*",8` lists matching files of the image; `LOAD"GAMES/DEMO.D64/START",8,1` and `SAVE"/X.D64/NEW",8` work through the path; `LOAD"DEMO.D64",8` with nothing mounted mounts it and returns its directory (the SD2IEC convention). With an image mounted a bare name is always a file inside the image (`CD:_` leaves it; since 0.9.0.27 a `/` inside a name is literal there, and only a leading `/`, a component with an image suffix or `..` makes it a path). **Listings only list:** a directory listing by path (`$/GAMES/G.D64`, `$GAMES/`, `LOAD"G.D64"`) restores the folder and image selected before when its channel closes (`host.push` before the walk, `host.pop` on close; `restore_ch` per unit; the glue keeps the `/.vdrive` sidecar untouched meanwhile). `$==path` (also `$:==`, `$0:==`; a single `=` is the CBM listing modifier `$=T`, `$=P` and stays a pattern) does the walk for good, like `CD`, and lists. A *file* loaded through a path (`LOAD"GAMES/G.D64/START"`) also keeps the selection, because the program usually goes on to load more from the same image (`pop` with `out == NULL` drops the snapshot). Without the push/pop callbacks (old hosts) paths select for good. A path with nothing after the image shows the directory, so the "file" a LOAD of an image returns is its directory listing. Names can be up to 63 characters (the command buffer); the 16-character limit applies to the file name after the walk. `CD` also now accepts several components in one command. `UI`/`UJ`/`U9` answer `73,TRVDRIVE V1.0,00,00` and close all channels (works with nothing mounted), `UI+`/`UI-` are accepted, block commands (`U1` ...) give `31` instead of silently succeeding.

## 19. Alternative considered: RAM vectors + an Ultimax-banked handler (not built)
The TR+ author suggested avoiding a KERNAL replacement: point the KERNAL's RAM vectors (`$0314-$0333`) at a small stub in IO1/IO2 space, which banks in an Ultimax image holding the real handler, then banks out and returns, leaving no trace. It would run on the original TR as well as the TR+, which cannot serve a KERNAL replacement. We did not take this route, because it would cover less and cost more.

**What it would do**
* Cover the file-level API that goes through the vectors: `OPEN CLOSE CHKIN CKOUT CHRIN CHROUT GETIN CLALL LOAD SAVE`. That is enough for BASIC and for programs that use the standard KERNAL file calls.
* Need no ROM patch, so it also works on the original TR.

**What it wouldn't do**
* **Direct serial calls.** `LISTEN TALK SECOND TKSA CIOUT ACPTR UNLSN UNTLK` are plain `JMP`s into ROM, not vectored. Disk utilities, copiers and custom loaders that call them directly would bypass the hook. Our patch hooks at this level, so everything above it works unchanged. A vector hook has to reimplement or wrap the whole file layer (logical-file tables, current-device state, `STATUS`, `CLALL`) and forward real devices to the original routines.
* **The bus scan.** Automatic device numbers (16) probe with `LISTEN` + `SECOND`, a serial-level operation.
* **Persistence.** `RESTOR` (RUN/STOP-RESTORE), a reset, or a program installing its own LOAD/SAVE vectors removes the hook, so something would have to re-install the vectors. A patched ROM survives all of these.
* **Fast LOAD/SAVE.** Ultimax mode shows RAM only at `$0000-$0FFF` (plus ROML `$8000` and ROMH `$E000`), so a banked-in handler cannot write a loaded file to `$0801` or anywhere else above `$0FFF`. The block-copy loop would have to run in normal mode from the 512 bytes of IO code, which is what the `$DF00` window already does, so the Ultimax image adds little.
* **Interrupts.** While Ultimax is banked in, the IRQ/NMI/reset vectors come from ROMH, so the stub must disable interrupts or the image must carry its own trampolines.

**Possible follow-up (not started):** a reduced "plain TR" mode for the vectored calls only, reusing the `d64/` engine and mailbox registers unchanged, with only a new 6502 side. Whether it is viable depends on whether the original TR can re-apply the vectors after a reset or RESTORE without patching ROM.

## 20. Changing the disk from outside while a program runs (remote control; done on the host, builds with the real toolchain, not on hardware)
The C64 side has no way to ask for a different disk without BASIC, and an on-screen menu would have to save and restore the VIC state, so the choice is made over the TeensyROM's remote-control port (TCP 2112, also the USB serial port) instead.

**Commands** (`SerUSBIO.ino`, tokens in `Common_Defs.h`; both sit in the "allowed while busy" switch of `ProcessCommand()`, because every other command answers `Busy!` while an IO handler other than the TR menu runs):
* `VDriveGetToken 0x64E3`: `Drive(1)` (USB = 0, SD = 1, as the file commands). Reply `AckToken` + one JSON line `{"unit","storage","device","fixed","mounted","writable","path","cwd"}`, or `FailToken` + a text line.
* `VDriveSetToken 0x64E4`: `Drive(1)`, then a null-terminated image path on that card (`/dir/IMAGE.D64`; empty = unmount). Reply as for get (the state after the change).
* With the VDrive (or VDrive+SwiftLink) handler active they work on the live drive. With the TR menu showing (0.9.0.12) they work on the card instead, see below. File names come back lower-cased (names go through PETSCII), folder names as they were given; FAT does not care.

**Quiet point.** The set command runs in the main loop between polls, so it never splits a mailbox command. It then waits (calling `PollingHndlr_VDrive()` itself, so the C64 keeps being answered) until `VD_Quiet()`: 40 ms without a mailbox command and no open bus transaction or fast LOAD/SAVE, or 1 s without any mailbox activity at all (whatever was open has been abandoned). After 3 s without a quiet moment it gives up with "C64 is busy with the drive". A missing or non-image path is rejected *before* anything is touched, so a typo leaves the current disk mounted; a mount that fails after the check puts the previous image back.

**What the swap does.** It is `CD:` for the unit: `vdrive_attach()` closes every channel on the unit and resets the status channel to `00, OK`. The channel pool lives on the Teensy, so nothing has to be cleaned up on the 6502; a program that keeps a file open across a swap gets errors on it, as with a real drive. The new mount is written to the `/.vdrive` sidecar like any other.

**Browsing (0.9.0.9).** `GetDirectory` / `GetDirNDJSON` (what `trls` sends) are also let through while VDrive runs, so a client can find an image to mount. They are held back until `VDrive_RemoteQuiet()` (the same wait as the swap) says the drive is quiet; if the C64 keeps the drive busy for 3 s the client gets `FailToken` + "C64 is busy with the drive". The listing itself is upstream's `GetDirectoryCommand()` and runs to the end without polling the mailbox, so a C64 command arriving mid-listing waits for it: keep pages small (`trls` pages by 200) and watch for time-outs on hardware. Every other file command still answers `Busy!`.

**With the TR menu showing (0.9.0.12; host-tested only).** Nothing is mounted then (the handler only runs while a program does), and the commands used to answer "VDrive is not running". What a unit will mount at the next start is its `/.vdrive` sidecar (section 14: written on every mount, read in `VDrive_PreStart()` unless the launch comes from a disk image opened in the menu), so get and set work on that file: `VDrive_RemoteIdleStatus()` reports the remembered image (JSON as above plus `"running":false`; `device` is the fixed number from EEPROM or 0 = automatic; `mounted` is false when the file or image is gone) and `VDrive_RemoteIdleSet()` checks the image like the live set (62 / 74 / 33) and rewrites or removes the sidecar. No mailbox, no quiet wait, no handler state. The USB unit waits for the stick as the other remote commands do (`USBFileSystemWait()`).

**Paths (0.9.0.12).** The TR menu hands USB paths over as `//NAME.D64`; `VD_MountPath()` and the sidecar reader collapse repeated slashes, and `VD_PathFor()` no longer doubles the slash after a root `cwd`, so `trvdrive` shows `/NAME.D64` for both units.

**Not done:** a firmware-to-6502 "disk changed" flag or generation counter (nothing on the C64 side needs it yet), the other file commands while a program runs (still refused as busy), and the freezer integration (the Alt button is section 22). Client: `trvdrive` in the `teensyrom-rcp` tools.

## 21. Direct sector access: buffer channels, U1/U2, B-P, B-A, B-F (0.9.0.16; done on the host and in VICE, not on hardware)

Programs that read or write raw sectors (disk editors, copiers, save routines of some games) use buffer channels. On a real drive `OPEN n,8,n,"#"` takes a 256-byte buffer, and commands on channel 15 move whole sectors in and out of it. All of it is plain serial-bus traffic, so it is handled in `vdrive.c` like the other DOS commands, with no 6502-side change.

| Command | Here |
|---|---|
| `OPEN n,d,n,"#"` | channel mode 5. The buffer shares the union with the file/directory state of a slot (`u.b`: pointer + 256 bytes), so a slot costs no more RAM. Zero-filled on open. A `#2` style request for a particular buffer number is accepted and ignored. Takes a slot from the same pool of `VD_NSLOT` as files (error 70 when none is left). |
| `U1:ch d t s`, `UA:` | read track/sector into the buffer of channel `ch`, buffer pointer to 0. The drive number is ignored; the numbers can be separated by any non-digit (`,` space `:`). |
| `U2:ch d t s`, `UB:` | write the buffer to the sector. |
| `B-R`, `B-W` | **same as `U1` / `U2`.** The real `B-R` treats the first byte of the buffer as an end-of-data count; no emulator of note does, and programs that care use `U1`. |
| `B-P:ch,pos` | buffer pointer (low 8 bits). |
| `B-A:d t s` / `B-F:d t s` | mark a sector used / free in the BAM (works on D64, D71 and D81 BAM layouts, through the same `bam_load`/`bam_store` as file writes). `B-A` on a used sector answers `65,NO BLOCK,t,s` with the next free sector (0,0 when the disk is full) and changes nothing. |

Reading a buffer channel returns bytes from the pointer to 255 and flags EOI on byte 255. Writing (PRINT#, bytes after `LISTEN`/`SECOND $6n`) stores at the pointer and silently drops what does not fit. Errors: `70` no such buffer channel (or channel is not a buffer), `66,ILLEGAL TRACK OR SECTOR,t,s` for a sector outside the image geometry (D64 35/40 tracks, D71 70, D81 80), `30` too few numbers, `21`/`25` transfer failures, `26` write to a read-only image, `31` for any other `B-x`. Commands need a mounted image (`74` otherwise, as for the other DOS commands).

The library side is `d64_block_read/write/alloc/free` in `d64.c`: raw sector transfers go straight to the image callbacks, so a `U2` to a file's sector is not seen by a file channel that has already buffered it, as on a real drive.

**Not emulated:** memory commands (`M-R`/`M-W`/`M-E`, used to detect the drive type and to upload drive code), user jumps `U3`-`U8`, `B-E`, error bytes in `.d64` images (no 21-29 errors from the image), GCR-level access. Fast loaders and copy protection that rely on drive code cannot work.

## 22. Switching disks with the Alt button (0.9.0.17; done on the host and as a 6502 routine under py65, not on hardware)
An alternative to `trvdrive` that needs no computer: the TR+'s Alt button cycles through a list of images kept by the user.

**Lists.** One file per drive, `/vdrive-config.txt` in the root of the SD card (unit 0) and of the USB stick (unit 1). One image per line, `path, colour, flashes`: the path starts with `/` and is on that drive (no `sd:` / `usb:` prefix), colour is 0-15 (C64 border colour, default 1), flashes 0-20 (default 1; 0 = silent). `#` lines, blank lines and lines that do not start with `/` are skipped; the two numbers are taken from the right, so a path may contain a comma. There is no JSON parser in the firmware, hence a line format. Read at every press (the file is small: 2 KB, 32 entries), so editing it on a PC needs no restart.

**Taps.** One tap acts on the SD list, two taps within 1 s on the USB list; a single tap therefore acts 1 s after the press, a double tap at once. The tap counter is fed by `SpecialBtn_VDrive()` (called from `loop()`), the work is done from `PollingHndlr_VDrive()`.

**Choosing.** The entry after the mounted image (compared case-insensitively with the entry paths), wrapping; with nothing mounted, or a mounted image that is not in the list, the first entry. Entries whose image has vanished (or is not a disk image) are skipped; the mount itself is `VDrive_RemoteSet()`, so it waits for the quiet point (section 20), closes the unit's channels and rewrites the `/.vdrive` sidecar. If the C64 never lets go of the drive, or nothing in the list is usable, the current disk stays.

**Feedback.** The Teensy cannot write `$D020`, so the KERNAL does it: a new mailbox register `BORDER` (`$DE18`, `VIO_BORDER`) holds a request (`$80` idle, `$C0|c` set the border to c, `$B0` remember the border colour, `$D0` put it back; exact values only, `$DE` is what a late answer returns). `irq_border` (`patches/vdrive.s`, 37 bytes in the reclaimed tape space) replaces the `JSR $FFEA` (UDTIM) in the IRQ routine at `$EA31` (the operand at `$EA32` is retargeted; `tools/build.py` refuses a ROM without that `JSR`), reads the register twice, acts on it and ends in UDTIM (`$F69B`). The colour is remembered in `$9C`, a tape-only zero page byte. The sequencer on the Teensy (`VD_FlashPoll()`) sends SAVE, waits 100 ms, then per flash SET (250 ms) and RESTORE (750 ms; 100 ms after the last one) and ends with `$80`. A new press during a running sequence first restores the colour, then starts the new one. The error flash (no card or stick, no or bad list, nothing usable, drive busy) is red, 100 ms on / 100 ms off, ten times (2 s).
Limits: nothing flashes under a program that masks the KERNAL IRQ or replaces its handler without chaining to `$EA31`, or if the program owns `$D020` itself (the switch still happens); a program using `$9C` loses it for the length of a flash. Before the mailbox is live (BASIC init, TR menu handler) the routine reads whatever the menu's register 24 returns, which only matters if that is exactly `$B0`, `$D0` or `$C0-$CF` twice in a row.

**Wiring.** The Alt button's action is a TR setting (`rwRegPwrUpDefaults2`, bits 5:3: AutoLaunch, Pause, TR menu, Reboot, None; `SetUpMainMenuROM()` points `fSpecialBtnChange` at the matching function whenever the menu is entered). `InitHndlr_VDrive()` takes the button over only when the setting is **None**, so a chosen action is never overridden; the menu setting is the opt-in. (A sixth "VDrive" entry would need the 6502 menu changed.) Takes effect for any program launched with the VDrive handler (also VDrive+SwiftLink).

## 23. VDrive Config.prg and the host text channels (0.9.0.18; firmware and channels done on the host, the program run in a BASIC interpreter model only, not on hardware)
**Why a channel.** A C64 program can list the card (`$/path/`) and mount (`CD:`), but cannot read or write a plain host file such as `/vdrive-config.txt`, and the mounted-image path is not shown by any listing. Two new channel names fill the gap, handled in `vdrive.c` (`open_hostfile()`, channel modes 6 / 7) and implemented by four optional host hooks (`hf_open` / `hf_read` / `hf_write` / `hf_close`, `IOH_VDrive.c: VD_Hf*`):
* `OPEN n,d,n,"!CONFIG"` reads and `"!CONFIG,W"` rewrites the unit's `/vdrive-config.txt` (section 22; a write replaces the file, a read of a missing file gives status 62).
* `OPEN n,d,n,"!STATE"` (read only; `,W` gives 26) returns two lines: the mounted image's path (empty when none) and the folder.
Text crosses as PETSCII with CR line ends; the glue converts to ASCII with LF (a CRLF file reads with single CRs). The channels move 64-byte chunks (`VD_HFBUF`); a read flags EOI on the very last byte (the next chunk is fetched one byte early). The host keeps one such file open at a time: opening another closes the first. File names go through the same upper/lower mapping as paths (section 14), so a list saved by the program has lower-case paths (FAT does not care).

**The program.** `util/vdconfig.bas` is BASIC V2 (tokenised with `petcat` by `tools/make_util.py`; `make util` writes `build/util/VDrive Config.prg`, loads at `$0801`). Run it from the TR menu with the VDrive handler (it reads the device numbers from `PEEK(56854/56855)`, `$DE16/$DE17`; with neither present it says so and ends). A 40-column screen: two lines showing what each drive has mounted, the current list (SD or USB) with the mounted entry marked `*`, each entry with its colour (a reverse block in that colour, plus the number) and flash count; keys: cursor up/down, RETURN mounts the entry (`CD:path`), `B` browses the card for any image and mounts it, `U` unmounts, `A` adds (browser, then colour and flashes), `E` edits (path, `?` browses for it, colour, flashes), `D` deletes (asks), `+` / `-` move an entry down / up, `T` switches between the SD and USB lists, `R` re-reads everything (the Alt button may have switched the disk), `Q` quits. Every change to a list is written at once. Comments and other lines of the file are kept in place; only lines starting with `/` are entries. At most 32 entries and 2000 bytes (what the firmware reads). The line editor (path, colour, flashes) uses `GET` so any character can be typed; the left arrow cancels.
**Browser.** Lists `$/folder/` (a listing by path leaves the mounted image alone) and parses the BASIC listing: only folders and `.D64/.D71/.D81` are shown (the host listing hides names over 16 characters and dot files, so those cannot be picked).
**Things this had to respect.** Closing channel 15 closes every channel of the unit (as on a real drive), so the program keeps one command channel open per drive (logical files 15 and 14) for the whole run and only closes them at the end. `FOR` always runs its body once (all loops that can be empty are guarded). Variables are distinct in their first two characters, and the tokenised program was checked for keywords hiding in names.
**Tests.** `tests/test_vdconfig.py` runs the program in `tests/basic_interp.py`, a small BASIC V2 interpreter (screen with cursor addressing and scrolling detection, scripted keys, `GET#` of a 0 byte giving `""`, `FOR` semantics, `RETURN` dropping FORs), against a model of the drive that follows `vdrive.c`. That checks the program's logic, screen layout and keystrokes, **not** the real C64 BASIC or timing; `make test` runs it. The channel code itself is tested in `tests/test_vio.py` (`test_host_text_files`) and `tests/fw_host` (real files, PETSCII/ASCII, chunking).

## 24. Copy: `C:new=old[,old2...]` (0.9.0.19; done on the host, not in VICE or on hardware)

`OPEN 15,8,15,"C:NEW=OLD"` copies a file on the mounted image; `"C:NEW=A,B,C"` joins up to 8 files into one, byte after byte (two PRGs joined keep the second load address in the data, as on a real drive). Drive prefixes (`C0:NEW=0:OLD`) are accepted and ignored, and each source is a pattern (its first match). The new file gets the type of the first source; locked flags are not copied.

`d64_copy()` in `d64.c` checks every source before anything is written (`62` and no new file if one is missing; `63` if the new name is taken), creates the new file and streams the sources through a 64-byte buffer. A disk full (`72`) or a broken source chain removes the partial copy, so a failed `C` leaves the disk as it was. The second lookup of each source skips the new file's own entry, which could otherwise match a source pattern (it can take a freed directory slot ahead of the source). Errors: `34` no `=` or an empty name, `33` a wildcard in the new name or more than 8 sources, `31` for the dual-drive forms (`C1=0`, whole-disk duplicate), `64` for a REL source among several (a single REL source is copied record for record, see 25).

**Not done:** copying between the two units (`VDCOPY` does that, through the C64) or from the host file system.

## 25. REL files (0.9.0.20; done on the host, not in VICE or on hardware)

Relative files hold fixed-length records (1-254 bytes) over an ordinary data chain, with side sectors that index the data blocks so a record can be reached without walking the chain.

| On the C64 | Here |
|---|---|
| `OPEN 2,8,2,"NAME,L,"+CHR$(len)` | creates the file (one data block of empty records + side sector 0; on a D81 also the super side sector), or opens it if it exists. The length byte is taken raw, so `CHR$(44)` (",") or `CHR$(82)` ("R") are lengths, not syntax. A length that differs from the file's: `50`. `,L` on a file of another type: `64`. Without a length and no such file: `62`. Length 255: `33`. |
| `OPEN 2,8,2,"NAME"` | an existing REL file opens as REL whatever mode is asked for. |
| `PRINT#15,"P"CHR$(96+ch)CHR$(lo)CHR$(hi)CHR$(pos)` | record `lo+256*hi` and byte `pos`, both from 1 (0 means 1); `hi` and `pos` may be left out. Handled before the command's CR/LF are stripped, since the bytes are binary (13 is a record number like any other); bytes after the fifth are ignored. A record past the end answers `50,RECORD NOT PRESENT` (a write then makes it); `pos` past the record length: `51`. |
| `PRINT#2,...` | one LISTEN session = one record: bytes go in from the position, the rest of the record is zero-filled at `UNLSN`, then the next record is current. More bytes than the record holds: `51,OVERFLOW IN RECORD`, the excess is dropped. Writing past the end grows the file to that record, the records in between empty (`$FF` + zeros), and the last block is filled with whole empty records, as DOS does. |
| `INPUT#2` / `GET#2` | a read sends the record from the position up to its last non-zero byte (at least one byte, so an empty record reads as `CHR$(255)`), with EOI on the last, and moves to the next record. Reading past the last record: `50`, and the read times out. |
| `C:NEW=OLD` | a REL source is copied block for block (same record length, same records). Joining a REL file with others: `64`. |
| `S:NAME` | frees the data blocks and the side-sector chain (on a D81 starting at the super side sector). |

**Layout** (`d64_rel_*` in `d64.c`): directory entry type `$84`, bytes 21/22 the first side sector (D81: the super side sector), byte 23 the record length, the block count includes the side sectors (and the super side sector). A side sector holds a link, its number in its group (0-5), the record length, the t/s of all 6 side sectors of its group, and 120 data-block pointers; the last one's byte 1 is the index of its last used byte. D64 and D71 files have one group (720 data blocks, then `52,FILE TOO LARGE`). A D81 file has a super side sector (link to the first side sector, `$FE`, then up to 126 group pointers) and its side sectors are linked across groups. `tests/test_rel.py` walks every file the way a drive would (pointers equal the data chain, group lists, block count, record fill) and `c1541 -validate` must leave the BAM unchanged (it re-allocates side sectors, checked by hand).

**Memory:** a REL channel caches one data block (`vd_rel_t` fits in the existing channel union, so `vio_t` does not grow). A record that spans two blocks is read and written through that one buffer; sequential blocks follow the chain link, others are found through the side sectors (two sector reads; three on a D81).

**Not done:** the same REL file open on two channels at once does not share the cached block (the second channel can see stale data until the first writes it back, at the end of each record); a `LOAD` of a REL file sends its first record only. Not checked against a REL file written by a real drive.

## 26. Append: `OPEN n,d,n,"NAME,A"` (0.9.0.21; done on the host, not in VICE or on hardware)

`,A` (with or without a type, `"LOG,S,A"`, `"0:LOG,A"`) reopens an existing file for writing at its end: `d64_append()` walks the chain to the last sector, loads it into the write buffer with the cursor after its last byte, and marks the directory entry unclosed (the splat a real drive shows while the file is open); `CLOSE` closes it with the new block count. Missing file: `62`; a REL file: `64`.

The write structure remembers the old last sector, its last-byte index, the old type byte and block count. On a disk full (`72`) during the append, `abort_write()` frees only the sectors added since, restores the old last sector's link and length and the directory entry, so the file is exactly what it was (BAM and directory byte-identical; tested with 3 sectors and with none left). A new file is still removed on a disk full, as before.

## 27. Validate: `V` (0.9.0.22; done on the host, not in VICE or on hardware)

`V` (`V0`) used to be accepted and ignored. `d64_validate()` now rebuilds the BAM the way DOS does: every sector free (D71 track 53 stays all used, as `N` formats it), then the header (D81: and the two BAM sectors), the directory chain, the data chain of every closed file and the side-sector chain of every REL file (on a D81 from the super side sector) are marked used. An unclosed ("splat") entry is deleted without following its chain, so a file left open by a crash stops holding blocks. Sectors marked used by `B-A` but not in any file become free again. The BAM sectors are held in memory (two buffers: D64 18/0; D71 18/0 + 53/0; D81 40/1 + 40/2) and written once at the end; the directory sectors are rewritten only where an entry was deleted. A broken chain is followed as long as its links are legal (at most a disk's worth of sectors). Tracks 36-40 of a 40-track image are not in the standard BAM and are left alone. Read-only image: `26`.

Tested against `c1541 -validate` on D64, D71 and D81 holding a PRG, a SEQ, a REL file (with a second side sector), a scratched entry and a splat, after scrambling the BAM bytes: the BAM sectors come out byte-identical.

## 28. Lock and write protect from the C64: `L:name`, `W-1` / `W-0` (0.9.0.23; done on the host, not in VICE or on hardware)

Commodore DOS has no command for either (on a 1541 the lock flag needs a disk editor, write protect a notch tab). The CMD drives' syntax is the one software knows, so it is used here:

- `L[n]:pattern` toggles the lock flag (bit 6 of the type byte, `<` in the listing) of every closed file matching the pattern (`d64_lock`). `S` already skips locked files. No match: `62`; no name: `34`.
- `W-1` sets `d64_t.ro`, and every write path (`wr()` and the early checks in create / scratch / rename / format / copy / validate / block alloc / lock) then answers `26,WRITE PROTECT ON` as on a read-only image; reading is unaffected. `W-0` clears it, unless the image itself is read-only (`26`). It is not stored anywhere: `d64_open` clears it, so mounting an image (the same one again included) starts unprotected.

## 29. New images: `N:NAME.D64,ID` with nothing mounted (0.9.0.24; host and glue tests, not on hardware)

The agreed design from CLAUDE.md "next steps": with no image mounted, `N:` names a new image file instead of a disk. `N[0]:[path/]NAME.D64[,ID]` (or `.D71` / `.D81`) walks the folders of the path (a leading `/` = the card's root, as in `CD:`), asks the host to create the file, mounts it and writes a blank disk: header and disk name `NAME` (the file name without the suffix, at most 16 characters), ID (`00` when none is given), BAM and the first directory sector. With an image mounted, `N:` formats it as before, even with a `.D64` in the name.

- Host callback `vd_host_t.create(ctx, unit, name, size, &disk)`: 174,848 / 349,696 / 819,200 bytes, all zero, in the current folder, then mounted like `mount()` (so the `/.vdrive` sidecar remembers it). An existing file (or folder) of that name: `63`, nothing touched. The glue (`VD_Create`) probes with `exists()` first (FILE_WRITE would create or append), writes 4 KB zero blocks from a heap buffer, removes the file again if a write fails (`72`), and answers `26` if the card refuses the open.
- `d64_format_blank()` is `d64_format()` without the pass that zeroes every sector: the file is already zero, and writing it twice would double the time. The whole command still has to finish inside the KERNAL's mailbox wait (`vcall`, ~65,536 polls, about 1.6 s): a D64 is about what a full `N:` writes (verified on hardware), a D81 is 800 KB, which on a slow USB stick may not make it. **If it does not**, the KERNAL gives up on that one transaction (treats it as "not mine") but the Teensy finishes the job; the status channel then reports the result. To be checked on hardware.
- Errors: `33` no image suffix, a wildcard, an empty or over-long disk name, an ID that is not 2 characters, a path through an image; folder errors as in `CD:`.

Tested in `tests/test_vio.py` (all three formats, `c1541 -validate` leaves each new image unchanged, writable at once, folders, default ID, never replacing) and in the glue host test (`N:MADE.D81` on the stub SD card, read back by `c1541`; a folder path).

## 30. Plain files on the card (0.9.0.25; host and glue tests, not on hardware)

With nothing mounted, the host listing already showed every file of the folder as `PRG`; now a read opens it. `LOAD"GAME.PRG",8` (and `OPEN 2,8,2,"GAME.PRG"`) reads the file itself: the exact name, else `NAME.PRG` when the name has no dot, and a pattern (`"GA*"`) takes the first file in the folder listing that is not a folder or an image. `"DIR/GAME.PRG"` and `"/DIR/GAME.PRG"` walk the folders first (a path through an image keeps meaning a file inside the image); the folder stays selected afterwards, as for a file loaded through an image path.

- Host callbacks `plain_open` (0 / `62`; size out), `plain_read`, `plain_close` (named apart from the `hf_*` hooks of the `!CONFIG` / `!STATE` text channels); the glue keeps one `File` per unit (`VD_PlainOpen` opens with `FILE_READ`, so nothing is created, and refuses folders). Channel mode 9 (`vd_plain_t`, fits the channel union) fetches 256 bytes at a time and flags EOI from the file size. One plain file per unit at a time (a second: `70`). Reading only: `SAVE` / `,W` with nothing mounted still answer `74`.
- A pattern is resolved through the host listing callbacks, which share the glue's directory handle with a host listing channel: a `LOAD"PAT*"` while a host directory listing is open on the same unit would end that listing. The KERNAL reads a listing to the end before anything else, so this only matters for programs that interleave the two.

## 31. Folders on the card: `MD:NAME`, `RD:NAME` (0.9.0.26; host and glue tests, not on hardware)

CMD syntax, with nothing mounted: `MD[n]:NAME` makes a folder in the current one (`63` if the name is taken by a file or folder), `RD[n]:NAME` removes a folder if it is empty (`62` no such folder, or a file; `63` not empty, nothing removed). Names may not hold `/ * ? : = , "` or be `.`, `..`, `_` (`33`); no name: `34`. Host callbacks `mkdir` / `rmdir`; the glue checks with `exists()` / a directory open first, since the SD and USB `mkdir`/`rmdir` only say yes or no (a refusal there answers `26`). With an image mounted both answer `31`: on a D81 they would make and remove partitions (1581 sub-directories), which are not supported.

## 32. VDrive+Swift+REU: the drive, SwiftLink and an REU together (0.9.0.28; host tests, VICE, works on a real C64)

A fourth Special IO, `IOH_VDriveSwiftREU` ("VDrive+Swift+REU", `firmware/IOH_VDriveSwiftREU.c`), runs the virtual drive, the SwiftLink / Turbo232 modem and the TeensyROM REU (`IOH_REU.c`, unchanged) at once. Three things were in the way.

- **IO2.** The window used all of `$DF00-$DFFF`, and the REU answers there too (11 registers at `$DF00-$DF0A`, repeated every 32 bytes). The window is now **128 bytes at `$DF80`** (`VIO_WIN`, `VIO_WIN_OFS` in `d64/vdrive_io.h`) for every VDrive handler, so one KERNAL image serves all of them; the combined handler gives `$DF00-$DF7F` to the REU (4 of its 8 register mirrors, enough for software that writes `$DF00-$DF0A`), the plain handlers leave it unanswered. No layout gives the drive 254 contiguous bytes next to the REU, so the cost is the same for any choice: fast LOAD / SAVE move a block in two mailbox calls instead of one. A name of 128 characters or more no longer fits the window: the KERNAL then opens the file through the byte-level path (`open_named` in `patches/vdrive.s` answers "not mine" and the stock code sends the name with `CIOUT`). The engine clamps `LEN` to the window, so an old KERNAL image cannot make it read past it. **KERNAL images built before this change no longer match the firmware: rebuild and re-copy `kernal-tr.bin`.** The shared name copy saved 11 bytes of the reclaimed tape space (666 of 683 used).
- **RAM.** SwiftLink's receive queues take about 355 KB of RAM2, and the REU wants up to 512 KB. But the REU fills its banks from the RAM1 image (`RAM_Image`, 128 KB) first, which nothing uses after a PRG launch (no cartridge), and only then from RAM2, rounding the total down to a power of two. With SwiftLink's queues allocated first, the REU ends up at **128 KB** (a 1700), its status register says so (bit 4 clear), and software that requires 512 KB will refuse it. Below 128 KB `InitHndlr_REU` disables the REU and its registers read as open bus; the drive and SwiftLink keep working. Init order: SwiftLink, REU, VDrive. The firmware build reports about 488 KB of RAM2 free for `malloc` before any handler starts.
- **The bus-snoop hook.** `fBusSnoop` is one pointer. `KernalCheck` sits on it to serve the replacement KERNAL, and an REU command that waits for a write to `$FF00` replaces it with `REU_FF00_W_Check`, which sets it to `NULL` when the transfer starts. Either would stop the KERNAL being served. `IO2Hndlr_VDriveSwiftREU` compares the hook before and after each REU register access: armed, it installs `VDREU_Snoop`, which watches for the `$FF00` write and otherwise calls the displaced snoop (`KernalCheck`), then puts it back after the trigger; cleared (an immediate command, or a new command while armed), it puts the displaced snoop back at once. In the normal case (no `$FF00` wait) nothing changes per cycle; while armed, KernalCheck runs behind one more call.

Also: `VD_IsHandler()` (`IOH_VDrive.c`) lists the VDrive Special IOs for the places that treat them alike (PreStart, remote control, listings while a program runs). `VDriveSwift_Remap()` gives the combined handler to menu programs paired with SwiftLink **or** the REU when it is the default Special IO; with VDrive or VDrive+SwiftLink as the default, REU programs keep the plain 512 KB REU. The alt button: `InitHndlr_REU` points it at "save REU to file", and VDrive takes it for disk switching only when the alt-button setting is "None" (as before). The new enum entry sits after `IOH_VDriveSwift`, so the numbers of the handlers after it (`IOH_Debug`, ...) move by one: an EEPROM default of Debug becomes a different handler.

Tested: `tests/fw_host` (IO2 split both ways, the window at `$DF80`, init and poll order, the alt-button choice, the snoop hand-back through immediate, armed, re-armed and cancelled REU commands, the remap), `tests/test_vio.py` (block sizes, clamping, wrap), the VICE runs (`make test`: every fast LOAD / SAVE now goes in 128-byte blocks; a 127-character name takes the fast open, a 130-character one the byte-level path), and the TR+ firmware build (arduino-cli, teensy:avr 1.61.0). On a real C64 the owner reports it works (2026-10-23). The checks listed for that run: `BUSTEST` (now 128-byte window passes), an REU program (REU-Checker reports 128 KB) while a disk image is mounted, an REU program that uses the `$FF00` trigger, CCGMS over TCP alongside, and the RAM2 free count on the serial console after the three inits.

## 33. Default device 8 for LOAD / SAVE / VERIFY (KERNAL only; VICE, not on hardware)

With the tape code reclaimed, the stock default device 1 for `LOAD"NAME"` only hangs at "PRESS PLAY ON TAPE". The SX-64 KERNAL (no tape port) changes that default to 8, and so do we, the same way: BASIC's LOAD/SAVE/VERIFY parameter routine at `$E1D4` (in the KERNAL chip) does `LDX #$01` at `$E1D9` before `SETLFS`, and `P_E1DA` in `patches/vdrive.s` turns the operand into 8; `tools/build.py` refuses a ROM without that `LDX`. `OPEN` still defaults to device 1 (its own routine at `$E219`), as on the SX-64. The default is the number 8, not "the SD drive": with a real drive at 8 and the SD drive automatically at 9, `LOAD"NAME"` goes to the real drive. No firmware change (no REV bump); rebuild and re-copy `kernal-tr.bin`.

Tested: the `basic` VICE run (`LOAD"SAVEME"`, `VERIFY"SAVEME"`, `SAVE"@0:SAVEME"` with no number, then read back with `,8`).

## 34. VDrive+REU: the drive and a full-size REU (0.9.0.31; host tests and firmware build, not on hardware)

A fifth Special IO, `IOH_VDriveREU` ("VDrive+REU", `firmware/IOH_VDriveREU.c`), runs the virtual drive and the TeensyROM REU (`IOH_REU.c`, unchanged) without SwiftLink. IO1 `$DE10-$DE18` is the mailbox (`$DE00-$DE0F` unanswered, as in the plain VDrive handler), IO2 is split as in section 32 (`$DF00-$DF7F` REU registers, `$DF80-$DFFF` window), and there is no per-cycle callback. Init: REU, then VDrive (the REU sets the alt button to "save REU" and clears `PendingfBusSnoop`; VDrive re-stages `KernalCheck` and takes the button when the setting is "None").

- **Size.** Without SwiftLink's ~355 KB of receive queues the REU's `AllocRAMBanks` takes the 128 KB RAM1 image and 48 banks (384 KB) from the RAM2 heap, which has about 488 KB free before a handler starts: the full **512 KB** that upstream's REU allows (`MAX_REU_BANKS` = 64; status bit 4 set, a 1764/1750-class REU). The VDrive state (`vio_t`, the config buffers) is static `DMAMEM`, already out of that free count, so the drive costs the REU nothing. Bigger than 512 KB would need upstream's `MAX_REU_BANKS` and its PSRAM/SD back ends, and RAM2 does not have it anyway.
- **Shared code.** The IO2 split and the `fBusSnoop` hand-back (`VDREU_Snoop`, `vdReuBase`) moved from `IOH_VDriveSwiftREU.c` into `IOH_VDriveREU.c` as `IO2Hndlr_VDriveREU`; VDrive+Swift+REU now uses that handler (same code, renamed). `IOHandlers.h` includes `IOH_VDriveREU.c` right before `IOH_VDriveSwiftREU.c`.
- **Remap.** `VDriveSwift_Remap()` now gives menu programs paired with the REU the VDrive+REU handler whenever the default Special IO is any VDrive one, VDrive+Swift+REU included (before: VDrive+Swift+REU with its 128 KB REU when that was the default, the plain REU without the drive otherwise; the reason, "the combined one is smaller", is gone). SwiftLink programs are unchanged: VDrive+Swift+REU when that is the default, VDrive+SwiftLink otherwise. `VD_IsHandler()` lists the new handler.
- **List order.** The enum and table entry sits after `IOH_VDriveSwiftREU` (under `Fab04_REU`), so the stored index of the four existing VDrive entries stays, but Kernal Replace and Debug move down by one again: a stored default of Kernal Replace now selects VDrive+REU.

Tested: `tests/fw_host` (wiring, IO1 split, init and poll order, the alt-button choice, the `$FF00` snoop hand-back through the shared handler, the remap for every default), the full `make test`, `git apply --check` on pristine upstream `5728ddc9`, and the TR+ firmware build. Not run on hardware: check that REU-Checker reports 512 KB with VDrive+REU while an image is mounted, an REU program with the `$FF00` trigger, and the RAM2 free count on the serial console after init.
