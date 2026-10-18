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

- **Mailbox engine** (`d64/vdrive_io.{h,c}`): IO1 `CMD ARG DATA STAT ST LEN` + a 256-byte IO2 window. The ISR-side accessors are O(1); `vio_poll()` does everything else. New commands: `FLOAD_OPEN/BLK`, `FSAVE_OPEN/BLK/CLOSE` built from the byte-level protocol (so there is one source of truth).
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
One path syntax for `CD` and for names: `[/]dir/dir/image.d64[/file]`. A leading `/` starts at the card root, `..` (or `_`, the left arrow) goes up (from a mounted image it only unmounts), a component with an image suffix is mounted and ends the walk, any other component is a folder, and a component with `*` or `?` is not entered but is the pattern (`path_walk()` in `vdrive.c`, one host `chdir`/`mount` call per component). The consequences, all by the same rule: `CD:/GAMES/DEMO.D64` and `CD0:...`; `LOAD"$/GAMES/DEMO.D64",8` mounts and lists the image; `LOAD"$GAMES/",8` lists a folder; `LOAD"$/GAMES/DEMO.D64/S*",8` lists matching files of the image; `LOAD"GAMES/DEMO.D64/START",8,1` and `SAVE"/X.D64/NEW",8` work through the path; `LOAD"DEMO.D64",8` with nothing mounted mounts it and returns its directory (the SD2IEC convention). With an image mounted a bare name is always a file inside the image (`CD:_` or a path with a `/` leaves it). **Listings only list:** a directory listing by path (`$/GAMES/G.D64`, `$GAMES/`, `LOAD"G.D64"`) restores the folder and image selected before when its channel closes (`host.push` before the walk, `host.pop` on close; `restore_ch` per unit; the glue keeps the `/.vdrive` sidecar untouched meanwhile). `$==path` (also `$:==`, `$0:==`; a single `=` is the CBM listing modifier `$=T`, `$=P` and stays a pattern) does the walk for good, like `CD`, and lists. A *file* loaded through a path (`LOAD"GAMES/G.D64/START"`) also keeps the selection, because the program usually goes on to load more from the same image (`pop` with `out == NULL` drops the snapshot). Without the push/pop callbacks (old hosts) paths select for good. A path with nothing after the image shows the directory, so the "file" a LOAD of an image returns is its directory listing. Names can be up to 63 characters (the command buffer); the 16-character limit applies to the file name after the walk. `CD` also now accepts several components in one command. `UI`/`UJ`/`U9` answer `73,TRVDRIVE V1.0,00,00` and close all channels (works with nothing mounted), `UI+`/`UI-` are accepted, block commands (`U1` ...) give `31` instead of silently succeeding.

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
* Both fail with "VDrive is not running" unless the VDrive (or VDrive+SwiftLink) handler is active. File names come back lower-cased (names go through PETSCII), folder names as they were given; FAT does not care.

**Quiet point.** The set command runs in the main loop between polls, so it never splits a mailbox command. It then waits (calling `PollingHndlr_VDrive()` itself, so the C64 keeps being answered) until `VD_Quiet()`: 40 ms without a mailbox command and no open bus transaction or fast LOAD/SAVE, or 1 s without any mailbox activity at all (whatever was open has been abandoned). After 3 s without a quiet moment it gives up with "C64 is busy with the drive". A missing or non-image path is rejected *before* anything is touched, so a typo leaves the current disk mounted; a mount that fails after the check puts the previous image back.

**What the swap does.** It is `CD:` for the unit: `vdrive_attach()` closes every channel on the unit and resets the status channel to `00, OK`. The channel pool lives on the Teensy, so nothing has to be cleaned up on the 6502; a program that keeps a file open across a swap gets errors on it, as with a real drive. The new mount is written to the `/.vdrive` sidecar like any other.

**Not done:** a firmware-to-6502 "disk changed" flag or generation counter (nothing on the C64 side needs it yet), listing images over the remote port while a program runs (the file commands are still refused as busy), and the Alt-button/freezer integration. Client: `trvdrive` in the `teensyrom-rcp` tools.
