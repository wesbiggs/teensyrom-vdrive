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
*Since 0.9.0.35 the TR menu probes the bus before every launch (section 37), so the lazy scan below is only the fallback.*
**Problem.** A unit that claims 8 or 9 hides a physical drive (or SD2IEC, Meatloaf, ...) with the same number: the KERNAL never puts the call on the serial bus. **Rule.** Use the first free numbers from 8 up: SD first, USB second, only for units that are present, unless the user pinned a number with `U0>`.
**Who probes.** Only the 6502 can see the bus. **When.** Not at reset: the Teensy's own IO1 handler owns `$DE00-$DE3F` until the program has been loaded and the VDrive handler takes over (`rsIOHWSelInit`), so a mailbox call during BASIC init would hit the TR's menu registers. The scan is therefore lazy: `VDrive_PreStart()` calls `vio_arm_scan()` (state 1) when some present unit is automatic, and the first LISTEN / TALK / fast-LOAD-open / fast-SAVE-open of the run is answered with the token `STAT = $3C` (SCANREQ) instead of being executed. `vcall` in `patches/vdrive.s` sees it, saves the registers and runs `bus_scan`, then sends the command again.
**The scan** (`bus_scan`, about 100 bytes of the reclaimed tape space): `SCAN` (14) releases the automatic units' numbers and returns how many units need one (0 = skip everything), then for 8, 9, ... `LISTEN` + `SECOND $6F` is sent through the stock KERNAL (the Teensy answers "not mine" for the released numbers); if the KERNAL sets ST bit 7 nobody answers there and `FREE n` (15) hands the number to the next waiting unit; `UNLSN` otherwise. It stops when no unit is waiting (reply 0) or after 30. **Why SECOND:** every drive on the bus pulls DATA low as soon as ATN falls, whatever address follows, so LISTEN alone only fails on a completely empty bus (checked in VICE with true-drive 1541s: LISTEN to an absent number gives ST=0 as soon as any drive is attached, SECOND gives $80). A real drive gets `LISTEN n, SECOND $6F, UNLSN` and nothing in between, which is what `OPEN15,n,15` does anyway. The number being probed lives on the stack (no zero page is free inside a user program); ST, `$A7`, `MBX_ARG`, X and Y are saved and restored.
**Cost.** Nothing at all with no card/stick or both numbers pinned (no request is made); otherwise one failed probe per free number found, so two probes (about 3 jiffies measured in VICE including BASIC overhead, probably much less) plus a few ms per drive that answers. A KERNAL that does not know SCANREQ times out once (about 1 s, `vcall`'s usual limit) on the first call and the defaults stay: the request is made once per run (`scan_state` 1 -> 2 -> 3). An old firmware with this KERNAL never answers `$3C`, so nothing changes.
**User-visible.** `PEEK(56854)` / `PEEK(56855)` ($DE16/$DE17, registers `DEV0`/`DEV1`) give the SD / USB number now (0 = none; since section 37 right from the program's first instruction), `LOAD"$",n` shows it in the header id, and the serial console prints it at start-up. Auto-assigned numbers are never written to EEPROM.
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

## 23. VDrive Config.prg and the host text channels (0.9.0.18; firmware and channels done on the host, the program run in a 6502 simulator; works on hardware since 0.9.0.35, owner 2026-10-30)
**Why a channel.** A C64 program can list the card (`$/path/`) and mount (`CD:`), but cannot read or write a plain host file such as `/vdrive-config.txt`, and the mounted-image path is not shown by any listing. Two new channel names fill the gap, handled in `vdrive.c` (`open_hostfile()`, channel modes 6 / 7) and implemented by four optional host hooks (`hf_open` / `hf_read` / `hf_write` / `hf_close`, `IOH_VDrive.c: VD_Hf*`):
* `OPEN n,d,n,"!CONFIG"` reads and `"!CONFIG,W"` rewrites the unit's `/vdrive-config.txt` (section 22; a write replaces the file, a read of a missing file gives status 62).
* `OPEN n,d,n,"!STATE"` (read only; `,W` gives 26) returns three lines: the mounted image's path (empty when none), the folder, and (since 0.9.0.40) the TR menu's seven colour references as hex digits (`IO1[rwRegColorRefStart..+6]`, which the Teensy keeps after the menu hands over to the VDrive handler: background, border, banner, time, option, sources, name; `044A7ED` with the menu's defaults).
Text crosses as PETSCII with CR line ends; the glue converts to ASCII with LF (a CRLF file reads with single CRs). The channels move 64-byte chunks (`VD_HFBUF`); a read flags EOI on the very last byte (the next chunk is fetched one byte early). The host keeps one such file open at a time: opening another closes the first. File names go through the same upper/lower mapping as paths (section 14), so a list saved by the program has lower-case paths (FAT does not care).

**The program.** `util/vdconfig.s` is machine code (first written in BASIC V2, rewritten because reading the list and the browser listings byte by byte through `GET#` and BASIC strings was slow): `tools/make_util.py` assembles it like VDCOPY, `make util` writes `build/util/VDrive Config.prg`, loads at `$0801` with a `SYS` stub, buffers are BSS at `$2000-$4FFF`. Run it from the TR menu with the VDrive handler. It reads the device numbers from `$DE16/$DE17`, which the TR menu's bus probe (37) settled before the launch (until 0.9.0.35 it made one bus access first so the lazy scan of 16 had run); with neither present it says so and ends. A 40-column screen: two lines showing what each drive has mounted, the current list (SD or USB) with the mounted entry marked `*`, each entry with its colour (a reverse block in that colour, plus the number) and flash count; keys: cursor up/down, RETURN mounts the entry (`CD:path`), `B` browses the card for any image and mounts it, `U` unmounts, `A` adds (browser, then colour and flashes), `E` edits (path, `?` browses for it, colour, flashes), `D` deletes (asks), `+` / `-` move an entry down / up, `T` switches between the SD and USB lists, `R` re-reads everything (the Alt button may have switched the disk), `Q` goes back to the TR menu (37). Every change to a list is written at once. Comments and other lines of the file are kept in place; only lines starting with `/` are entries. At most 32 entries and 2000 bytes (what the firmware reads). The line editor (path, colour, flashes) takes any key from 32 to 94; the left arrow cancels. The screen is in the lower-case character set (`CHR$(14)`, and `CHR$(8)` so Shift+C= cannot switch it back): paths show as they are on the card, the messages in mixed case. The `PS` macro swaps the case of the letters in its strings at assembly time (`'a'-'z'` to `$41-$5A`, `'A'-'Z'` to `$C1-$DA`); the strings for the drive (`CMDS`, the types table) are left as they are, and the types are shown in capitals by `fchu`.
**Colours (0.9.0.40).** The program takes the TR menu's theme from the third `!STATE` line (all seven digits or none; a firmware without the line leaves the menu's defaults in `THEME`) and draws as the menu does: border and background from it, a reverse banner across row 0 (its 40th column is stored straight into screen and colour RAM, since printing it would link rows 0 and 1), keys in reverse in the option colour followed by their meaning in the sources colour (`KEY` / `KEYS` macros), labels in the sources colour, paths and names in the name colour, the cursor marks in the option colour, messages and the Alt gesture in the time colour. Screen strings select a colour with the bytes 3-7 (`C_BAN`..`C_NAME`, colour reference + 1), which `fch` turns into a store to `$0286` instead of printing; an entry's own colour block is set the same way, so the colour codes table went.
**Inside.** The file is kept as text (`TXT`, 4 KB, lines ending in CR, at most 119 lines of 255 characters) with an index of its lines; the entries point into it. A change (one line replaced, removed, swapped with another, or added) rebuilds the text into a second buffer, copies it back, re-indexes and re-parses, then writes the whole file; over 2000 bytes it is kept on the screen but not written (`LIST TOO LONG TO SAVE`). All output goes through one routine that writes fixed-width fields (the BASIC `left$(x$+sp$,39)` padding) and clears the screen editor's quote mode after each character, so a `"` in a path cannot turn the colour codes after it into characters.
**Browser.** Lists `$/folder/` (a listing by path leaves the mounted image alone) and parses the BASIC listing: only folders and `.D64/.D71/.D81` are shown (the host listing hides dot files; since 0.9.0.47 long names appear by their alias, section 44).
**Things this had to respect.** Closing channel 15 closes every channel of the unit (as on a real drive), so the program keeps one command channel open per drive (logical files 15 and 14) for the whole run and only closes them at the end. `FOR` always runs its body once (all loops that can be empty are guarded). Variables are distinct in their first two characters, and the tokenised program was checked for keywords hiding in names.
**Tests.** `tests/test_vdconfig.py` runs the assembled PRG in py65; `tests/c64_sim.py` answers its KERNAL calls (OPEN/CHKIN/CHRIN/CHROUT/GETIN/PLOT/LISTEN/SECOND...: a screen with cursor addressing and scrolling detection, scripted keys, CHRIN answering CR while ST is not 0, writes reaching the drive at UNLISTEN) against a model of the drive that follows `vdrive.c`; the screen model knows both character sets, and a write of `VIO_MENU` to `$DE10` ends the run ("menu"). That checks the program's logic, screen layout and keystrokes, **not** the real KERNAL's screen editor or timing; `make test` runs it (skipped without py65). The channel code itself is tested in `tests/test_vio.py` (`test_host_text_files`) and `tests/fw_host` (real files, PETSCII/ASCII, chunking).

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
- `d64_format_blank()` is `d64_format()` without the pass that zeroes every sector: the file is already zero, and writing it twice would double the time. The whole command still has to finish inside the KERNAL's mailbox wait (`vcall`, ~65,536 polls, about 1.6 s): a D64 is about what a full `N:` writes (verified on hardware), a D81 is 800 KB, which on a slow USB stick may not make it. **If it does not**, the KERNAL gives up on that one transaction (treats it as "not mine") but the Teensy finishes the job; the status channel then reports the result. On hardware a D81 on a slow (10+ year old) USB stick took about 0.8 s, half the wait (owner, 2026-11-01).
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

## 35. Bad sectors from the image's error table (0.9.0.32; unit and glue host tests, not in VICE or on hardware)

Disk copiers and VICE store a copy-protected disk's read errors as one byte per sector after the data: 683 bytes after a 35-track `.d64` (175,531 bytes), 768 after a 40-track one (197,376), 1,366 after a `.d71` (351,062), 3,200 after a `.d81` (822,400). The files were already accepted, but the table was ignored. Now `d64_t.errtab` points at it, and `rd()` (every sector read the engine makes) checks the sector's byte after fetching the data:

| byte | 0, 1 | 2 | 3 | 4 | 5 | 6 | 9 | 11 | 15 | 7, 8, 10, other |
|---|---|---|---|---|---|---|---|---|---|---|
| DOS error | none | 20 | 21 | 22 | 23 | 24 | 27 | 29 `DISK ID MISMATCH` | 74 `DRIVE NOT READY` | none (25 / 26 / 28 are write errors) |

A read error returns `D64_E_BADSEC` and stores the first one in `d64_t.bad` / `bad_t` / `bad_s`. The sector's data is still in the buffer, as in VICE (on a 1541 that is true for `23`, not for errors where the sector never arrives; a protection that checks the buffer contents after `20`-`22` would see the image's bytes). Most engine paths collapse errors to `-1`, so the drive layer (`read_err()` in `vdrive.c`, also reached from `err_from()` for `D64_E_IO` / `D64_E_BADSEC`) puts the stored code and sector on the error channel and clears it, else it reports what it did before (`21` for `U1` and REL reads, `74` otherwise). Each `LISTEN` / `TALK` to the unit clears a stored bad sector that some earlier step read and did not report (the clean-up reads of a failed write, a host-side read), so it cannot turn up on an unrelated operation.

What a bad sector does:
- `U1` / `B-R`: the status channel says e.g. `23,READ ERROR,17,00`; the buffer holds the image's data.
- A file read (`GET#`, byte-level `LOAD`): bytes up to the bad sector, then a time-out (`ST` bit 1); the status channel names the sector. A bad first sector sends nothing, so `LOAD` says `FILE NOT FOUND` as on a 1541. The fast `LOAD` ends the load at the bad sector with `ST` = `$42` (EOI + time-out; the KERNAL ends the load normally, and a program can check `ST`).
- The directory or the BAM: the listing stops at the bad directory sector; `OPEN` / `SAVE` / `S:` etc. fail with the sector's error, nothing is written.

Not emulated: write errors (a write to a bad sector succeeds and the sector still reads as bad, while on a 1541 rewriting a sector with `23` repairs it; `N:` does not clear the table either), `29` from an ID that differs from the header's (only the table's `11`), and anything that depends on timing, GCR or the drive's memory (no `M-R`/`M-E`).

**Firmware:** at mount, `VD_LoadErrTab()` (`firmware/IOH_VDrive.c`) reads the table (one 683-3,200 byte read) and keeps a `malloc` copy (RAM2 heap) only if a byte in it is a read error, so the usual all-`01` table costs nothing and a bad sector costs no extra card access; unmount frees it. No memory: the image mounts without the table. The memory backend (`d64_open_mem`, tests and the VICE mock) points `errtab` into the image.

Tested: `tests/test_vdrive.py` `BadSectorTests` (table offsets for every size, the byte-to-code map, `U1` status and data, a `LOAD` that stops at a bad third sector, a bad first sector, a bad directory sector, a bad BAM failing a `SAVE` with the image unchanged, no stale error, a D81), `tests/test_vio.py` (fast `LOAD` ends with `ST` = `$42` and the bus is clean afterwards), and `tests/fw_host` (an SD image with a bad sector keeps its table and reports `23,READ ERROR,17,00`; an all-OK table is dropped; unmount frees it).
## 36. CMD path syntax (0.9.0.33; unit tests, not in VICE or on hardware)
CMD drives (and sd2iec, which copies them) write a path in front of the name: `[n][path]:name`, the path `//` for the root, `/a/b/` relative, ending in `/` before the colon, `[n]` a partition number. Our own syntax (section 18) puts the path in the name: `/a/b/name`, a leading `/` = root. The two meet only at a single leading `/`, which CMD reads as "here" and we read as the root, and only CMD has the colon form. The rule: **a path before a colon is CMD; anything after the colon, and a name without a colon, is ours**. So `CD:/GAMES` and `LOAD"$/GAMES/G.D64"` keep their meaning (documented and used on hardware), and the CMD forms work as on a CMD HD.
- `cmd_path()` in `vdrive.c` rewrites `[n]/path/:rest` into our syntax by dropping the first `/` (`//a/` -> `/a/`, `/a/` -> `a/`; `/` alone -> nothing) and appending the rest (adding a `/` when the path did not end in one, `N//SUB:X.D64`). The colon must come before any `,` (OPEN modifiers stay after the name). Callers: `parse_open` (names; a non-empty CMD path is always a path, also with an image mounted, where a bare `a/b` would be a file name), `open_dir` (`$//GAMES/:*`, `$0/SUB/:A*`; still only lists, restored on close), `N` with nothing mounted.
- `CD` without a colon reads CMD: `CD/FOO` and `CD/FOO/BAR` relative, `CD//FOO` root, `CD/` the current folder (before: the root). `CD_`, `CD:_`, `CD:..`, `CD:/` unchanged.
- `MD`/`RD` take a CMD path (`MD//GAMES/:NEW`, or `MD/GAMES/NEW` with the name after the last `/`): the folder is walked between host `push` and `pop`, so the current folder stays; a path into an image or ending on a pattern is `33`.
- Not done: paths in `C:`, `R:`, `S:` (CMD allows them), `CP`/`G-P`/`$=P` (no partitions here: two units are two devices), wildcards inside a `CD` path component (sd2iec takes the first match), the 1581 `/` partition command (needs D81 partitions in the engine), CMD-style subdirectories inside `.DNP` images (a separate change).

Tested: `tests/test_vio.py` (`test_cmd_*`: every CD form, names relative and from the root, inside a mounted image, SAVE through a CMD path, listings by CMD path restoring the selection, MD/RD with and without the colon, refusals, `N` with a path).

## 37. The TR menu probes the bus before the launch, and the way back to it (0.9.0.35; host tests, the probe routine in VICE with true-drive 1541s; works on hardware, owner 2026-10-30)
**Why.** With the lazy scan (16) `$DE16/$DE17` hold the defaults until the program's first disk access, so a program that reads them first (VDCOPY, VDrive Config, anything written against our registers) had to make one bus access of its own. Only the 6502 can see which numbers real drives hold, and the mailbox is not live before the program runs; but the TR menu's own code runs right before every launch and can probe.
**Where.** Every PRG start from the menu goes through `XferCopyRun` in `MainMenu.asm` (remote launches, hot keys, auto-launch and "Exit to BASIC", which is a PRG item, included). Its first step is `rCtlKERNALPreStartWAIT`: the Teensy runs `KERNALPreStart()` while the C64 waits, which for a VDrive Special IO hooks the replacement KERNAL (`InitHndlr_KERNALReplace_PreStart`) and then runs `VDrive_PreStart()` (units attached, images mounted). The replacement KERNAL is served from the moment PreStart hooks it, so the probe has to hold it back.
**Protocol (no new menu registers or codes).** The menu clears `rwRegScratch` before PreStart. When some present unit has an automatic number, `VDrive_PreStart()` ends with `VD_ProbeBegin()`: `vio_probe_begin()` releases the automatic numbers (as `SCAN`, and marks the scan done so no SCANREQ follows), the count of waiting units goes to `rwRegScratch`, and `fBusSnoop` is saved and cleared, so the C64's own KERNAL is back while the probe runs (the replacement's serial hooks would talk to a mailbox that is not live in the menu). The menu then runs `VDriveProbe` (`firmware/vdrive_probe.asm`, inserted by `tools/make_tr_patch.sh`): for 8 to 30, `LISTEN` + `SECOND $6F` (+ `UNLSN` when a drive is there) through `$FFB1/$FF93/$FFAE`, as `bus_scan` does; each number nobody answers goes to `rwRegScratch` with another `rCtlKERNALPreStartWAIT`. The patched `KERNALPreStart()` first calls `VDrive_ProbeStep()`: while a probe runs, a nonzero `rwRegScratch` is a step (`vio_probe_free()`, as `FREE`), answered with the count still waiting; `$FF` (the menu ran out of numbers) ends it too. At 0 `VD_ProbeFinish()` puts `fBusSnoop` back (HIRAM state unknown, as PreStart leaves it). The menu reads the answer and stops at 0.
**Safe by construction.** A TeensyROM without VDrive never writes `rwRegScratch` in PreStart, so the same menu image skips the probe there. A launch whose first PreStart finds a probe still marked as running (a reset in the middle of one) drops it and starts afresh (`rwRegScratch` = 0 is never a step). If the handler starts with the probe unfinished (not with this menu), `InitHndlr_VDrive()` restores the numbers, re-arms the lazy scan and leaves the hook to `InitHndlr_KernalReplace()`. Both numbers pinned, or no card and no stick: no probe at all. The lazy scan stays for KERNAL images on other launch paths.
**Cost.** The same probes as the lazy scan, now during the "running..." dots before the launch instead of on the first disk access: a few ms per drive that answers, one PreStart round trip per free number found (two, usually).
**Back to the menu.** Mailbox command `VIO_MENU` (16): `vio_poll()` answers OK and sets `menu_req`; `PollingHndlr_VDrive()` takes it (`vio_take_menu()`), writes back any unsaved image (`VD_Flush`) and sets `BtnPressed`, so `loop()` does what the menu button does (`SetUpMainMenuROM()`, reset into the menu). VDrive Config sends it on `Q` with interrupts off, after closing its files; with no answer within about a second (or no reset within about 4 s) it returns to BASIC as before. Other programs can do the same: `POKE 56848,16` (`$DE10`).
**Tests.** `tests/test_vio.py` (`vio_probe_begin/free`: numbers, pins, absent units, no SCANREQ afterwards; `VIO_MENU` answered and reported once); `tests/fw_host` (PreStart asks for the probe and unhooks the KERNAL, each step, the end marker, the KERNAL hook restored, a new launch after an unfinished probe, no probe with pins; `VIO_MENU` sets `BtnPressed`; the lazy-scan tests run the fallback path); `tests/menuprobe_vice.py` (`make test-menuprobe`): the routine itself, assembled with ACME into a harness that does what `XferCopyRun` does, in VICE with the stock KERNAL and true-drive 1541s at 8 and 10, the Teensy side played over the real drive model: offers 9 and 11 (SD 9, USB 11), a pin, no probe when not asked (also the stale-`rwRegScratch` case), and every number up to 30 then `$FF`. The whole round trip through the real menu and Teensy is only possible on hardware: works there (owner, 2026-10-30).

## 38. The drive numbers in the power-on banner (0.9.0.36; py65, firmware glue and VICE, not on hardware)

The second banner line becomes ` 64K RAM  38911 FREE  VDRIVE SD 8 USB 9`: one leading space and 38 characters, like the stock ` 64K RAM SYSTEM  38911 BASIC BYTES FREE` (top line stock). The drive field is always 10 wide: `SD 8` / `SD10` / `NOSD`, a space, `USB 9` / `USB11` / `NOUSB`. BASIC's start-up message routine INITMS (`$E422`) lives in the KERNAL chip: it prints the banner string at `$E473` (`JSR STROUT` at `$E42D`), the free bytes (`$BDCD`, no leading space), then ` BASIC BYTES FREE` + CR (`$E460`, from `$E43D`) and ends in `JMP $A644` (NEW). `patches/vdbanner.s` ends the banner string after the top line and its two CRs (a 0 at `$E49A`), points the `JSR` at `vd_head` (the banner string, then ` 64K RAM  ` or the stock ` 64K RAM SYSTEM  `) and makes `$E43D` jump to `vd_banner` (the stock ending, or ` FREE  VDRIVE ` and the field, the CR, then NEW). `tools/build.py` refuses a ROM whose INITMS code differs.

**Where the numbers come from.** The banner is printed before the VDrive handler owns IO1 (section 16: the TR's menu handler answers there until the program is loaded), so the mailbox cannot be asked. The numbers travel in the image instead: `VDrive_PreStart()` runs after `InitHndlr_KERNALReplace_PreStart()` has loaded the image into `KernalFast`, while the C64 still waits in the menu, and `VD_BannerUnits()` writes the two bytes after the tag `VDUNITS:` (not printed; the firmware searches the 8 KB for it, so the address and the wording are free to change): per unit, SD then USB, `$FE` absent (or no number), `8-30` the number, `$FF` automatic and not settled yet. PreStart writes the pinned numbers and `$FF` for automatic units; when the TR menu's bus probe (section 37) ends, `VD_ProbeFinish()` writes the numbers it gave (`$FE` for a unit it found none for), so on that path the banner only prints. The bytes as built, `0 0`, mean "not written": the file on the card is never changed (the published SHA-256 holds), and the plain Kernal Replace handler and older firmware leave them so, which prints the stock line (absent is `$FE`, not 0, to tell the two apart). An image without the tag is left alone.

**Automatic units without the menu probe** (a launch path that does not run it, a menu without it, a probe cut short by a reset). The bytes still say `$FF`, and the scan's rule is simple, so the KERNAL applies it itself at power-on: for each automatic unit in order the first number from 8 up that is not fixed by a unit and where nobody answers `LISTEN n` + `SECOND $6F` on the real bus (`UNLSN` when somebody does). The probes go through the stock routines without the mailbox hooks: `iec_listen`, `iec_second`, `iec_unlsn` are the hooks' "not mine" tails in `vdrive.s`, now global labels (same bytes). Same bus, same rule, same answer as the menu probe or the later scan, unless a device is switched on or off in between (or was still booting at power-on, the same limit the scan has). Nothing is probed for fixed or absent units; a unit that finds every number taken shows as `NOSD` / `NOUSB`.

**Space.** 296 bytes at `$FB97` in tape-only routines (`$FB97-$FCD0`, up to 314 bytes; called only from the tape code at `$F8A1-$F8D6` and from tape code `vdrive.s` already reclaimed; `$FB8E-$FB96` and `$FCD1-$FCE1` stay, serial SAVE uses them; the build asserts the end). Zero page `$FB-$FE` (no program runs yet). Cost: a couple of probes at power-on, a few ms.

**Tests.** `tests/test_banner.py` (py65 on a synthetic image, INITMS run from `$E429` with BASIC's STROUT / LINPRT and CHROUT trapped and a fake serial bus): the stock line when nothing was written, automatic on an empty and a populated bus, fixed numbers skipped without a probe, absent units, every number taken, one- and two-digit numbers, always 38 characters, LISTEN + SECOND `$6F` (+ UNLSN) per probe, ST left at 0. `tests/fw_host`: the bytes for automatic, absent (`$FE`), neither present, fixed and mixed units, before and after the menu probe (the probed numbers; `$FE` when the menu ran out of numbers), an image without the tag, a tag at the very end. VICE (`make test`): `basic` shows `SD 8 NOUSB` (empty bus), `scan` shows `SD 9 USB11` with true-drive 1541s at 8 and 10, which the scan of the first `LOAD` then confirms. (The VICE mock writes `$FF`, so these runs exercise the KERNAL's own probe; the menu-probe path is the firmware writing plain numbers, covered by `tests/fw_host` and `test_banner.py`'s fixed-number cases.) Not on hardware: look at the banner after a launch with and without a real drive on the bus, with a fixed number, and with no card or stick.
## 39. Deleting image files on the card: `S:NAME.D64` (0.9.0.39; unit, glue host and VICE tests, not on hardware)

With nothing mounted, `S[n]:NAME.D64[,NAME2.D81...]` deletes image files (`.D64` / `.D71` / `.D81`) in the current folder; a name may carry a path (`S:GAMES/OLD.D64`, `S:/OLD.D64`) or a CMD path in front (`S//GAMES/:OLD.D64`, applied to the whole list as `cmd_path` rewrites it), and the selected folder stays as it was (host push / pop, as for `MD`/`RD`). The answer is the 1541's: `01,FILES SCRATCHED,nn,00`, with a name that is not there counted 0. Refused: a wildcard (`33`; deleting whole disks by pattern is too easy to get wrong), any other name (`64`; plain files on the card stay read-only from the C64, section 30), no name (`34`), a path that runs into an image (`33`). With an image mounted `S:` scratches files inside it, as before.

Host callback `scratch` (`62` no such file, `26` refused). The glue (`VD_Scratch`) opens the file `FILE_READ` first (a `FILE_WRITE` open would create a missing one), then `FILE_WRITE`: SdFat refuses that for a file with the FAT read-only attribute, so such an image stays (`26`), and `remove()` would refuse it too. Only one unit can have a given image mounted (SD and USB are separate cards), and `S:` runs only with nothing mounted on its own unit, so a mounted image is never deleted. A `/.vdrive` sidecar naming a deleted image just starts unmounted next time.

`L:` for card images (toggling the read-only attribute) was considered and dropped: SdFat 2.1.2, the version Teensyduino 1.61 bundles, can read the attribute (`isReadOnly()`) but has no call to set it; its directory-entry cache is private. Set the attribute on a computer.

**Tests.** `tests/test_vio.py` (a list with a missing name, folder paths, CMD paths, the cwd kept, 26/33/34/64, mounted = inside the image); `tests/fw_host` (`VD_Scratch` on the stub card, a file made read-only with `chmod` stays); VDSUITE `8` deletes its own images and folders in the `basic` VICE run.


## 40. VDCOPY: whole disks, sector by sector (utility only; py65 against `vdrive.c`, VICE with a true-drive 1541; not on hardware)
**Why.** Programs that upload their own drive code (fast loaders, most commercial games of the time) cannot run from the card (section 21: no `M-W`/`M-E`/`B-E`; the cartridge port has no serial-bus lines, so even an emulated 1541 on the Teensy could not answer the C64's `$DD00` loader code). With a real 1541 on the bus they run from a floppy, so the missing piece was getting a `.d64` onto a floppy (and a floppy into a `.d64`) from the C64 alone.

**How.** `VDCOPY`'s `D` menu entry copies the 683 sectors of a 35-track disk between any two units with the block commands both a 1541 and the vdrive already understand: `OPEN 2,src,2,"#"` and `OPEN 3,dst,3,"#"`, then per sector `U1:2,0,t,s` on the source, 256 `CHRIN`s, `B-P:3,0`, 256 `CHROUT`s, `U2:3,0,t,s` on the destination, each answer from the error channel read. No firmware or KERNAL change. Before the copy: a vdrive source may mount an image (`CD:path`); track 18 sector 0 of the source is read first and must have `A` / `$00` at bytes 2-3 (a 1541 BAM; a D71 has `$80`, a D81 keeps its header on track 40), else the program asks; a vdrive destination gets a new image (`CD:←` to unmount, then `N:NAME.D64`, section 29; `.D64` added when missing; an existing file answers `63` and nothing is written) or the mounted image is written over after a `Y`; a real destination may be formatted first with `N:` and the source's disk name and ID (characters `N:` cannot carry become `-`, or `0` in the ID), so the sector headers carry the ID the BAM names. After the last sector the destination gets `I`, so a 1541 drops the BAM it holds in memory.

**Errors.** A read error on the source (`20`-`29`, also an image's error table, section 35) is counted and the sector written as the drive gave it (a 1541 keeps the data of a `23`; the vdrive always has the image's bytes). The new image has no error table. A write error (`26` write protect, `74`, ...), a transfer that does not deliver 256 bytes (a bus error, or EOI before the last byte), or RUN/STOP ends the copy with the sector named on row 1.

**Screen.** The TR menu's colours (the third `!STATE` line, as in VDrive Config 0.9.0.40; the menu's defaults without a vdrive), a reverse banner, keys in reverse in the option colour; the whole program keeps the upper-case character set. The copy shows a map: rows 2-3 the track numbers 1-35 (columns 4-38), rows 4-24 the sectors 0-20; `.` waiting, `+` (option colour) the sector in work, a ball (name colour) done, a reversed digit (time colour) a read error's last digit. Row 1: units, track, sector, errors; at the end "DONE, n READ ERRORS" and "PRESS A KEY" in the banner. Written straight into screen and colour RAM (row address table), the status row through `PLOT` and padded to 39 columns (the 40th would link rows).

**Program changes around it.** A start menu (`F` files, `D` disk, `Q` back to BASIC with the colours found at the start); RETURN at the file selection goes back to the menu. `probe` clears `ST` first (it collects bits: the end of a `!STATE` read made the next probe report "nothing on unit"). The line editor got a text mode for paths (any printable key but `"`, 60 characters). The code grew past `$1FFF`, so the tables moved to `$2800-$3FFF` (`tools/make_util.py`: `bss=`).

**Speed.** Per sector a real 1541 moves 256 data bytes plus a command (~12 bytes) and an error-channel answer (~20 bytes) through the KERNAL's byte-level serial routines, at roughly the speed of a stock `LOAD` (a few hundred bytes a second), plus the drive's own seek and rotation: expect about a second a sector, 10-15 minutes a disk, on hardware (not measured yet). VICE says nothing useful about it: the mock stops the emulator for every mailbox byte, so the one full-disk run made while building this (SD image onto the true-drive 1541, with format) took 573 s in warp for that one direction (byte-identical, 683 sectors), almost all of it in monitor round trips; the test copies tracks 17-19 (59 sectors) each way, about a minute a direction and two of the `make test` total. A drive-side fast transfer (our own code uploaded to the real 1541 with `M-W`/`M-E`, which a real drive does run) is the next step if it is too slow. `tfirst` / `tlast` (bytes in the PRG, 1 and 35) set the tracks copied; the VICE test patches them.

**Tested.** `tests/test_vdcopy.py` (`make test-vdcopy`): the program in py65 (`tests/c64_sim.py`) against `vdrive.c` on both units (8 plays the real drive): image to "floppy" with format (byte-identical, the map), floppy to a new image and the `63` refusal, writing over the mounted image (and `N` to cancel), an image with two bad sectors (counted, shown, data copied), write protect (`W-1`: stops at 1/0), nothing mounted (`74`), a D71 refused, cancel, file mode, the theme from `!STATE` and the colours restored on `Q`. `tools/vdrive_mock.py scan`: tracks 17-19 (59 sectors: track 18, and a speed-zone change on each side) of the SD's mounted image onto the true-drive 1541 at 8 after a format (name and ID checked), then the floppy into a new image on the USB unit, byte-identical; this is what proves the real 1541's side (`U1` and the 256 bytes it sends from a buffer, `B-P` + `U2`, `N:` with the source's ID).
## 41. The firmware patches the C64's own KERNAL; `SYS64738` returns to the TR menu (0.9.0.43; host tests and VICE, works on a real C64)

**Why.** Until now the VDrive Special IOs served the `<Shift-K>` file, which the user had to build on a PC (`make build/kernal-tr.bin`) from a ROM dump they had to find. Agreed with the TR maintainer: the VDrive Special IOs should not depend on that setting at all, so the plain Kernal Replace Special IO can keep any other image there, and a user who owns a C64 already owns the ROM.

**Where the image comes from** (`VD_KernalImage` in `firmware/IOH_VDrive.c`, called in `KERNALPreStart()` through a new hook in `IOH_KernalReplace.c`, `KernalImageSource`, which `KERNALPreStart()` sets for the VDrive Special IOs and clears for Kernal Replace; the `<Shift-K>` file branch is unchanged, only braced):
1. `/VDrive/kernal.bin` on the SD card, 8192 bytes, served as it is (a `kernal-tr.bin` built on a PC from any base ROM: the way out for a ROM the firmware does not know). Wrong size: ignored.
2. Otherwise the C64's own KERNAL, read by DMA (`PerformDMA`, 8 pieces of 1 KB, about 8 ms) while the C64 waits in the menu: `fBusSnoop` is NULL there (`SetUpMainMenuROM()`), HIRAM is set and the menu cartridge does not map `$E000`, so the read sees the chip. Its CRC-32 must be on `VD_KnownKernals`; then `VD_KernalPatchApply` lays `VD_KernalPatch` over it and that is served.
3. Anything else (a read that fails, a ROM not on the list: JiffyDOS, other languages, a 4064, an already patched image) serves nothing: the C64 runs its own KERNAL, the drives stay silent, the serial console says why and names `/VDrive/kernal.bin`.

**The table.** `tools/build.py --table` writes `vd_kpatch.h`: the `P_XXXX` segments as runs (address hi/lo, length hi/lo, bytes; zero length ends), 1038 bytes of our own code and strings, no Commodore bytes. `tools/make_tr_patch.sh` puts it next to the library in the TeensyROM patch. The firmware checks that every run stays inside `$E000-$FFFF` before writing any. The image is the same as `tools/build.py`'s for the same ROM (`tests/fw_host` compares them byte for byte with the 901227-03 ROM).

**Which ROMs.** The patch calls into the stock routines, so a ROM is only patched when it is known to match where it matters; the build's asserts (`$EA31`, `$E1D9`, INITMS) hold for all five. Compared with 901227-03 the others differ only outside every patched range (checked byte by byte; that the stock routines our code calls behave the same is what the VICE runs below check): 901227-02 (screen clear and colour RAM, `$E4AC-$E599`, `$EA07`, RS-232), 251104-04 SX-64 (banner, colours, single bytes in OPEN / LOAD / SAVE away from the hooks), and the C128's C64-mode KERNALs 325179-01 and 325182-01 (keyboard scan, `$E4B7-$E4D2`, `$FEC2`). 901227-01, the 4064, the Japanese and Scandinavian ROMs are not on the list (not checked; `/VDrive/kernal.bin` covers them). Each listed ROM runs the mock's `variant` scenario in VICE (`make test-variants`: banner, directory, SAVE / LOAD, the reset hook), which also checks that the list in the firmware and the files tested agree.

**`SYS64738` back to the menu.** `P_FCE2` turns the cold start's `LDX #$FF / SEI` into `JMP rst_hook`. The hook runs on every reset, also the one the TR does when it launches a program, while the menu's own IO1 handler still answers (section 16), and on machines without VDrive. So it writes nothing until two new read-only mailbox registers, `ID0` / `ID1` at `$DE19` / `$DE1A`, read `$D6 $64` (the first read twice, as everywhere): under the menu handler those addresses are a SID's init address (0 without a SID; no tune starts in I/O space), unanswered they float to `$DE`. Then `VIO_MENU` through `vcall` (section 37: the Teensy answers, writes back what is unsaved and resets into the menu) and `BCC *` until that reset. No answer within vcall's ~1 s, or "not mine": the stock cold start from `$FCE5`. The hook (33 bytes) lives in the cassette IRQ set-up `$F875-$F8CF`, reached only by a tape transfer with PLAY pressed, which already ran into the reclaimed tape code (section 9); `$F8D0`, the STOP check of "PRESS PLAY ON TAPE", stays. So any program that ends with `SYS64738` or `JMP ($FFFC)` lands in the menu instead of a fresh BASIC: start BASIC from the menu for that. No setting to turn it off; one can be added if a program needs the old behaviour.

**Tests.** `tests/test_reset_hook.py` (py65, `make test-reset`): the request with the IDs, no write with the menu's SID values, an empty bus, a disagreeing or wrong read, no answer and "not mine" (stock cold start, stack set). `tests/test_vio.py`: the ID registers, read-only. `tests/fw_host`: the IDs through the bus handler, the CRC-32, `/VDrive/kernal.bin` first (wrong size ignored), the DMA read patched == `kernal-tr.bin` (`make test-fw` passes `STOCK`), an unknown, a one-byte-changed and an already patched ROM refused, a failed read. VICE: `basic` ends with `SYS64738` (one `VIO_MENU`, no banner) and a reset without the IDs (stock start, the drive still answers); `variant` on each listed ROM. On a real C64 (owner, 2026-11-02): the DMA read of the real chip, the launch reset under the menu handler and `SYS64738` work; `SYS64738` on a C128 not checked.

## 42. SHIFT+RUN/STOP: `LOAD"*",8,1` and `RUN` (0.9.0.44; VICE on the five ROMs, works on a real C64)

**Why.** Stock SHIFT+RUN/STOP puts `LOAD` CR `RUN` CR in the keyboard buffer: a tape load, and the tape code is gone (section 9). The SX-64 KERNAL types `LOAD":*",8` CR `RUN` CR. We want `,1` too, so a machine-code program lands at its own address (a BASIC program at `$0801` loads there either way).

**The buffer.** `LOAD"*",8,1` CR `RUN` CR is 16 keys; the buffer `$0277-$0280` holds 10 (`XMAX`). The SX-64 just writes its 15 (`LDX #$0F` at `$E5EE`, table at `$F0D8`), running over `$0281-$0285` (`MEMSTR`, `MEMSIZ`, `TIMOUT`: not used while BASIC runs); a 16th key would land on `$0286`, the text colour. Abbreviating (`lO`, `rU`) would fit but shows graphics characters on the screen. So the editor is told the first keys directly: the key loop at `$E5CD` gets a key from the buffer and, for any key that is not RETURN, echoes it through `JSR $E716` (at `$E5CA`) before waiting for the next; `shift_run` does the same for `LOAD"*` (6 keys; `$E716` keeps X and Y, quote mode toggles as when typed) and puts `",8,1` CR `RUN` CR (9 keys) in the buffer, then goes back to `$E5CD`. At the CR the editor reads the line from the screen as usual; `RUN` CR stays in the buffer through the load and runs after `READY.`, as stock.

**Where.** `P_E5EE` replaces `LDX #$09 / SEI` (SX-64: `LDX #$0F`) of the stock buffer fill with `JMP shift_run`; the rest of the stock fill and the `LOAD`/`RUN` table at `$ECE7` are left unreached. `shift_run` and its 16-byte text (46 bytes) sit after `rst_hook` in the cassette IRQ set-up `$F875-$F8CF` (79 of 91 bytes used; the assert names both). `tools/build.py` refuses a ROM without `LDX #n / SEI / STX $C6` at `$E5EE`; all five ROMs on `VD_KnownKernals` have it (only the count differs on the SX-64).

**Tests.** The mock's `variant` (each of the five ROMs) and `basic` scenarios put `$83` in the keyboard buffer on a cleared screen and check for `LOAD"*",8,1`, `SEARCHING FOR *`, `RUN` and `HI` (the first file on the mock disk is `10 PRINT"HI"`), and that `$0286` is unchanged. Not on hardware.

## 43. Drive activity LED (0.9.0.46; firmware glue host test, works on a real C64)

**Credit.** The code and the idea are ziggystar12's (the creator of Mean Hamster Engine), from his own VDrive build; this is his routine under our names.

**What.** The TR's LED (pin 34, the one `SetLEDOn` / `SetLEDOff` drive) blinks while the drive is busy. The first access of a burst reads the LED's current state (on while a program runs; a pause or some cartridges turn it off) and remembers it; from then on the LED is the opposite of that state for 75 ms, that state for 75 ms, and so on, until 180 ms pass with no access, when it gets the remembered state back.

**Where.** `PollingHndlr_VDrive` (main loop, so every combined VDrive Special IO gets it): `VD_LedPoll()` on every pass, idle or not, and `VD_LedAccess()` after each mailbox command `vio_poll()` serviced, unless its answer is `VIO_NOTMINE` (a command for another device, which the KERNAL then sends to the real bus). One `vio_poll()` is one command (a LISTEN, a byte, a 128-byte fast-LOAD block), so a long single operation (`N:` of a D81, about 0.8 s) holds one LED state through it. Nothing in the bus ISR: the hot path is unchanged. When the handler ends (back to the menu) the menu set-up turns the LED on as before.

**Tests.** `tests/fw_host` (pin 34 stubbed): a command for device 20 (not ours) leaves the LED alone; one for device 8 flips it at once, then the trace is on at 75 ms, off at 150 ms, on (the old state) at 180 ms; starting from off it does the same the other way round. Works on a real C64 (owner, 2026-11-03).

## 44. Aliases for long host names (0.9.0.47; host and glue tests, works on a real C64)

The host listing used to skip any card file or folder whose name had more than 16 characters, so a folder of `The-Pumpkins-Breadbin-of-Horror-side1.D64` images looked empty. Such names now appear under a 16-character alias, and every place that takes a name takes the alias.

- **The alias** (`vd_alias()` in `vdrive.c`, portable and unit-tested): a name is used as it is when it has at most 16 characters, all of them ones a C64 can type and the DOS does not parse (`$20-$7A` without `` ` `` and `* ? , = : "`). Otherwise each such character becomes `^` (the up arrow: the unshifted `↑` key, `$5E` on both sides; `~` is not usable, PETSCII has no tilde and `$7E` shows as π, which the keyboard sends as `$DE`), and a name over 16 characters keeps its start and its last word, joined by `^`: the extension (`.` and 1-3 characters) stays, the last word after ` `, `-`, `_` or `.` is kept when it has 1-5 characters (else the last 3 characters), and the start fills the rest. `The-Pumpkins-Breadbin-of-Horror-side1.D64` → `THE-PU^SIDE1.D64`, `Long Folder With A Long Name` → `LONG FOLDER^NAME`, `a,b.prg` → `A^B.PRG`. Multi-disk sets usually differ at the end of the name, which is why the end is kept. An alias depends only on its own name, never on the other files in the folder or their order, so an alias written into a BASIC program stays valid when files are added.
- **Collisions are not numbered.** If another entry of the folder shows the same name (a second long name with the same alias, or a real name equal to it), the alias is dropped: not listed, not found. A real name always wins. The fix is renaming one of the files on a computer. (Numbering, `~1`/`~2` or hash digits, was rejected: numbers depend on the other files and would change under a program that uses them.)
- **Listing** (`VD_DirOpen` / `VD_DirNext` in the glue): `VD_DirOpen` makes a first pass over the folder and marks the hash (FNV-1a, 10 bits) of every shown name in a seen and a duplicate bitmap (`vdSeen` / `vdDup`, 128 bytes each per unit, RAM2). An alias whose bit is in the duplicate map is checked exactly (`VD_CountShown`, one more folder scan) and dropped if a twin exists, so the extra cost is one folder read per listing plus a scan per likely twin. Names of `VD_CwdLen` (96) characters or more are still hidden: `imgName` must hold any image name it mounts.
- **Resolution** (`VD_Resolve`): mount, `CD:`, plain-file reads, `MD:`, `RD:`, `N:` and `S:` (image files) turn a C64 name into a host path. The exact name is tried first; only when it does not exist and the name holds a `^` does the glue scan the folder for the one entry whose alias it is. Every alias holds a `^`, so names without one never cost a scan. Paths need nothing extra: `path_walk()` calls `chdir` / `mount` once per component, so `LOAD"$/LONG FOLDER^NAME/THE-PU^SIDE2.D64",8` works. `MD:` / `N:` on a name that is an alias of an existing entry answer `63`. Patterns (`LOAD"THE*"`) go through the listing callbacks and so match aliases.
- **The host spelling is kept.** `imgName` (and `snapImg`) now hold the host name of the mounted image instead of its PETSCII spelling, and new helpers (`VD_HostPath`, `VD_MountHost`) mount and rebuild paths from it. So the `/.vdrive` sidecar, `!STATE`, `trvdrive` get, the push/pop of a listing by path and the alt-button list all see the real path. A side effect: remote get and the sidecar now show the case of the path as it was given (a mount from the menu or `trvdrive` set keeps `/GAME.D64`; it used to come back as `/game.d64`).

Tested: `tests/test_vdrive.py` (`Alias`: the examples, the characters, a random sweep for length and the `^`) and `tests/fw_host` (a stub folder with a long folder name, the two Breadbin sides, a comma name, two names with the same alias, an alias equal to a real name: listing, `CD:` by alias, mount, sidecar, a listing by alias path that restores the selection, `LOAD` through an alias path, a pattern, `MD:` on an alias, `S:` by alias). Works on a real C64 (owner, 2026-11-05). Not measured there: the listing time of a large folder on a USB stick, now that it reads the folder twice.

## 45. A VDrive KERNAL in the machine itself: the mailbox gate (0.9.0.48; py65, fw_host and VICE; works on a MEGA65)

Asked for a MEGA65 (Boris Schneider-Johne), whose C64 core can replace the KERNAL as a file the way it does for JiffyDOS; an EPROM in the KERNAL socket of a real C64 is the same case. Until now the patched KERNAL only ran while a VDrive Special IO served it, so its mailbox calls always met a live mailbox (only the cold start, `rst_hook`, ran under other handlers, and it checks the ID registers first). A KERNAL that is always there also runs with no TR, with another Special IO and under the TR menu's own handler, which the menu's launch-time bus probe (section 37) drives through the C64's own KERNAL. There every hook used to write the mailbox and wait out vcall's ~1 s time-out per call (a real 1541 unusable), a floating `$DE13` could read like an answer token, and under the menu's handler `$DE11` (ARG) and `$DE15` (LEN) are EEPROM-backed settings (`rwRegMIDISettings`, `rwRegTimezone`) that a write stores.

- **KERNAL** (`patches/vdrive.s`): `vd_here` reads ID0 (`$DE19`, `$D6` while a VDrive handler answers) up to 4 times; `vcall` asks it before it writes anything and gives "not mine" at once otherwise, so the call goes to the real bus. 4 tries, not 1: with the handler there, a late (bit-mixed) answer would otherwise hand a transaction to the real bus half-way. With no handler the 4 misses cost about 50 cycles. Only ID0 is read (the tape space is nearly full; under the menu `$DE19` is a SID's init address high byte, never `$D6`; a floating bus that happens to read `$D6` only gets the old time-out). The hooks no longer write `MBX_ARG` themselves: they leave the argument in `$A7` and `vcall` writes it after the check (which also saved 22 bytes); `open_named` asks `vd_here` before it fills the window and LEN. The bus scan's resend keeps the argument in `$A7` only. Tape space `$F8E2-$FB8D`: 667 of 683 bytes.
- **Firmware** (`VD_KernalImage`): an unknown CRC used to mean "drives off". Now, if the DMA image holds every run of `VD_KernalPatch` byte for byte (`VD_KernalPatchPresent`), the C64's own KERNAL is this build's VDrive KERNAL: nothing is served (no Kernal Replace over the machine's own replacement), the mailbox answers it and the menu's bus probe runs as usual (safe through the gate). A KERNAL with the banner tag `VDUNITS:` but other bytes is another revision's VDrive KERNAL: nothing served, and no menu probe (an older one has no gate), so the KERNAL's own first-access scan sets the numbers. Unknown ROMs, failed DMA reads (a host that cannot DMA) and other revisions all skip the menu probe now (`vdNoMenuProbe`, consumed by the next `VDrive_PreStart`); a served image probes as before.
- Not changed: the banner of a resident KERNAL shows the stock second line (the firmware cannot write `VDUNITS:` into a ROM); tape stays broken; `SYS64738` returns to the TR menu only while a VDrive Special IO runs.

Tested: `tests/test_vcall_gate.py` (py65, `make test-gate`: LISTEN with the handler, with "not mine", with no handler and under the menu's registers, `STAT` reading like each token: no IO write at all and the real bus within 300 cycles; 1-3 late ID reads still reach the mailbox, 4 do not; the fast-LOAD open writes neither window nor LEN without the handler), `tests/test_reset_hook.py` (now sees the ARG write before VIO_MENU), `tests/fw_host` (this build's KERNAL resident: nothing served, probe asked; another build's or an unknown ROM: no probe), VICE `basic` (with the ID cleared, `LOAD"X",8` gives `DEVICE NOT PRESENT` with no mailbox command and CMD/ARG/LEN untouched; the reset checks now restore the ID afterwards). Works on a MEGA65 (Boris Schneider-Johne, reported 2026-11-08). Not run: an EPROM in a real C64.

## 46. The banner's drive numbers for a VDrive KERNAL in the machine itself (0.9.0.49; py65 and host tests, not on hardware)

With a resident VDrive KERNAL (section 45; Boris Schneider-Johne's MEGA65) the banner showed the stock second line: its unit bytes `VDUNITS:` are `0 0` as built and the firmware cannot write into that KERNAL (section 38).

- **When the banner is printed.** Every launch from the TR menu, F2 (a small PRG from the Teensy menu) included, goes through `XferCopyRun` in `MainMenu.asm`: `rCtlKERNALPreStartWAIT` (PreStart, then our probe, section 37), and then the menu calls the KERNAL's init routines itself (IOINIT, a RAMTAS without the RAM test, RESTOR, CINT, BASIC's vector and RAM set-up) and INITMS at `$E422`, the banner, in place: no reset. The PRG is streamed afterwards and only the `rCtlRunningPRG` handshake swaps in the VDrive handler. So the banner runs under the menu's own IO1 handler, which answers a plain read with `IO1[n]`.
- **The registers.** Menu IO1 offsets 60-63 (`$DE3C-$DE3F`) lie inside `IO1Size` (104) between `rwRegPwrUpDefaults3` (52) and `StartSIDRegs` (64), and nothing reads or writes them (upstream appends new registers after 52, so the top of the gap is the safest). `VD_BannerRegs` writes there what `VD_BannerUnits` writes into a served image: tag `$D6 $64`, then SD and USB (`$FE` absent, the number, `$FF` automatic not settled). `VDrive_PreStart` writes them, `VD_ProbeFinish` again with the probed numbers; `VDrive_ProbeStep`, first in every `KERNALPreStart` whatever the Special IO, clears them on an ordinary PreStart, so a launch with another Special IO never shows a stale VDRIVE field.
- **The KERNAL** (`vd_head` in `patches/vdbanner.s`): the two bytes now live in zero page `RES` (`$FC/$FD`, the per-unit results, which are filled from them anyway): the image's, or, when those are `0 0` and `$DE3C/$DE3D` read `$D6 $64`, the registers'; else the stock line. Everything after is as before (a fixed or settled number printed, `$FF` probed on the real bus, `$FE` `NO..`). With no TR or another cartridge those addresses float or hold something else, and a wrong tag gives the stock line. Space: the stock line now prints ` ` and the ROM's own `64K RAM SYSTEM  ` at `$E49B`, which the cut at `$E49A` left in place (17 bytes of copy gone; the five whitelisted ROMs have it there, and `tools/build.py` now checks it, along with INITMS, whose check had looked for a segment `E444` the banner never had and so never ran). Banner area 312 of 314 bytes.
- Considered first: the menu stamping the numbers into RAM that survives the start-up (`$07E8`, past the screen CINT clears). Not needed: the registers carry them with no menu change. (A MEGA65 clears RAM on a reset, Boris found; a launch is no reset, but the registers do not depend on that.)

Tested: `tests/test_banner.py` (py65: the registers with automatic, settled, absent, cleared and wrong-tag values; the image's bytes win over them), `tests/fw_host` (the registers after the probe, cleared by an ordinary PreStart step, `$FF` before the probe ends, the probed numbers after). The VICE runs serve the image with the bytes written, so they cover the image path on the real ROMs. Not on hardware: a MEGA65 or EPROM banner after a launch (and the served banner on a C64, unchanged in what it prints).

## 47. VDrive Config: folder listings in blocks (0.9.0.50; py65, not on hardware)

Boris found VDrive Config's browser very slow on a card with many files, while CBM FileBrowser (which loads listings with `LOAD`) was fast. The browser read each `$/path/` listing with `OPEN` and `CHRIN`: one mailbox round trip per byte, every byte of every entry, also the many it then skips (it keeps folders and `.D64`/`.D71`/`.D81`, at most 100). The KERNAL's fast `LOAD` (section 5) moves 128 bytes per round trip through the IO2 window.

- **`fopen`** copies the name from `CMD` into the window (read back), sets LEN and ARG (the unit's device), sends `FLOAD_OPEN` (9) through `mcall` (STAT polled for `$5A`; `$A5`, `$3C` or about a second without an answer = not answered; ST and DATA read twice until two reads agree, as the KERNAL does). DATA 0: open, the window holds the two load address bytes. Anything else: the drive refused, and `rdel` reads why from the unit's command channel (`62` for a folder that is gone), shown as before as "Cannot list: ...".
- **`fget`** hands out the window's bytes (each read twice) and sends `FLOAD_BLK` (10) when they are used up; STATUS gets `$40` with the last byte of the block that came with EOI and `$42` after it, as `CHRIN` from a serial file leaves it, so the listing parser is unchanged except that it calls `rdb` (`fget` after a fast open, else `CHRIN`). The firmware closes the channel itself at EOI (`fload_blk`); the parser reads to the end anyway.
- **Fallback:** a name of 128 bytes or more, or a fast open that is not answered, takes the old `OPEN 1,dv,0` + `CHRIN` path. The firmware side is the one `LOAD"$/path/"` has used since section 18 (VDSUITE, VICE and hardware), so only the program changed: the PRG is now 6052 bytes and ends at `$1FA3`, under its BSS at `$2000`.

Tested: `tests/c64_sim.py` answers `FLOAD_OPEN` / `FLOAD_BLK` from the drive model like `fload_open` / `fload_blk` (`fast=False` answers "not mine"); `tests/test_vdconfig.py`: the browser uses blocks and leaves no channel open, the fallback gives the same screens, a folder of 400 PRGs and 150 images in about one block per 128 bytes with the same screens either way, a folder gone between the listing and RETURN shows "Cannot list: file not found" on both paths. Not on hardware: the speed itself (expect roughly the speed of `LOAD"$/path/"`).

## 48. DMATEST: can a DMA LOAD write C64 RAM while the KERNAL is served? (test program; py65, passes on a C128 in C64 mode)

**Question.** A LOAD could skip the 128-byte window and the 6502 copy loop (about 24 cycles a byte, ~0.75 s for 30 KB): the Teensy
writes the file into RAM by DMA (`PerformDMA(DMA_WRITE, ...)`, one byte a cycle, ~30 ms) while the 6502 waits on STAT, as c64cast's
`WriteC64Mem` already does. The worry was Kernal Replace's `/GAME`: in Ultimax mode writes to `$1000-$7FFF` are lost and writes to
`$E000+` go to the cartridge. The source says it cannot happen: the phi2 ISR (`ISRs.c`) runs `DMATransferISR()` and returns before
`fBusSnoop` while `DMA_State == DMA_S_TransferExecuting`, `KernalCheck` asserts `/GAME` only for CPU *reads* of `$E000-$FFFF` and
releases it in the same cycle, and `DMAControl.c` skips cycles with BA low (badlines, sprites). DMATEST checks it on the board.

**Program** (`tests/asm/dmatest.s`, `DMATEST` on the test disk, no mailbox use). It waits in a `GETIN` loop (so DMA starts while the CPU
runs the served KERNAL and its IRQ code); the host (`tools/dmatest.py`, `trlib.py` from teensyrom-rcp) writes a pattern
(`((lo EOR seed) + hi * $3B) AND $FF`) into `$2000-$3FFF`, `$A000-$BFFF`, `$C000-$CFFF`, `$E000-$FEFF` (28.4 KB a round, in 4 KB
`WriteC64Mem` commands), then seed and round at `$03F0/$03F1`. The program checks every byte (under ROM with `$01 = $34`, IRQs off one
page at a time; the RAM NMI vector points at an `RTI`), three guard pages it filled itself (`$1F00`, `$4000`, `$9F00`: a write outside
its range, as on some NTSC C128s), and a sum of `$E000-$FFFF` as the CPU reads it (the served KERNAL unchanged), then acknowledges at
`$03F2`. Counts per area, the first wrong address with the byte read and expected, rounds dropped because the host wrote again during a
check (`OVERRUN`), and whether the VDrive ID (`$DE19/$DE1A`) answers. Counters at `$0340-$0388` (`--sync` reads them back).

**Use.** On the C64 `LOAD"DMATEST",8` and `RUN` under a VDrive Special IO (and, to compare, under plain Kernal Replace or none), then
`tools/dmatest.py -H <tr-ip> --rounds 50` (TCP rather than USB: a USB reply runs with interrupts off). Clean: all counts `0000`.
`--selftest` writes one wrong byte at `$E123` per round (`E000-FEFF` must count one per round); `--sync` paces by DMA-reading the
acknowledgement instead of a fixed `--delay` (2 s; a check takes about 1.2 s); `--chunk` sets the bytes per command. The script prints
the write time per round, a first figure for the transport-bound speed.

**Tests.** `tests/test_dmatest.py` (py65, `make test-dmatest`): `$01` banking of the BASIC and KERNAL ROMs modelled, clean rounds, a lost
write under the KERNAL (address, byte read and expected), a lost write in low RAM, a stray write into a guard, an overrun dropped, no ID,
a key quits.

**On a C128 in C64 mode (owner, 2026-11-08, new TR+, a VDrive Special IO, over TCP; every DMATEST run so far, none on a C64 yet):** 50 + 50 rounds clean, and 100 `--selftest` rounds with `--chunk 1024`
counting exactly one wrong byte per round (`0064`) and nothing else. So DMA writes reach RAM in `$2000-$3FFF` and under BASIC and the KERNAL
while our KERNAL is served, and the test catches a wrong byte on the board. One run stopped after 28 rounds on the Mac side:
`write $3000: Timeout! Error receiving data!`, the TR timing out while it still *received* a 4 KB `WriteC64Mem` payload, before any DMA.
Suspected cause: with a Kernal Replace / VDrive handler running, `kernal-fnet-poll.patch` masks the PIT and runs `fnet_poll()` only from
`loop()`, and a remote command's receive wait (`SerialAvailabeTimeout`) runs inside `loop()` without polling (`EthernetClient::available()`
only peeks the socket). A payload that fits one TCP segment (`--chunk 1024`) has not failed.

**Fix (0.9.0.51, in `kernal-fnet-poll.patch` and on the PR #85 branch, pushed 2026-11-08):** `CmdInputPoll()` in `SerUSBIO.ino` calls
`FnetLoopPollService()` in the three places a remote command waits for input (`SerialAvailabeTimeout`, `DiscardCmdInput`,
`ReceiveSpanBytes`). The stock firmware never had the problem: its PIT ISR polls during those waits. Firmware builds; fw_host passes;
**not on hardware**: check with a long 4 KB run (`tools/dmatest.py --rounds 300`, no `--chunk`) and no time-out.

**Later runs (owner, 2026-11-08, the same C128; host Mac -> WiFi -> Pi -> Ethernet -> TR):** 0.9.0.51
still timed out once after 27 rounds. `dmatest.py` then got `--nodelay` (`TCP_NODELAY`) and timed each command apart (send / reply,
`--slow`): with Nagle off the Mac hands a command over at once (`send 0 ms`) and the TR's reply took 0.5-3 s now and then, so the time is
spent between the Mac's socket and the TR's answer, not in the DMA. FNET acknowledges data only on its 200 ms timer (`fnet_tcp.c`), the
TR's receive ring has 4 buffers, and the TR gives up after 500 ms without a byte (`SerialTimoutMillis`); a lost frame costs a TCP
retransmission (~200 ms, then doubling), and two in a row exceed the 500 ms. 300 rounds from the Mac with `--nodelay` passed (slowest round
7.1 s); from the Pi on the wired side (`dmatest.py` run on the Pi) 300 rounds passed, 8.5 MB at 82 KB/s, slowest round 441 ms (typical ~340 ms). Conclusion: the time-outs were WiFi loss on the host's path, not
DMA, not the C128, and not shown to be the missing poll (0.9.0.51 is still right: without it a wait could not receive at all). For clients
over WiFi: retry a `WriteC64Mem` that timed out (the same bytes to the same place again do no harm; a trlib change in teensyrom-rcp).

## 49. DMA LOAD (0.9.0.52; host, glue and VICE tests; first hardware timing on a C128)

**Why.** The window LOAD (section 9, 32) moves 128 bytes per mailbox round trip and copies each byte with two reads (bus margin), about
24 cycles a byte. DMATEST (48) showed the TR+ can write C64 RAM by DMA while our KERNAL is served, under BASIC and the KERNAL too, so a
LOAD can skip the window: the Teensy reads the next part of the file and writes it into RAM itself while the 6502 waits on STAT.

**Protocol.** One new command, `VIO_FLOAD_DMA` (17), sent by the KERNAL's LOAD loop in place of `VIO_FLOAD_BLK` with ARG/LEN = the address
the next byte goes to (`$AE/$AF`). The Teensy answers either:
- as `VIO_FLOAD_BLK` (DATA = 1-128 bytes in the window, which the KERNAL copies as before), or
- with DATA = 0: it read up to `dma_max` bytes (`VD_DmaPart`, 2 KB) and wrote them by DMA, then wrote the new end address into `$AE/$AF`
  (`VIO_DMA_PTR`), also by DMA. The KERNAL's loop already treats DATA = 0 as "nothing to copy" and goes on from `$AE/$AF`, so the 6502
  side costs 9 bytes (`lda $AE / sta $A7 / lda $AF / sta MBX_LEN` and the command number; `P_F8E2` 676 of 683 bytes).
ST is as for `VIO_FLOAD_BLK` (`$40` last part, `$42` a read error ended it). A failed DMA write (`PerformDMA` / `CloseDMA` false) closes the
channel and answers NOTMINE: the KERNAL reports DEVICE NOT PRESENT instead of loading stale bytes, as for a failed window block.

**Where DMA is used** (`vio_dma_ok`): only where a CPU store would reach the same RAM. Not `$0000-$01FF` (the CPU port at `$00/$01` is not RAM
to a DMA write; zero page and the stack the KERNAL is running on change under it) and not `$D000-$DFFF` (I/O or the character ROM depending
on `$01`; our own `$DE/$DF` registers). There the window path stores the bytes with the CPU, exactly as before. A DMA part stops at `$D000`
and at `$FFFF`; the next request then gets the window. A load that wraps past `$FFFF` goes on at `$0000` with the window, as the stock loop.
Under BASIC and the KERNAL a DMA write lands in RAM, as the CPU's store does.

**Firmware.** `vio_t` gets `dma_write`, `dma_ctx`, `dma_buf`, `dma_max` (`vio_set_dma`; `vio_init` clears them, so no host = no DMA = the
window path: host tests, the scan and variant VICE runs). The glue (`IOH_VDrive.c`) sets `VD_DmaWrite` (`PerformDMA(DMA_WRITE, ...)` and
`CloseDMA()`) and a 2 KB `DMAMEM` buffer (`vdDmaBuf`, RAM2 outside the heap the REU sizes itself from). The read of a part is the window
path's byte loop (`fload_read`, shared with `fload_blk`).

**Build flag.** The DMA is compiled in only with `Fab04_VDriveDMALoad` (`Common_Defs.h`, next to `Fab04_VDrive`; needs
`Fab04_FullDMACapable`), which `tools/make_tr_patch.sh` defines for our builds. Without it the glue sets no DMA hook and the drive answers
every `VIO_FLOAD_DMA` with a window block, so the same KERNAL works with either firmware. The flag stays out of the upstream TeensyROM PRs
(Travis, 2026-11-08: no DMA in the first VDrive firmware upstream). The KERNAL's request is a build option as well, `VD_DMA_LOAD`
(`patches/vdrive.s`, default 1; 0 = the plain `VIO_FLOAD_BLK` loop, byte-identical to the 0.9.0.51 `kernal-tr.bin`). Upstream's KERNAL is the
firmware's own patch table (40), so it leaves the request out too (owner, 2026-11-08: not until DMA is wanted there).
`VD_DMA_LOAD=0 tools/make_tr_patch.sh <tree>` does both: no `Fab04_VDriveDMALoad`, the table built with `VD_DMA_LOAD=0`, written to
`build/teensyrom-vdrive-nodma.patch` (`firmware/` left alone); it differs from ours only in the define and the table. The engine keeps
command 17 (portable code, unused there). Tests: `tests/fw_host/run.py` builds and runs the glue both ways (`VD_TEST_NO_DMA`); `make test`
builds `kernal-tr-nodma.bin` / `kernal-vd-nodma.bin`, and the VICE `scan` run uses the latter.

**Costs and limits.**
- The CPU is halted for each part, about 2.5 ms per 2 KB (badlines skipped): IRQs and NMIs wait that long. A SwiftLink receiving at high
  speed during a LOAD may drop characters; nothing else runs during a KERNAL LOAD anyway. `VD_DmaPart` is the knob.
- VERIFY and SAVE were unchanged in 0.9.0.52; section 51 (0.9.0.57) does them by DMA reads.
- KERNAL and firmware must match: an older firmware answers command 17 NOTMINE, so every LOAD fails with DEVICE NOT PRESENT. The firmware
  patches the C64's own KERNAL from its own table (40), so only a `/VDrive/kernal.bin` or a VDrive KERNAL in a ROM from another revision can
  meet an older firmware. VDrive Config's block listing (47) still uses `VIO_FLOAD_BLK`.

**Tests.** `tests/test_vio.py`: every file size loads the same bytes as the window LOAD, DMA never below `$0200` or in `$D000-$DFFF` and never
more than 2 KB, loads at `$0100`, `$8000`, `$CF80`, `$FFF0` (window, DMA, wrap), a failed DMA answers not mine and closes the channel, no DMA
host = the window. `tests/fw_host`: the glue's `PerformDMA` writes into a fake 64 KB, a 5000-byte file at `$0801` in three parts, across I/O
from `$CE00`, from `$0100`, a failed DMA. VICE `basic` (`tools/vdrive_mock.py`): DMA on, each DMA write done as a monitor write while the CPU is
stopped at the CMD checkpoint; the whole run (BASIC test programs, VDSUITE, the soak, the fault injection) passes and BIG (6000 bytes)
comes in three DMA parts. `scan` and `variant` keep the window path with the new KERNAL.

**On hardware (owner, 2026-11-08, C128 in C64 mode):** a 189-block program (about 48 KB), `TI$="000000":LOAD"FILE",8,1` then `?TI`:
0.9.0.51 85 jiffies every time (~1.42 s, ~34 KB/s), 0.9.0.52 13-14 jiffies (~0.23 s). The count includes the messages (`SEARCHING FOR`, `LOADING`, `READY.`),
any screen scroll (~20-25 ms each) and the file open, perhaps 1-3 jiffies, plus +-1 jiffy of rounding: DMA LOAD is about 6-8x faster, ~200-250 KB/s.
The LOAD loop runs with interrupts on and a DMA part halts the CPU for ~2 ms, so the jiffy clock counts true. A 1-block file timed the same way gives the fixed part.

**On hardware, still to check:** a load under BASIC (`LOAD"X",8,1` to `$A000`) and one across
`$D000`, `VDSUITE` "run all", the C64 as well as the C128, and VDrive+SwiftLink with CCGMS (the NMI hold).

## 50. The REU's full size back: no VDrive data in RAM2 (0.9.0.53)

REU-Checker showed 256 KB where a 512 KB REU was expected. `REUTEST` (`tests/asm/reutest.s`, on the test disk) measures the REU itself:
the status register's chip bit, a tag at the start of each 64 KB bank written from bank 7 down and read back from bank 0 up (an REU
of N banks wraps, so bank N reads bank 0's tag), then a different pattern in every 8 KB chunk of the size found, fetched back and
compared byte by byte. VICE with 128, 256 and 512 KB REUs and with none gives the right answers. On hardware (owner, 2026-11-09, 0.9.0.51):

- VDrive+Swift+REU: 128 KB, chip bit clear, every byte right. REU-Checker from the TR menu had not run under it: a menu program
  paired with the REU gets VDrive+REU whenever the default is a VDrive Special IO (`VDriveSwift_Remap`, section 34).
- VDrive+REU **and the plain "512KB REU"**: 256 KB. The serial console: `Resizing from 63 banks to 32 banks`. Stock 0.9 and a recent
  upstream build: 512 KB.

The cause was ours. `AllocRAMBanks` takes the 128 KB RAM1 image (16 banks) and then 8 KB `malloc`s from the RAM2 heap, and rounds
down to a power of two. On the Teensy 4.1 that heap is what RAM2 has left after its static `DMAMEM` data, and the drive kept about
9.4 KB there in every build, whichever Special IO runs: `vioStore` (`vio_t`, 4568 bytes), `vdDmaBuf` (2 KB, DMA LOAD), `vdCfgBuf` (2 KB)
and `vdCfg` (256), `vdSeen`/`vdDup` (512). Stock leaves room for exactly the 48 banks the REU needs; ours left 47. Section 32's and
34's "takes nothing from the malloc heap" was wrong for the same reason.

All five are plain `static` now (DTCM; the TR's DMA is the CPU driving the bus, so `vdDmaBuf` needs no DMA-capable RAM). RAM1 has room
(66 KB free for locals before, about 9.4 KB less now). Rule from here: **no `DMAMEM` statics in VDrive code.** RAM2 needs go through
`malloc` at the time they are needed (as the bad-sector table does), or into DTCM.

**0.9.0.53 on hardware (owner, 2026-11-09):** the plain "512KB REU" 512 KB, VDrive+Swift+REU 128 KB, the plain VDrive KERNAL no REU
(as meant), but **VDrive+REU still 256 KB**, now `Resizing from 62 banks`: two banks short, not one. The VDrive launch path has heap
users the plain REU does not. One is upstream's Kernal Replace start-up: `InitHndlr_KERNALReplace_PreStart()` `malloc`s an 8 KB
`KernalBin` (`MIDIRxBuf`) and keeps it for the run. Our `kernalreplace-fixes.patch` serves the KERNAL from the DTCM copy
`KernalFast`, and VDrive never reads `KernalBin` again, so **0.9.0.54** frees it right after the copy when the image came from
`KernalImageSource` (the VDrive handlers; `make_tr_patch.sh`). The plain Kernal Replace keeps it: its `InitHndlr` tests it for "loaded".
The other ~8 KB is not found yet (open `File`s for the mounted image and the probe are only ~100 bytes each). 0.9.0.54 prints
`VDrive: RAM2 free before the REU: <bytes>` from the VDrive+REU and VDrive+Swift+REU inits (`RAM2BytesFree()`, SerUSBIO.ino): 48 banks
need 48 x 8200 = 393600 bytes or so (8192 plus `malloc`'s header each), and that line says how far short a launch is.

**0.9.0.54 on hardware (owner, 2026-11-09):** after F2 (BASIC) and right after boot, VDrive+REU gets 512 KB (`RAM2 free before the
REU: 393216` / `405504`). Launching `REUTEST.PRG` from the SD card in the TR menu: `327680`, `Resizing from 63 banks`, 256 KB.
`RAM2BytesFree()` is `_heap_end - __brkval`, the room above the heap top, blind to freed holes below it. The 64 KB difference is the
menu's directory listing (`DriveDirMenu`: `MaxMenuItems` 4000 x 16 bytes, plus a `malloc` per name), which stays allocated while a
program runs. `AllocRAMBanks()` frees it first, but by then VDrive's PreStart has opened the mounted image and the probe's files
(`new`'d `File` objects, from the heap) *above* it. So the 64 KB became a hole under live allocations: 7 banks fit in it (7 x 8200 =
57400, the rest wasted), the remaining banks come from the top, and the total lands one bank short. F2 and the boot start have no
listing (or no SD one), so no hole. **0.9.0.55** frees the listing in `KERNALPreStart()` (after it has read the selected item's
handler, before Kernal Replace's and VDrive's PreStart allocate) when the handler is VDrive+REU or VDrive+Swift+REU; the REU frees it
a moment later anyway. The drive's `File`s then land in the freed space at the bottom, and the top stays one block.
**0.9.0.55 on hardware (owner, 2026-11-09):** VDrive+REU gives 512 KB with `REUTEST.PRG` launched from the SD browser.

**The 0.9.0.54 free broke every menu launch with a VDrive handler (fixed in 0.9.0.56).** VDrive Config from the TR menu switched to
lower case and hung (owner, 0.9.0.55). Upstream's `InitHndlr_KernalReplace()`, which the VDrive inits call once the PRG transfer is done,
re-hooks `KernalCheck` only when `KernalBin != NULL` (its test for "a KERNAL was loaded", so a CRT that inherits Kernal Replace as its
Special IO is not hooked). With `KernalBin` freed after the copy it returned early: the program ran on the C64's own KERNAL, its screen
output worked and its disk calls went to the real bus. `REUTEST` never touches the drive, so the REU runs did not show it. 0.9.0.56 sets
`KernalFastOnly` when it frees the buffer (cleared at the start of every PreStart), and the re-hook takes `KernalBin != NULL ||
KernalFastOnly` as loaded (`make_tr_patch.sh`; the plain Kernal Replace is unchanged). Lesson: before freeing a buffer, grep for tests
of its pointer that mean something else. **0.9.0.56 on hardware (owner, 2026-11-09):** VDrive Config launches from the TR menu again.

## 51. DMA SAVE and VERIFY (0.9.0.57; host, glue and VICE tests; not on hardware)

**Why.** With DMA LOAD (49) a LOAD runs at ~200 KB/s, but SAVE still copies every byte into the 128-byte window (read back once,
~24 cycles a byte) and VERIFY still goes the stock byte-level way (one mailbox round trip per byte). The TR+ can read C64 RAM by DMA as
well, so the Teensy can take a SAVE's bytes straight out of RAM and compare a VERIFY's file with RAM itself.

**KERNAL space.** The reclaimed tape space had 7 bytes left (`P_F8E2`, 676 of 683). The two requests cost 9; 7 came back from the
error exits: `fl_hook`'s DEVICE NOT PRESENT branches to `fl_fail` (`JMP $F707`) instead of its own `JMP`, and `fs_hook` tests the open's
result with one `TAX` and shares its `@ff` (`JMP $F707`) for the open's DEVICE NOT PRESENT. Now 678 of 683. These changes are made only with
`VD_DMA_LOAD` (which now switches all three DMA requests), so `kernal-tr-nodma.bin` stays byte-identical to the 0.9.0.51 image.

**SAVE: `VIO_FSAVE_DMA` (18).** At the top of the SAVE loop (after the 2-byte header block) the KERNAL sends command 18 with no
arguments: `LDA #18 / JSR vcall / BEQ loop` (7 bytes). The Teensy reads the KERNAL's `$AC-$AF` (current pointer, end) by DMA, then up to
`dma_max` (2 KB) bytes from `$AC`, sets `$AC/$AD` to the new pointer by DMA and appends the bytes to the open file. It answers:
- DATA = 0: a part was saved; the KERNAL asks again at once.
- DATA = 1: nothing done (no DMA reads in this build, nothing left, the address is not one a DMA read may take, or a DMA read failed).
  The KERNAL goes on with its window block as before, then asks again; at the end its own `end - current = 0` test sends the close.
- NOTMINE (an older firmware that does not know 18): `vcall` returns `A = $A7`, the device number (never 0), so the window block is sent too.
  So a new KERNAL saves with any firmware, DMA or not.
- NOTMINE from this firmware only when the pointer write failed after the bytes were read: the Teensy cannot tell whether `$AC/$AD`
  moved, so it closes the file (what came before stays in it, as after a save cut short) and answers NOTMINE to this and to the next
  `VIO_FSAVE_BLK` / `VIO_FSAVE_CLOSE`: the window block that follows fails and the KERNAL reports DEVICE NOT PRESENT. (If the pointer did reach
  the end in spite of the failure, the KERNAL's close is answered NOTMINE and ignored: the SAVE looks done but the file is short. A
  pointer write that fails is not known to happen.)
The pointer is written before the bytes go to the file so that a failed write never leaves bytes the KERNAL will send again.

**VERIFY: `VIO_FVERIFY_OPEN` (19).** `fl_hook` used to send VERIFY (`$93` not 0) to the stock path at once. Now it opens with command 19
instead of `VIO_FLOAD_OPEN` (2 more bytes). The Teensy takes it only when it can compare the whole file by DMA:
- it needs the DMA read hook (else NOTMINE: the KERNAL's stock VERIFY, as before, so an older firmware or a build without DMA keeps
  verifying the old way);
- it reads the original secondary address (`$A4`, where `fl_hook` keeps it) and the caller's address (`$C3/$C4`) by DMA, opens the file,
  reads it through once for its length and opens it again; the start is the file's own address or, with secondary address 0, `$C3/$C4`;
- the whole range must lie in `$0200-$CFFF` (`vio_dma_rd_ok`), else NOTMINE and the stock VERIFY.
Then it answers as `VIO_FLOAD_OPEN` (not found / no device are reported as for a LOAD) and the KERNAL runs its LOAD loop unchanged: every
`VIO_FLOAD_DMA` is now a compare. The Teensy reads the next part of the file (up to 2 KB), reads the same range of RAM by DMA (256 bytes at a
time), ORs `$10` into ST on any difference (vcall ORs ST into `$90`, so BASIC prints `?VERIFY ERROR` as with the stock loop, which also goes
on to the end), writes the new end into `$AE/$AF` and answers DATA = 0: the KERNAL never gets a window block it would store. A failed DMA
mid-way answers NOTMINE and closes the channel (DEVICE NOT PRESENT rather than a result that was not checked). The file is read twice, which
costs Teensy time only (inside `vcall`'s ~1.6 s wait for any file a C64 can hold).

**Where DMA reads are used** (`vio_dma_rd_ok`, `$0200-$CFFF`): only where a DMA read sees what the KERNAL's own `LDA ($AC),Y` sees. Not
`$0000-$01FF` (the CPU port at `$00/$01`; zero page and the stack change under the KERNAL), not `$D000-$DFFF` (an I/O read has side effects;
our own IO1/IO2 registers do not answer during a DMA), and not `$E000-$FFFF` (with the KERNAL running HIRAM is set, so the CPU sees the
served KERNAL there and a DMA read sees the machine's own ROM chip). Under BASIC (`$A000-$BFFF`) both see what `$01` maps. A SAVE part
stops at `$D000` and the window takes over from there; a VERIFY touching any of those ranges is the stock VERIFY. The zero-page reads
(`$A4`, `$AC-$AF`, `$C3/$C4`) and the pointer writes are a handful of bytes per part.

**Firmware.** `vio_set_dma` takes a read hook as well; the glue's `VD_DmaRead` is `PerformDMA(DMA_READ, ...)` + `CloseDMA()`, set only with
`Fab04_VDriveDMALoad`, like the write (the name stays; it now covers all three). Without the flag: SAVE by window blocks, VERIFY the stock
way, with the same KERNAL. `VD_DMA_LOAD=0` leaves both requests out of the KERNAL.

**Risk on hardware.** The DMA reads are new on a running program. CLAUDE.md records that remote memory reads by DMA (`ReadC64MemToken`,
`trmon`) crashed or corrupted some programs, cause not found; here the CPU only waits in `vcall`'s loop in the KERNAL during the reads, as it
does for the DMA LOAD's writes, which work on hardware. Check this first on a real machine.

**Tests.** `tests/test_vio.py`: DMA SAVE of 0-40000 bytes is the same file as a window SAVE, all bytes by DMA, reads only in
`$0200-$CFFF`, writes only to `$AC/$AD`; SAVE from `$0100`, across `$D000`, from `$E000` (window there); no read hook / no DMA = the window
SAVE; a failed read falls back to the window; a failed pointer write abandons the save once (NOTMINE to the block and the close), the drive
works after it; a refused open needs no DMA. VERIFY: equal and one byte different (`$10`), secondary address 0 at `$C3/$C4`, ranges outside
`$0200-$CFFF` (and right up to `$CFFF`) refused to the stock VERIFY with the channel closed, no read hook refused, not found / no device,
a failed read at open (stock VERIFY), a failed pointer write mid-way (an error), a LOAD after a VERIFY stores again. `tests/fw_host`: the
glue's `PerformDMA` reads from the fake 64 KB (refused at `$D000` and up), a 5000-byte SAVE in three parts, across `$D000`, failing reads,
a failing pointer write, VERIFY equal / different / at `$C3/$C4` / refused ranges / not found / failing write; without
`Fab04_VDriveDMALoad` every `VIO_FSAVE_DMA` answers 1 and `VIO_FVERIFY_OPEN` NOTMINE. VICE `basic` (DMA reads as monitor reads while the CPU
is stopped): the BASIC `VERIFY` of `SAVEME` goes through command 19 with DMA parts only, its corrupted copy gives `?VERIFY  ERROR`, the
6000-byte `SAVE"BIG2"` from `$2000` comes in three DMA reads and loads back intact, `VERIFY"BIG2",8,1` passes and fails with the last
byte changed; the rest of the run (VDSUITE, the BASIC test programs, `VERIFY` without a device number) passes with the new paths. `variant`
(the five ROMs, no DMA host): `SAVE` by window blocks after DATA 1, and `VERIFY` refused at command 19 and done the stock byte-level way.

**On hardware, to check:** a SAVE of a large program and a `VERIFY` of it (and of a changed copy) on a C64 and a C128 in C64 mode; a SAVE
from under BASIC (`$A000-$BFFF` with `$01` set to RAM, by a machine-code SAVE); timing against 0.9.0.52 (`TI` before and after); VDSUITE
"run all"; VDrive+SwiftLink with a terminal open.

## 52. The plain TeensyROM: vector hooks instead of a KERNAL (VICE only; not on hardware)
The plain TeensyROM (Fab 0.2/0.3, `--target tr`) builds without `Fab04_Features`. It has no Kernal Replace and no full DMA, so it can neither serve a patched KERNAL nor read the C64's ROM to patch it. It also lacks the Special button (alt-button disk switching) and the REU Special IOs. Everything this project does on the Teensy side (`d64/`, `vdrive.c`, `vdrive_io.c`, most of the glue) needs none of those. What has to change is how the C64 reaches the mailbox.
**Hooks.** `vector/vdvec.s` points the KERNAL's RAM vectors at its own handlers: `IOPEN` `$031A`, `ICLOSE`, `ICHKIN`, `ICKOUT`, `ICLRCH`, `IBASIN`, `IBSOUT`, `IGETIN`, `ICLALL`, `ILOAD` `$0330`, `ISAVE` `$0332`, plus `IMAIN` `$0302` (below). `IGETIN` is needed because stock GETIN goes to BASIN's serial code without passing through `$0324`. A handler leaves device numbers 0-3 alone. For 4 and up it sends the Teensy the commands the KERNAL patch's hooks send: LISTEN/TALK/SECOND/TKSA/CIOUT/ACPTR/UNLSN/UNTLK and the fast LOAD/SAVE commands. So `vdrive_io.c` sees the same traffic and needs no change. On "not mine" the handler jumps through the vector value it found at install (`old`, the `JMP (old+n)` exits). That keeps real drives, JiffyDOS and wedges installed earlier working, and the stock KERNAL stays as it is, tape included. `vcall` and `bus_scan` are shared with the KERNAL patch (`patches/mailbox.inc`, `patches/vcall.inc`; the patched images are byte-identical after the move). In this build, the scan's `$FFB1`/`$FF93`/`$FFAE` calls are the unhooked real-bus routines.
- **OPEN.** The stock OPEN touches the bus only for a serial device with a name and a secondary address. For those, the handler first sends LISTEN to the Teensy: it doubles as the ownership question and triggers the lazy bus scan when one is due. If the device is ours, the handler calls the previous OPEN with the name length set to 0, so the stock code fills the file table and sends nothing. Then SECOND, the name and UNLISTEN go to the Teensy. A table error (FILE OPEN, TOO MANY FILES) still ends the Teensy's LISTEN.
- **CLOSE.** The CLOSE goes to the Teensy, then the entry's secondary address gets bit 7, so the previous CLOSE only removes the table entry.
- **CHKIN / CHKOUT.** The handler looks the file up as stock `$F30F`/`$F31F` do, then sends TALK+TKSA or LISTEN+SECOND. With no secondary address it sends TALK or LISTEN alone, as stock does.
- **CLRCHN / CLALL.** UNLISTEN/UNTALK go to the Teensy, and the defaults go back to screen and keyboard, so the previous routine sends nothing on the real bus for our devices.
- **CHRIN / GETIN.** One ACPTR per byte when ST is 0. With ST set, the stock code returns CR without bus traffic. Bytes to and from a real drive therefore cost one extra mailbox question each, as they do through the KERNAL patch's hooks.
- **CHROUT.** One CIOUT per byte. A comes back as the byte sent (the `PRINT` regression from the KERNAL patch).
- **LOAD.** The fast path covers LOAD and **VERIFY** (compare instead of store; a difference sets ST bit 4 and the load goes on, as at stock `$F4FE`). The KERNAL patch can leave VERIFY to its byte-level hooks; here nothing would catch it. "SEARCHING FOR" is printed once the Teensy has said "mine".
- **SAVE.** As `fs_hook`.
- **Names too long for the window (128+ characters).** The handler asks with LISTEN/UNLISTEN; for our device it reports FILE NOT FOUND. There is no byte-level LOAD/SAVE in this build. OPEN sends names byte by byte, so a long name works there.
**Install and keeping the hooks.** `SYS` to the block's first byte installs it (`SYS49152` for the VICE build). A vector that already points at us keeps its saved value, so a second `SYS` is harmless; without that check, the handlers would save themselves as "old" and loop. RESTOR (`$FF8A`: RUN/STOP+RESTORE, many programs' start-up, a reset) puts the ROM vectors back, but leaves `$0300-$0313` alone. `v_main` on `IMAIN` (BASIC's direct-mode loop) checks at each `READY.`: if `ILOAD` again holds exactly the value it replaced, it hooks again. Any other value (a wedge installed after us) is left alone. A reset also runs INITV (`$E453`), which resets `IMAIN`, so after `SYS64738` the drives stay off until the `SYS`. The plan for the TR menu is to install from `PRGLoadStartReloc` right after the handler swap (`rRegIOHSwapPoll`) and before `JMP $A7AE`, unless the launched PRG loaded over `$0300-$0333`, where it would overwrite an autostart vector. The Teensy knows the load range. The launch path does not reset the C64: `XferCopyRun` runs RESTOR itself, then streams the PRG.
**What does not reach the drives.**
- Anything that calls the serial routines directly: `$FFB1` LISTEN, `$FFB4` TALK, `$FF93`, `$FF96`, `$FFA5`, `$FFA8`, `$FFAB`, `$FFAE`. That includes DOS wedges and some directory readers, and our own VDCOPY and VDrive Config, which probe units with `LISTEN`+`SECOND` (they would need to probe with an OPEN of channel 15).
- Code that calls the stock routines behind the vectors directly (`$F333`, `$F4A5`, ...).
- Programs that call RESTOR and then never return to `READY.`.
- Programs that install their own vectors without chaining.
Fast loaders were never served (no drive CPU).
**What the KERNAL build has that this one cannot:**
- the drive numbers in the banner (38)
- SHIFT+RUN/STOP (42)
- `SYS64738` back to the TR menu (41)
- the IRQ border flash (22; the Special button is TR+ only anyway)
- device 8 for a bare `LOAD` (33). This would be possible in `v_load` (`FA` = 1 → 8), but it would take the tape away.
**Size and placement (open).** The block is 1,124 bytes. On hardware it has two possible homes:
- **(a) Served by the Teensy from the IO pages.** `$DE20-$DEFF` and `$DF00-$DF7F` are free with no REU, about 350 bytes. The plain TR already serves executable IO2 bytes for Epyx FastLoad. This would cost no C64 RAM, but 1.1 KB would need a bank register (a write switches the served page; execution continues in the new bank). Every opcode fetch would be an IO read, and the read-twice cure of the bus timing margin (see CLAUDE.md, "Bus timing margin") cannot protect an opcode: one late answer is a crash. A soak test (a checksummed loop running from IO2 for hours, on PAL, NTSC and a C128) has to come first. The block writes into itself (`old`), so the served page must keep writes, which the Teensy can do. VICE's GeoRAM (a 256-byte RAM window at `$DE00`, banked by `$DFFE/$DFFF`) can model "code in a banked IO page" for the mock.
- **(b) A RAM copy,** e.g. under a lowered top of BASIC as DOS wedges do it, or at `$C000` as in the VICE build. This costs 1.1 KB and collides with programs that use that RAM.
**Firmware.** Not started.
- VDrive's handlers are gated on `Fab04_VDrive`, and `IOH_VDrive.c` calls `PerformDMA` and touches `KDS_State` / `KernalCheckArm` without guards.
- A plain-TR gate would compile VDrive and VDrive+SwiftLink without `VD_KernalImage`, the `KernalCheck` re-hook, the menu probe (the lazy scan is enough), the alt button and the REU variants.
- The plain TR's RAM budget must be measured from a `--target tr` build.
**Testing (VICE).**
- `make build/vdvec-vice.prg` links the block at `$C000` with the mock's mailbox at `$C800`.
- `make test-vector` (part of `make test`) runs `tools/vdrive_mock.py STOCK vector|vecscan build/vdvec-vice.prg` on the **stock** KERNAL: the PRG is written to RAM, then `SYS49152`.
- `vector` (no real drives):
  - nothing reaches the mailbox before the `SYS`; a second `SYS` is harmless
  - the scan, directory, LOAD (fast, BIG intact), VERIFY OK and VERIFY ERROR
  - 127/130-character names, the CHROUT regression, SAVE (fast), replace, 63, `@0:`, a SEQ round trip
  - `SEQTEST`, `DOSTEST`, `BLKTEST`, `RELTEST`, `DOS2TEST`, `CDTEST`
  - RUN/STOP+RESTORE (a jump to the NMI routine's `$FE66`) heals through `IMAIN`; a reset (`$FCE2`) needs the `SYS` again
  - a dead mailbox falls back to the stock code
- `vecscan` (true-drive 1541s at 8 and 10, ours at 9 and 11):
  - both error channels in one BASIC line (the real 1541's `CBM DOS V2.6 1541` through the chained stock code, then `73 TRVDRIVE`)
  - `FILE NOT FOUND` from the real drives, 5000 bytes from the real 1541 through the chained stock LOAD, the USB unit
  - byte-by-byte BASIC copies real → ours → real → ours, byte-exact (CHKIN/CHKOUT alternating between a real drive and ours)
- VDSUITE is not run here: its default-device and `SYS64738` checks are KERNAL-build features.
**Hardware, when a board is available:** the IO-execution soak test first, then the placement decision, the firmware gate and the menu install.
