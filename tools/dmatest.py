#!/usr/bin/env python3
"""Host side of DMATEST (tests/asm/dmatest.s): DMA-write a pattern into C64 RAM with the TR+'s remote WriteC64Mem, round after round.

Start DMATEST on the C64 first (it is on build/testdisk/vdtest.d64), then:
    tools/dmatest.py -H 192.168.1.50 --rounds 50
Each round writes $2000-$3FFF, $A000-$BFFF, $C000-$CFFF and $E000-$FEFF (28.4 KB) with a new seed, then SEED/ROUND at $03F0;
the C64 checks every byte and shows the counts. A clean run: every ERRS column 0000, ROM SUM 0000, OVERRUN 0000.
By default the script waits --delay seconds between rounds and reads nothing back (DMA reads are a crash suspect of their own);
--sync instead reads the C64's acknowledgement at $03F2 (one byte per poll) and prints the C64's counts at the end.
Needs trlib.py from teensyrom-rcp: $TRLIB_DIR, or ../teensyrom-rcp next to this repository. TCP is better than USB here: a USB
reply from the TR runs with interrupts off (CLAUDE.md, remote commands)."""
import argparse
import os
import sys
import time
from pathlib import Path

AREAS = ((0x2000, 0x4000), (0xA000, 0xC000), (0xC000, 0xD000), (0xE000, 0xFF00))
NAMES = ("2000-3FFF", "A000-BFFF", "C000-CFFF", "E000-FEFF", "GUARDS")
SEED_AT, ACK_AT, RESULTS_AT = 0x03F0, 0x03F2, 0x0340


def pattern(addr, seed):
    """the byte DMATEST expects at addr"""
    return (((addr & 0xFF) ^ seed) + (addr >> 8) * 0x3B) & 0xFF


def seed_for(rnd): return (rnd * 0x9D + 0x11) & 0xFF


def round_byte(rnd): return (rnd - 1) % 255 + 1          # 1-255: never 0, the program's start value


def areas(seed):
    return [(a, bytes(pattern(x, seed) for x in range(a, b))) for a, b in AREAS]


def trlib():
    here = Path(__file__).resolve().parent.parent
    d = os.environ.get("TRLIB_DIR") or str(here.parent / "teensyrom-rcp")
    sys.path.insert(0, d)
    try:
        import trlib as t
    except ImportError:
        sys.exit(f"dmatest: trlib.py not found in {d} (set TRLIB_DIR to your teensyrom-rcp checkout)")
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-H", "--host", help="host[:port] or a serial device (else $TR_HOST)")
    ap.add_argument("--rounds", type=int, default=50)
    ap.add_argument("--delay", type=float, default=2.0, help="seconds between rounds without --sync (the C64 needs about 1 s to check)")
    ap.add_argument("--sync", action="store_true", help="wait for the C64's acknowledgement (DMA reads of $03F2) and print its counts")
    ap.add_argument("--chunk", type=int, help="bytes per WriteC64Mem command (trlib's default is 4096)")
    ap.add_argument("--selftest", action="store_true", help="write one wrong byte at $E123 every round: E000-FEFF must count one per round")
    a = ap.parse_args()
    t = trlib()
    if a.chunk:
        t.MEM_CHUNK = a.chunk
    total = sum(b - s for s, b in AREAS)
    with t.TR(*t.split_host(a.host)) as tr:
        wtime = 0.0
        for rnd in range(1, a.rounds + 1):
            seed, rb = seed_for(rnd), round_byte(rnd)
            t0 = time.monotonic()
            for start, data in areas(seed):
                if a.selftest and start == 0xE000:
                    data = bytearray(data); data[0x123] ^= 0xFF
                tr.write_mem(start, data)
            tr.write_mem(SEED_AT, bytes([seed, rb]))
            dt = time.monotonic() - t0
            wtime += dt
            print(f"round {rnd:3}: seed ${seed:02x}, {total} bytes in {dt * 1000:.0f} ms", end="", flush=True)
            if a.sync:
                t1 = time.monotonic()
                while tr.read_mem(ACK_AT, 1)[0] != rb:
                    if time.monotonic() - t1 > 10:
                        print(); sys.exit(f"dmatest: no acknowledgement of round {rnd} in 10 s (is DMATEST running?)")
                    time.sleep(0.1)
                print(f", checked after {time.monotonic() - t1:.1f} s")
            else:
                print()
                time.sleep(a.delay)
        print(f"{a.rounds} rounds, {a.rounds * total} bytes written, {a.rounds * total / wtime / 1024:.0f} KB/s with the transport")
        if a.sync:
            r = tr.read_mem(RESULTS_AT, 0x48)
            w = lambda o: r[o] | r[o + 1] << 8  # noqa: E731
            print(f"C64: rounds {w(0x3C)}, overrun {w(0x3E)}, ROM sum changed {w(0x40)}")
            for i, n in enumerate(NAMES):
                fa = w(0x14 + 2 * i)
                first = f" first ${fa:04x} read ${r[0x1E + i]:02x} expected ${r[0x23 + i]:02x}" if fa else ""
                print(f"  {n:10} {w(2 * i):5} wrong{first}")
        else:
            print("read the counts on the C64 screen")


if __name__ == "__main__":
    main()
