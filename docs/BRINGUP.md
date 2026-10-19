# TR+ hardware bring-up checklist — VDrive KERNAL

Nothing in this project has run on a TR+ yet. This is the staged plan for the first time. Each stage has a
pass criterion and a stop rule: **do not move on until the stage passes**, and record the numbers in the
table at the end. Everything up to stage 3 can be done without the patched KERNAL, so a failure there points at
the firmware side, not the 6502 side.

Equipment: TR+ (PCB v0.4), **a real C64** (a flat NTSC C128 in C64 mode did not reliably serve the replacement KERNAL: do not use one; a stock-image control can't detect this because the bytes are identical, so always include the banner proof in stage 6), SD card with a few `.d64`
images, a USB stick (second unit), a USB cable to the TR+ for the serial console (the TR USB power trace is cut: the C64
must be powered). Nice to have: a logic analyzer / scope with 4 channels (PHI2, /IO1, /IO2, R/W; data bus optional).

**Which build is on the board?** Patched builds are versioned `<stock>.<rev>` (e.g. `0.9.0.1`, from `firmware/REV`): look at the TR menu title or the hex file name
(`TeensyROM+_0.9.0.1_full.hex`). Stock firmware says plain `0.9`.

## Stage 0 — baseline the firmware you will patch
1. Check out TeensyROM at a known commit; use Teensyduino **1.61.0** (1.62.0 is documented as bad in `docs/Architecture/Build-System.md`).
2. `npm run build:c64` then `npm run build:tr-plus` (see `Source/BuildInfo.md` for exact commands) **without** our patch.
3. Record from the build log: `RAM1: variables / code / padding / free for local variables`, and the flash size. This is your reference.
4. Flash it (below), boot a C64, confirm the stock menu, REU or Kernal Replace with a stock dump still work.
   - Flashing: copy the generated `.hex` to the SD/USB drive and launch it from the TR menu (documented firmware-update path), or use the
     Teensy loader with the C64 powered.
   - **Pass:** unmodified TR+ boots and behaves as before. **Stop** if not: your toolchain/board, not this project.

## Stage 1 — patched firmware builds and fits
1. `cd TeensyROM && git apply /path/to/c64exp/firmware/teensyrom-vdrive.patch` (`make` in this repo regenerates it with `tools/make_tr_patch.sh`).
2. Build for tr-plus. Compare with stage 0:
   | Metric | Expected | Why |
   |---|---|---|
   | RAM1 code | + a few hundred bytes | only the IO1/IO2 handlers and 4 register accessors are not `FLASHMEM` (132 B measured on arm64 -Os) |
   | RAM1 variables | ≈ + 100 B | `vdUnit[2]`, pointers; the drive state (~4.6 KB) is `DMAMEM` (RAM2) |
   | Flash | + ~10-16 KB | d64/vdrive/vdrive_io in `FLASHMEM` (arm64 -Os measured 16 KB; Thumb-2 should be smaller) |
   - If it does not link (RAM1 or RAM2), try `-DVD_ALLOC_HEAP` (heap instead of RAM2), or reduce `VD_NSLOT` in `d64/vd_cfg.h`
     (each slot ≈ 290 B per unit; 6 → 4 saves ~1.2 KB).
   - If it does not compile: the glue was only compiled against host stubs. Likely suspects are `File::seek/read/write/flush`
     signatures on the Teensy `FS` classes, `FILE_WRITE`, and `DMAMEM` placement of a struct with `volatile` members.
3. **Pass:** links, RAM1 padding still > ~4 KB, flash fits. Record the numbers.

## Stage 2 — handler is registered, nothing else changed
1. Flash. Boot. Settings → Special IO: `a`/`A` cycles to **VDrive KERNAL** (name ≤ 20 chars).
2. Open the USB serial console (115200 irrelevant, CDC): on selecting/launching with this Special IO you should see
   `VDrive: dev 8 (SD) ready, dev 9 (USB) ready|absent`. Insert/remove media and re-launch to see `absent` flip.
3. Do **not** select a KERNAL image yet. Launch a PRG: it must run normally (the KERNAL hook is only installed when an image is loaded).
- **Pass:** name appears, serial message correct, PRGs run. **Stop** on a menu/enum crash: the `IOH_VDrive` entries in `Menu_Regs.h` and
  `IOHandlers.h` must match positionally (there is a `static_assert` on the count).

## Stage 3 — mailbox registers from BASIC (stock KERNAL, no patch)
Select Special IO = VDrive KERNAL, start BASIC (F2, or launch a trivial PRG). Registers: IO1 = 56832.. (`CMD ARG DATA STAT ST LEN` = +0..+5), IO2 window = 57088.
```
POKE56833,90 : PRINT PEEK(56833)            expect 90   (ARG reads back; the not-mine path depends on it)
POKE57088,170 : POKE57343,85 : PRINT PEEK(57088);PEEK(57343)     expect 170 85  (window RW, both ends)
POKE56833,8 : POKE56832,1 : FORI=0TO99:NEXT : PRINT PEEK(56835)  expect 90  ($5A: LISTEN 8 handled; idle STAT is also 90)
POKE56833,4 : POKE56832,1 : FORI=0TO99:NEXT : PRINT PEEK(56835)  expect 165 ($A5: LISTEN 4 is NOT MINE; BUSY would be 128)
POKE56832,7 : REM UNLSN to finish the dangling LISTEN
POKE57088,36 : POKE56837,1 : POKE56833,8 : POKE56832,9 : FORI=0TO99:NEXT
PRINT PEEK(56834);PEEK(57088);PEEK(57089)                         expect 0 1 4   (fast LOAD "$": ok, load address $0401)
POKE56832,10 : FORI=0TO99:NEXT : PRINT PEEK(56834);PEEK(56836)    count > 0; ST = 0 or 64
```
If a directory listing appears in the window (`PRINT CHR$(PEEK(57088+6))` ...) the whole ISR→poll→ISR path works.
- **Pass:** all values as expected, 10× repeatable. **Stop** if STAT never clears (polling handler not running → check `Special IO` really active),
  or reads return 0xFF/garbage (bus timing; go to the scope section below).

## Stage 4 — scope the bus (once, before the patched KERNAL)
Trigger on /IO1 low. Check against the same signals from an existing handler (e.g. TR_BASIC, `$DEB6` status reads):
- Read: data valid before PHI2 falls, held until PHI2 low, bus released afterwards; /IO1 width unchanged.
- Write: the handler reads the byte inside the PHI2-high window (`DataPortWaitRead`).
- Nothing in the VDrive path should make the ISR longer than the TR_BASIC register reads; `vio_io1_*` are a switch and a store.
- **Pass:** edges match the reference handler. **Stop** on any bus conflict (two drivers on the data bus): power off.

## Stage 5 — control: stock KERNAL through the replacement path
`make build/kernal-stock.bin` (byte-identical copy of your 901227-03 dump). Copy to SD, `<Shift-K>` it, select **VDrive KERNAL** as Special IO,
F2 to BASIC.
- **Pass:** BASIC looks and behaves exactly like the stock KERNAL (screen colours, `LOAD"$",8` on a real drive if you have one).
  This isolates the TR+ KERNAL-replacement feature (HIRAM sensing etc.) from our patches. **Stop** on any difference: not our bug.

## Stage 6 — proof patch (this is the real "is it being served" test)
`make build/kernal-banner.bin` (only the banner changes: `TRVDRIVE 64 BASIC V2`; 21 bytes differ from stock).
- **Pass:** new banner shows right after F2/PRG launch and the keyboard works. Quick re-check from BASIC: `FORI=0TO31:PRINTPEEK(58483+I);:NEXT` must print the bytes of `TRVDRIVE` (84 82 86 68 82 73 86 69 at positions 11-18), not `COMMODORE` (67 79 77 77 ...), which would mean the C64's own ROM is answering.

## Stage 7 — the real image, drive path in order of risk
`make build/kernal-tr.bin` (mailbox at `$DE10`, window `$DF00`). With `VD_TRACE` defined (off by default since 0.9.0.10: the printing delays the bus handler and corrupts bytes on the C64) every mailbox command prints
`vd cmd=.. arg=.. len=.. -> stat=.. data=.. st=..` on the serial console (`stat=5a` answered, `stat=a5` not mine) (the print runs after the answer is published, so it can make the C64's next reads late: use it for diagnosis only, never for soak or data tests).
Check after each line; keep the trace open.
1. Banner and BASIC still fine; a plain `PRINT`, `POKE`, `LOAD"X",11` → `?DEVICE NOT PRESENT` (stock IEC path through the hook, ~instant, trace shows `stat=a5` not-mine on LISTEN).
2. `LOAD"$",8` with nothing mounted → SD card listing (title `SD:/`), `DIR`/`D64`/`PRG` types. Trace: `cmd=9` then `cmd=10` blocks.
3. `OPEN15,8,15,"CD:GAME.D64"` → no error; `LOAD"$",8:LIST` → disk header and files. Then `PRINT DS$` style status via
   `OPEN15,8,15:INPUT#15,A,B$:PRINT A;B$:CLOSE15` → `0 OK`.
4. `LOAD"NAME",8` (small BASIC), `RUN`. Then a large PRG with `,8,1` (compare checksum of memory against the file; the `vdrive_mock.py` test does exactly this in VICE).
5. `SAVE"TEST",8`, power-cycle the C64 **after waiting ~1 s** (write-back is deferred 250 ms), reload and compare on a PC with `c1541 -attach … -list`.
   - Pull the card only after the 250 ms idle flush; yanking it mid-write may corrupt the image (no journalling).
6. `VERIFY`, sequential `OPEN2,8,2,"X,S,W"` + `PRINT#` + read back, `S:` scratch, `R:` rename, `N:NAME,ID` (on a copy!).
7. Device 9: insert USB stick, `OPEN15,9,15,"CD:X.D64"`, repeat 2-6. Remove the stick and `LOAD"$",9` → `?DEVICE NOT PRESENT`.
8. **Device numbers (only if you want something other than 8/9):** `OPEN15,8,15,"U0>"+CHR$(10):CLOSE15`, then `LOAD"$",10` lists the card (header id `10`) and `LOAD"$",8` goes to the real bus; power-cycle and confirm 10 stuck (EEPROM); `OPEN15,10,15,"U0>"+CHR$(0):CLOSE15` restores 8.
9. **Time-out check:** select a different Special IO (e.g. None) but keep the patched KERNAL image loaded → `LOAD"X",8` should print
   `?DEVICE NOT PRESENT` after ~1 s instead of hanging (this is `vcall`'s time-out; tested in VICE only).
- **Pass criteria:** every item above (items 8-9 if you use them), three times in a row; trace has no `stat=a5` on devices 8/9 and no unexpected `st=80`.

## Stage 8 — soak and edge cases
- Loop in BASIC: `10 LOAD"BIG",8,1:GOTO10` for 10 minutes (each pass ≈ 25 mailbox block calls); `SAVE`/`LOAD` cycle 100×; after each batch `c1541 -validate` on the image copy must not change the BAM.
- Press RUN/STOP during a long LOAD (fast path does not poll STOP; expected: ignored, load completes) and during a directory `LIST`.
- Tape is gone by design: `LOAD"X",1` now crashes into reclaimed code — confirm it is merely wrong, not a hang you didn't expect.
- Games/demos that call KERNAL `LOAD` should work; fast loaders and anything touching the serial bus directly will not (they never reach the KERNAL).
- Hot-unplug SD/USB during idle then `LOAD"$"`: expected `?DEVICE NOT PRESENT` or `FILE NOT FOUND`, never a hang.

## If something fails — quick triage
| Symptom | First suspects |
|---|---|
| `?DEVICE NOT PRESENT` for 8, SD inserted | `SDFullInit()` false at init (re-launch with card in), `vdrive_attach(...present)`; serial line at startup |
| ~1 s pause then `?DEVICE NOT PRESENT` on every device-8/9 access | mailbox never answers: wrong Special IO selected, KERNAL image not from the same build (`MBX/WIN` addresses), polling handler not running |
| Garbage data, wrong file contents | IO2 window read timing (stage 4), `WIN` base differs from firmware (`$DF00`), `MBX` base differs from firmware (`$DE10`), block count byte |
| Hang during LOAD | `BUSY` stuck: firmware blocked in SD/USB call (the 6502 will give up after ~1 s), `USBFileSystemWait` |
| Directory OK, LOAD fails with `FILE NOT FOUND` | PETSCII↔ASCII conversion on host side (`VD_ToAscii`), long names, wildcard in name |
| Files vanish after power cycle | write-back flush (250 ms idle) not reached; `File::flush()` semantics on the media |
| Link error RAM1 | `-DVD_ALLOC_HEAP`, lower `VD_NSLOT`, or move more code to `FLASHMEM` |

## Stage 9 — automatic device numbers, D71/D81, paths (firmware 0.9.0.7, host- and VICE-tested only)
Build with the new patch (`firmware/REV` 7) and a KERNAL image from the same tree (`make build/kernal-tr.bin`); the image and firmware must match (an old
image with the new firmware just keeps 8/9 after one ~1 s pause; a new image with the old firmware changes nothing).
1. No other drive on the bus: `LOAD"$",8` as the first command lists the SD card as before; `PEEK(56854)` is 8 (and `PEEK(56855)` 9 with a stick). Trace:
   `cmd=1` (LISTEN, answered `stat=3c`), `cmd=14`, one or two `cmd=15`, then the original command again. Note how long the first access takes.
2. A real drive (1541 / SD2IEC / Meatloaf) at 8 and nothing at 9: restart, `LOAD"$",9` lists the SD card, `PEEK(56854)` is 9, `LOAD"$",8` reaches the real drive.
3. Real drives at 8 and 9: SD must become 10, USB 11. A real drive at 9 only: SD 8, USB 10.
4. Fix a number with `OPEN15,8,15,"U0>"+CHR$(12):CLOSE15`: restart, the SD drive is 12 and the scan only places the USB drive. `U0>"+CHR$(0)` makes it automatic again.
5. A `.d81` on the SD card: `OPEN15,8,15,"CD:BIG.D81":CLOSE15`, `LOAD"$",8`, `LOAD"NAME",8,1`, `SAVE"X",8`; same for a `.d71`; `c1541` reads the file back from the image.
6. Paths: `LOAD"$/FOLDER/GAME.D64",8` (mounts, lists), `LOAD"FOLDER/GAME.D64/NAME",8,1`, `OPEN15,8,15,"UI"` then `INPUT#15,A,B$` gives `73 TRVDRIVE V1.0`.
7. Slow-starting device: power the real drive after the C64 and see that the numbers collide; pin them with `U0>` and see the problem go away.

## Stage 8 — VDrive+SwiftLink (firmware 0.9.0.4, host-tested only)
Select Special IO **VDrive+SwiftLink** (or keep VDrive as the default Special IO and launch a program the menu pairs with SwiftLink, e.g. CCGMS).
The mailbox is at `$DE10`; the SwiftLink ACIA stays at `$DE00-$DE07`.
1. Stage 7 items 1-3 still pass with this Special IO selected (device 8 directory/`LOAD`).
2. Launch CCGMS (built-in menu entry) with VDrive as the default Special IO: it starts, the serial trace says `Loading IO handler: VDrive+SwiftLink`, `AT` answers `OK`, and `LOAD"$",8` in a second session still works (or use CCGMS's own disk directory).
3. Download a file over the modem to device 8, then check it with `c1541`. Repeat at a higher baud rate (Turbo232 / 9600+) and watch for dropped bytes: SD sector writes mask interrupts, so this is the timing risk.
4. A `.crt` SwiftLink/Turbo232 cart is **not** remapped (carts run with the C64's own KERNAL).
**Stop** if the C64 hangs at the start of a modem session or bytes drop only while the drive is busy: `IO1`/ISR timing (stage 4) or SD writes masking the NMI.
## Rollback
`<Shift-K>` to a stock KERNAL (or none) and choose another Special IO; or reflash the stage-0 firmware. Nothing is stored outside EEPROM
settings and the files on the card.

## Results table (fill in)
| Item | Value |
|---|---|
| Stage 0 RAM1 padding / flash (stock, `[main]`, TeensyROM FW 0.9 tree, Teensyduino core 1.61.0) | padding 13,816; flash code 277,892; RAM2 vars 17,600 |
| Stage 1 RAM1 padding / flash (delta) | padding 12,840 (−976); flash code 293,604 (+15,712); RAM1 code +976; RAM1 vars +352; RAM2 vars 22,144 (+4,544). **Stage 1 passed** (builds, links, padding ≫ 4 KB) |
| Stage 8 build (0.9.0.4, VDrive+SwiftLink, real toolchain) | builds and links; RAM1 padding 12,840 (unchanged from stage 1), flash code 294,988 (+1,384 vs stage 1) |
| `LOAD"BIG"` 6000 B, wall-clock | |
| `SAVE` 6000 B, wall-clock | |
| Mailbox calls per 6000 B load (trace) | expect ~27 |
| Stage 9 (0.9.0.7: scan, D71/D81, paths) | paths, auto-mount, remembered mounts, remote get/set: passed (0.9.0.11). Not run: scan with a real 1541, `.d81` on the card |
| Stage 7 items passed (of 9) | owner-verified: all of 1-9 except the explicit no-handler time-out (0.9.0.11 + double-read KERNAL; USB device 9, `U0>` and its persistence, `DOSTEST`, `CDTEST`, `SEQTEST` pass). Before the double reads `SEQTEST` failed on hardware: see the bus test rows below |
| Soak: loads without error | `SOAK` with no errors (0.9.0.11 + double-read KERNAL) |
| Bus stress test, `BUSTEST` errors per 8x65536 accesses (0.9.0.9, trace on) | T1 3E, T2 B8, T3 12, T4 0D, T5 0A, T6 3B, T7 0B; T8 (stock KERNAL ROM read) 0 |
| Same, 0.9.0.11 (IO1 reads straight from the register bytes) | T1 6, T2 C, T3 9, T4 A, T5 A, T6 7, T7 5; T8 0. The raw bus still errs about once in 50,000 accesses, so the KERNAL double-reads |
| VDrive+SwiftLink (stage 8), `.d81` on the card, bus scan with a real 1541, `STATUS` vs a real drive | not run |
| *Quest for the King* with VDrive active | fails (starts, does not finish loading, corrupts); control with stock KERNAL not run yet |
