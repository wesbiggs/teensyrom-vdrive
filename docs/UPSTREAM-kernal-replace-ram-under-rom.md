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

## Fix (VDrive firmware 0.9.0.13, `firmware/kernalreplace-fixes.patch`)
Confirmed on hardware: once HIRAM is known to be RAM (sensed, and no write to `$00`/`$01` since), `KernalCheck` returns before `SetGameAssert`.
`VAULTTEST` is then clean (0 errors in every column), with the stock image and ours. Quest for the King plays for long stretches.

Three more findings from that work, useful for the Kernal Replace code in general:
* **HIRAM cannot be tracked from the writes.** The 6510 does not put the value of an on-chip port write (`$00`/`$01`) on the external data bus (the bus
  shows junk/VIC data), so the state has to be sensed (Skoe method) on the first `$E000+` read after each write. That first read is answered late
  (sense first, then drive): the real ROM's byte wins, so it is only harmless where the served byte equals the stock ROM's.
* **Serve the image from DTCM.** `KernalBin` is malloc'ed in cached RAM2; a cold cache line in the ISR costs 100 ns or more. A `uint8_t KernalFast[8192]` copy in
  RAM1 removed one class of wrong first bytes.
* **The Ethernet stack makes phi2 interrupts late.** Once Ethernet is initialized (a cable is not needed), about 39 per second take 1.1-1.5 us longer
  than the ISR can afford. Masking the Ethernet (ENET) interrupt and skipping TCP polling did not remove them; masking the PIT interrupt did.
  NativeEthernet runs `fnet_poll()` (the whole FNET stack) from a 1 ms `IntervalTimer` ISR, and that is what delays the phi2 interrupt, even though
  the GPIO interrupt has the higher priority (`FNET_POLL_TIME` in NativeEthernet.h; FNET's own IRQ disable is a no-op). The same poll called from `loop()`
  (about 6% of calls take over 5 us, a few 50 us) never coincided with a late phi2 interrupt; late interrupts fell to about one per 2 s.
  Late ISRs hit hardest at the first read after a `$01` write (the sense above): programs that switch `$01` constantly (Quest for the King, CCGMS
  with SwiftLink NMIs) got corrupt reads, NMIs or freezes. The TeensyROM developer terminal in Rx polling mode (rather than interrupt mode) worked well.
  **0.9.0.14 fix (Teensy.ino `loop()`):** while a Kernal Replace, VDrive or SwiftLink handler runs and the stack is initialized, mask `IRQ_PIT` and call
  `fnet_poll()` from `loop()` every ms. Blocking calls (`connect`, DHCP, DNS inside AT commands) wait for the stack and would hang, so `ProcessATCommand`
  unmasks PIT while it runs. Hardware: no stalls or flow-control problems in CCGMS over TCP (a rare glitch lines up with a rare late ISR),
  q4k and VDrive fine with Ethernet on. The counters are in `ISRs.c` (`LateIsrCnt`, `LateIsrBkt[]`), printed by `Teensy.ino` as `LATE-ISR ...`; compiled in only with `LATE_ISR_TRACE`.
  A start-up trace (`TRACE +ms ...`, same define) showed that 0.9.0.14 still had ~15 late ISRs in the first ~0.4 s after a launch, enough to crash q4k: `loop()` only masked the PIT timer once it saw the new handler. **0.9.0.15:** `PreStart` masks `IRQ_PIT` and starts the `loop()` poll immediately (`FnetLoopPollStart()`, held by `FnetPollHold` until the main menu returns); afterwards 0-1 late ISRs per second.
* **Tried and dropped:** a "serve map" (TR answers only the bytes that differ from the stock ROM, the real ROM serves the rest) broke disk access and made the
  first banner character wrong in every build that used it; whole-image serving is back.
