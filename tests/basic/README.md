# VDSUITE, the test program (run on the C64, and automatically in VICE by `make test`)

Source is `petcat` text (lowercase = what a C64 shows in uppercase). `make testdisk` builds `build/testdisk/vdtest.d64` with `VDSUITE`
(this BASIC program with the machine-code checks of `tests/asm/suite.s` appended: `tools/suite_files.py`, `build_vdsuite`), `DMATEST`,
LOADTEST's data files `LDA000` / `LDD000`, `HELLO` and `BIG` (6000 bytes at `$2000`, for the checks by hand), `VDCOPY` and `VDCONFIG`;
next to it `vdbad.d64` and `vdplain.prg`. Every GitHub release carries the three files (`.github/workflows/artifacts.yml`).
Copy all three to the root of the SD card (or USB stick), then:

```
OPEN15,8,15,"CD:VDTEST.D64":CLOSE15
LOAD"VDSUITE",8 : RUN        (device 9 for the USB stick)
```

It asks `DEVICE?` first: RETURN for 8, or 9 for the USB unit. Then a menu:

| Choice | What it checks | Expected end result |
|---|---|---|
| `1` setup | `CD:/` and the root's header (`SD:` / `USB:`), folder `VDSCRATCH` in the card root with `VS64.D64`, `VS71.D71`, `VS81.D81` (made with `N:` when missing; prints how long each `N:` took), a missing image (62) leaves the selection alone | `SETUP: 9 PASSED, 0 FAILED` |
| `2`-`4` | the same 44 checks on each image: quick `N:`, header and blocks free, a 100-line SEQ file by `INPUT#` and again byte by byte by `GET#` (EOI on the last), rename / scratch / create-existing / unknown command, `C:` join and its errors, append, `L:` lock, `W-1` / `W-0`, direct access (`#`, `B-A`, `B-P`, `U2`, `U1`, bad track 66, `B-F`), REL files (30 records, past the end 50, overflow 51, `C:` of a REL file), blocks free again and after `V` | `D64 IMAGE: 44 PASSED, 0 FAILED` |
| `5` bad sectors | `VDBAD.D64`: `U1` 21, a `GET#` read and a KERNAL `LOAD` stopping at the bad sector with 23 | |
| `6` card files | nothing mounted: `VDPLAIN.PRG` (open, `.PRG` added, pattern, path, 62/74, KERNAL `LOAD`); `MD`/`RD` and their errors | `CARD FILES: 20 PASSED, 0 FAILED` |
| `7` cmd paths | `CD//`, `CD/`, `MD//dir/:x`, `RD/x`, a file by CMD path through an image, `$//dir/:pat`, `N//dir/:x.d64` | |
| `8` clean up | `S:` deletes the images it made, also by path and CMD path, refuses patterns (33) and other files (64); `RD:` removes `T2` and `VDSCRATCH` | |
| `9` run all | `1`-`8`, the clean-up last, so a run leaves nothing behind and makes the images again next time | `TOTAL 191 PASSED, 0 FAILED` (failed checks are listed under it) |
| `10` checks by hand | the list of what it cannot check by itself | |
| `11` bus stress | BUSTEST (`tests/asm/bustest.s`): the mailbox registers and the window at full speed, no commands, about a minute; per test the wrong reads and their bits (hex) | `BUS STRESS: 1 PASSED, 0 FAILED` |
| `12` load test | LOADTEST (`tests/asm/loadtest.s`): mounts `VDTEST.D64` from the card root, loads `LDA000` under BASIC and `LDD000` across `$D000` and checks every byte, the guards and the end address; the jiffies show DMA (~2) or window (~14) | `LOAD TEST: 2 PASSED, 0 FAILED` |
| `13` ram under rom | VAULTTEST (`tests/asm/vaulttest.s`): `$D000-$FEFF` with `$01=$34`, as *Quest for the King* uses it (a Kernal Replace check, no mailbox) | `RAM UNDER ROM: 1 PASSED, 0 FAILED` |
| `14` reu | REUTEST (`tests/asm/reutest.s`): the REU's size by bank wrap and every byte of it | `REU: 2 PASSED, 0 FAILED` |
| `15` soak | saves a 4 KB file on `VS64.D64`, then loads and checks it until a key is pressed | `PASS n ERRORS 0` counting up, then `SOAK: 2 PASSED, 0 FAILED` |
| `16` status text | not a test: the raw error channel text of a few commands in `[brackets]`. Start VDSUITE with a real drive's number and run it there too, then compare (spacing, wording) | compare the two outputs |
| `17` renumber | `U0>` (sent by `PRINT#`, with its CR): the new number answers and the suite goes on with it. Power-cycle, run it from the new number to see it kept, then `17` with the old number puts it back | `NEW DEVICE 10 ANSWERS: 0 OK` |
| `0` quit | closes the command channel and resets with `SYS 64738`: back to the TR menu since 0.9.0.43 | |

Notes:
* `11`-`15` need a C64 (or a C128 in C64 mode); `11`-`14` also need the program as loaded (the machine code sits after the BASIC text; editing a
  line drops it). VDSUITE lowers the top of BASIC to `$6000` at start: REUTEST's buffers and the soak's file live above it.
* `VDSUITE` changes the selection (it ends at the card root, nothing mounted) and works in `/VDSCRATCH` on the card: its three images (quick-formatted on every run), `VSCMD.D64`, and `T2/E.D64`. `8` (also the last step of `9`) deletes them all with `S:` and the folders with `RD:`; a single section run on its own leaves them for the next one, reused, so the `N:` times show only when an image is made. It loads with the KERNAL's `LOAD` through `SYS` (BASIC's `LOAD` stops with `?LOAD ERROR` on a time-out, which a bad sector gives) to `$C000`, and uses `$0340` for the name.
* In VICE (`make test`, `tools/vdrive_mock.py`) all sections run except `12` (its `LDD000` would overwrite the mock's mailbox at `$C800`); `11` there checks
  the mock's RAM mailbox, so only the hardware says anything about bus timing.
* `DMATEST` stays a program of its own (it writes `$2000-$3FFF`, where VDSUITE's BASIC is): `docs/DESIGN.md` section 48.
