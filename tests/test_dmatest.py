#!/usr/bin/env python3
"""DMATEST (tests/asm/dmatest.s) in py65: the host's DMA writes are played by poking the RAM, $01 banking of the BASIC and KERNAL
ROMs is modelled (reads of $A000-$BFFF / $E000-$FFFF see a fake ROM unless banked out; writes always reach RAM), so the
program must bank out to see the pattern and must find the KERNAL sum unchanged. The pattern comes from tools/dmatest.py."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import py65  # noqa: F401
except ImportError:
    print("SKIP py65 not installed (pip install py65): DMATEST not run"); sys.exit(0)

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
from c64_sim import C64  # noqa: E402
import dmatest  # noqa: E402

SEED, ROUND, ACK = 0x03F0, 0x03F1, 0x03F2
TOT, FA, FG, FE, ROUNDS, OVR, ROMERR = 0x0340, 0x0354, 0x035E, 0x0363, 0x037C, 0x037E, 0x0380


def build():
    out = Path(tempfile.mkdtemp())
    cfg = out / "asm.cfg"
    cfg.write_text("MEMORY { LA: start=$07FF, size=2; M: start=$0801, size=$3000, file=%O; }\n"
                   "SEGMENTS { LOADADDR: load=LA, type=ro; CODE: load=M, type=ro; RODATA: load=M, type=ro; }\n")
    subprocess.run(["ca65", "-o", str(out / "d.o"), str(ROOT / "tests/asm/dmatest.s")], check=True)
    subprocess.run(["ld65", "-C", str(cfg), "-o", str(out / "d.prg"), str(out / "d.o")], check=True)
    return (out / "d.prg").read_bytes()


PRG = build()


class Sim(C64):
    def __init__(self, vdrive=True):
        super().__init__(PRG, drive=None, mem={1: 0x37})
        self.ram = self.mem._subject
        if vdrive:
            self.ram[0xDE19], self.ram[0xDE1A] = 0xD6, 0x64
        self.mem.subscribe_to_read(range(0xA000, 0xC000), lambda a: (a * 7 + 1) & 0xFF if self.ram[1] & 3 == 3 else None)
        self.mem.subscribe_to_read(range(0xE000, 0x10000), lambda a: (a * 13 + 5) & 0xFF if self.ram[1] & 2 else None)
        self.traps[0xFFE4] = self.idle

    def idle(self):
        """GETIN: no key. Stops the run while there is nothing new for the program to check."""
        if self.ram[ROUND] == self.ram[ACK]:
            return "wait"
        self.ret(a=0, carry=0)

    def host_round(self, rnd, skip=None, extra=None):
        seed = dmatest.seed_for(rnd)
        for start, data in dmatest.areas(seed):
            for i, v in enumerate(data):
                if start + i != skip:
                    self.ram[start + i] = v
        if extra is not None:
            self.ram[extra] ^= 0xFF
        self.ram[SEED], self.ram[ROUND] = seed, dmatest.round_byte(rnd)
        assert self.run() == "key"

    def w(self, a): return self.ram[a] | self.ram[a + 1] << 8


class DmaTest(unittest.TestCase):
    def setUp(self):
        self.s = Sim()
        self.assertEqual(self.s.run(), "key")           # waiting

    def test_clean_rounds(self):
        for r in (1, 2, 3):
            self.s.host_round(r)
        s = self.s
        self.assertEqual(s.ram[ACK], 3)
        self.assertEqual(s.w(ROUNDS), 3)
        self.assertEqual([s.w(TOT + 2 * i) for i in range(5)], [0] * 5)
        self.assertEqual(s.w(ROMERR), 0)
        self.assertEqual(s.w(OVR), 0)
        self.assertEqual(s.ram[1], 0x37)
        text = s.screen.text()
        self.assertIn("VDRIVE HANDLER: YES", text)
        self.assertIn("ROUNDS   0003", text)
        self.assertIn("2000-3FFF  0000 ----", text)
        self.assertIn("GUARDS     0000 ----", text)
        self.assertIn("WAITING FOR TOOLS/DMATEST.PY", text)

    def test_lost_write_under_kernal(self):
        s = self.s
        s.host_round(1)
        s.host_round(2, skip=0xE123)                     # left from round 1: a different seed, so a wrong byte
        self.assertEqual(s.w(TOT + 6), 1)
        self.assertEqual(s.w(FA + 6), 0xE123)
        self.assertEqual(s.ram[FG + 3], dmatest.pattern(0xE123, dmatest.seed_for(1)))
        self.assertEqual(s.ram[FE + 3], dmatest.pattern(0xE123, dmatest.seed_for(2)))
        self.assertIn("E000-FEFF  0001 E123", s.screen.text())

    def test_lost_write_low_ram_and_stray_write(self):
        s = self.s
        self.assertNotEqual(dmatest.pattern(0x2345, dmatest.seed_for(1)), 0)   # the RAM there is still 0
        s.host_round(1, skip=0x2345, extra=0x4010)       # a guard byte changed: a write outside the ranges
        self.assertEqual(s.w(TOT), 1)
        self.assertEqual(s.w(TOT + 8), 1)
        self.assertEqual(s.w(FA + 8), 0x4010)

    def test_overrun_dropped(self):
        s = self.s
        fired = []

        def late(a):
            if not fired and s.ram[1] & 2 == 0:          # while checking under the KERNAL: the host writes again
                fired.append(a); s.ram[ROUND] = 2
            return None
        s.mem.subscribe_to_read([0xFE80], late)
        s.host_round(1)
        self.assertEqual(s.w(OVR), 1)
        self.assertEqual(s.w(ROUNDS), 1)                 # the program went on to round 2 (same data: clean)
        self.assertEqual(s.ram[ACK], 2)

    def test_no_handler_id(self):
        s = Sim(vdrive=False)
        self.assertEqual(s.run(), "key")
        self.assertIn("VDRIVE HANDLER: NO", s.screen.text())

    def test_key_quits(self):
        s = self.s
        s.ram[ROUND] = 9; s.ram[ACK] = 0                 # the trap answers; give it a key instead
        s.traps[0xFFE4] = lambda: s.ret(a=32, carry=0)
        self.assertEqual(s.run(), "end")


if __name__ == "__main__":
    unittest.main(verbosity=1)
