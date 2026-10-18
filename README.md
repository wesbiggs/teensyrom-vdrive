# TR+ virtual drive

Make the SD card and the USB drive of a [TeensyROM+](https://github.com/SensoriumEmbedded/TeensyROM) behave as IEC disk drives on a
Commodore 64, backed by `.d64`, `.d71` and `.d81` images. No IEC cable, no drive.

It has two halves:

* **A patched KERNAL** (901227-03 plus a few hundred bytes of 6502, served live by the TR+). The serial routines
  (`LISTEN TALK SECOND TKSA CIOUT ACPTR UNLSN UNTLK`) ask a mailbox in I/O space (`$DE10`, with a 256-byte window at `$DF00`) whether the
  device is virtual. If so the Teensy answers; otherwise the stock IEC code runs, so real drives keep working. LOAD and SAVE have
  block-transfer fast paths.
* **Teensy firmware** (`firmware/`, a patch for the TeensyROM source) with a portable C99 D64/D71/D81 engine (`d64/`) and a virtual drive on top of it:
  channels, the command channel, directory listings, write support, `CD` into folders and images.

From BASIC it looks like a drive: `LOAD"$",8`, `LOAD"GAME",8,1`, `SAVE"X",8`, `OPEN15,8,15,"CD:/GAMES/DEMO.D64"`.
Device numbers are automatic (SD and USB take the first free numbers from 8 up) or fixed with `U0>`.

## Status
Works on a real C64 with a TR+ for the core path (directory, LOAD, SAVE, VERIFY, error channel); newer features are tested on the host and
in VICE only. See the Status section of [CLAUDE.md](CLAUDE.md) for exactly what has and has not been checked on hardware.
The C128 is not supported, and tape (device 1) no longer works because its code space was reused.

## Building and testing
Needs `cc65` (`ca65`/`ld65`), VICE (`x64sc`, `c1541`), a C/C++ compiler and Python 3.

```bash
make test
```

The KERNAL image is built from your own stock ROM, since no Commodore ROM bytes are in this repository:

```bash
make build/kernal-tr.bin STOCK=/path/to/kernal-901227-03.bin
```

Firmware build, flashing and usage are in [firmware/README.md](firmware/README.md).

## Documentation
* [firmware/README.md](firmware/README.md): installing and using it, device numbers, what changed in the KERNAL.
* [docs/DESIGN.md](docs/DESIGN.md): the design, one section per milestone, including alternatives considered (section 19).
* [docs/BRINGUP.md](docs/BRINGUP.md): the staged hardware bring-up checklist.
* [CLAUDE.md](CLAUDE.md): project notes, status and gotchas for contributors.

## License
Apache License 2.0, see [LICENSE](LICENSE). The TeensyROM firmware this patches is MIT-licensed, which is compatible; a firmware build combines both, so keep both license notices when distributing one.
