# BASIC test programs (run on the C64, and automatically in VICE by `make test`)

Source is `petcat` text (lowercase = what a C64 shows in uppercase). `make testdisk` builds `build/testdisk/vdtest.d64`
with these programs plus `HELLO` and `BIG` (6000 bytes at `$2000`). Copy it to the SD card (or USB stick), then:

```
OPEN15,8,15,"CD:VDTEST.D64":CLOSE15
LOAD"SEQTEST",8 : RUN        (device 9 for the USB stick)
```

| Program | What it checks | Expected end result |
|---|---|---|
| `SEQTEST` | writes a 100-line sequential file, reads it back with `INPUT#`, compares every field, scratches it | `SEQ TEST PASS` |
| `DOSTEST` | rename, rename onto an existing name (63), scratch one / by wildcard (01 + count), create-existing refused (63), unknown command (31) | `9 PASSED, 0 FAILED` |
| `BLKTEST` | direct access: `OPEN n,d,n,"#"`, `B-A` (and 65 on a used sector), `B-P`, `U2` write, `U1` read back (256 bytes, EOI on the last), bad track gives 66, `B-F`, a closed buffer gives 70. Allocates one sector of its own and frees it again | `12 PASSED, 0 FAILED` |
| `CDTEST` | `CD:/` (root listing), mount `VDTEST.D64`, header shows the disk, `CD:/` unmounts, missing image gives 62, state unchanged after the failure. Needs `VDTEST.D64` in the card root | `6 PASSED, 0 FAILED` |
| `SOAK` | endless loop: clears memory, `LOAD"BIG",8,1`, checksums it. Any key stops it | `PASS n ERRORS 0` counting up; any `MISMATCH` is a failure |
| `STATUS` | probe, not a test: prints the raw status text for a few commands in `[brackets]` so you can compare a real drive with ours (spacing, wording). Run it on your real drive 8, then on ours | compare the two outputs |
| `RENUM` | `U0>` renumber. Run with current=8 new=10, then power-cycle and run with current=10 new=8 to prove it persisted and put it back | `NEW DEVICE 10 ANSWERS: 0 OK` (then `... 8 ...`) |

Notes:
* They ask `DEVICE?` first: just press RETURN for 8, or type 9 for the USB unit.
* `RENUM`'s second run is loaded with `LOAD"RENUM",10` after the first run, since the drive is device 10 until you change it back.
* `SOAK` relies on `LOAD` inside a running program continuing from the first line with variables kept (it uses a state flag).
* The programs only create and remove their own temporary files (`SEQTST`, `DT*`, `NUMTST`; `BLKTEST` allocates and frees one sector); none formats the disk.
