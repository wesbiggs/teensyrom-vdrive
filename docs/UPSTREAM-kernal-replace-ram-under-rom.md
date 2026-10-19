# Kernal Replace corrupts reads of the RAM under the KERNAL (draft report for TeensyROM)

Found while testing the VDrive KERNAL on a TR+ (PCB v0.4, real C64, firmware 0.9). Nothing here depends on VDrive.

## Symptom
With Special IO = Kernal Replace and any KERNAL image (even the unmodified 901227-03), reading RAM at `$E000-$FFFF` with the KERNAL banked
out (`$01 = $34`, all RAM) now and then returns a wrong byte. Quest for the King keeps its map there and switches `$01` constantly, so it
freezes at start-up, shows wrong glyphs or ends in the NMI handler, differently on every run. With Special IO = None the same test is clean.

## Reproduction
`tests/asm/vaulttest.s` in this repository (a 536-byte PRG, also on `build/testdisk/vdtest.d64` as `VAULTTEST`; needs no mailbox):
it switches to all-RAM, writes or reads pages of `$D000-$FEFF`, switches back, and counts wrong bytes per column (`$Dxxx`, `$Exxx`, `$Fxxx`).
T1 one all-RAM session for the whole fill and verify; T2 one session per page; T3 one session per 64 bytes (like the game's row reads).

| Setup | `$Dxxx` | `$Exxx` | `$Fxxx` |
|---|---|---|---|
| Kernal Replace, stock 901227-03 image (3 runs, T1-T3) | 0 | 2-10 | 5-15 |
| Kernal Replace, image with 21 bytes of text changed (2 runs) | 0 | 2-11 | 4-10 |
| Kernal Replace, image with our patch (2 runs) | 0 | 2-18 | 3-13 |
| Special IO: None | 0 | 0 | 0 |

(Counts are wrong bytes per run of about a million RAM reads; one K2 run also left a stray HOME character, i.e. a corrupted screen-editor byte.)
The image does not matter, and `$Dxxx` (RAM under I/O, no hook involved) is always clean: the error is in what `KernalCheck` does to a RAM read.

## Suspect (from reading `IOH_KernalReplace.c`, not verified with a scope)
`KernalCheck()` runs for every CPU read at `$E000-$FFFF` and calls `SetGameAssert` before it knows whether the cycle is ROM or RAM. When
`HIRAM_State` is unknown (after any write to `$00`/`$01`) it also asserts EXROM and drives the address bus for the Skoe sense. A RAM read that lands in
those windows is taken over (Ultimax-style) and the CPU gets the bus instead of the RAM byte. Worth checking: skip the GAME assertion when the state
is known to be RAM, and the timing of the first read after a `$01` write.

## Where it was seen
Real C64 (breadbin), TR+ firmware 0.9 plus the VDrive patch (the handler is not involved: Special IO = Kernal Replace alone shows it).
