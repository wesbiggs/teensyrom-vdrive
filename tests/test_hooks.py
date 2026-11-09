#!/usr/bin/env python3
"""The plain TR's hook block (vector/vdhooks.s) in a 6502 simulator (py65; skipped when it is not installed): f_key, the KEYLOG hook
(CTRL+C= + F1 / F3 -> the KEY register, once per press), and f_irq, the border flash on CINV (the KERNAL build's irq_border with the
saved colour in the block). Assembled for the hardware layout: the block at $DF10, the mailbox registers at $DF00."""
import pathlib, subprocess, sys, tempfile

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("SKIP py65 not installed (pip install py65): vdhooks not simulated"); sys.exit(0)

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASE, MBX = 0xDF10, 0xDF00
BORDER, KEY = MBX + 8, MBX + 11
OLD_IRQ, OLD_KEYLOG, F_SEI, SAVED, F_IRQ, F_KEY = BASE, BASE + 2, BASE + 4, BASE + 6, BASE + 7, BASE + 44
with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t)
    subprocess.run(["ca65", "-I", str(ROOT / "patches"), "-D", "MBX=$%04X" % MBX, "-o", str(t / "h.o"), str(ROOT / "vector/vdhooks.s")], check=True)
    subprocess.run(["ld65", "-C", str(ROOT / "vector/agent.cfg"), "-S", "0x%04X" % BASE, "-o", str(t / "h.prg"), str(t / "h.o")], check=True)
    prg = (t / "h.prg").read_bytes()
assert prg[:2] == bytes([BASE & 0xFF, BASE >> 8])
code = prg[2:]
assert len(code) <= 0x70, len(code)


class Mem(list):
    """BORDER answers from a script (read twice by the routine); writes to KEY are recorded"""
    def __init__(self, data, reads): super().__init__(data); self.reads = reads; self.n = 0; self.keys = []
    def __getitem__(self, i):
        if i == BORDER:
            v = self.reads[min(self.n, len(self.reads) - 1)]; self.n += 1; return v
        return list.__getitem__(self, i)
    def __setitem__(self, i, v):
        if i == KEY: self.keys.append(v)
        list.__setitem__(self, i, v)


def cpu(reads=(0x80,)):
    m = MPU()
    m.memory[BASE:BASE + len(code)] = list(code)
    mem = Mem(m.memory, list(reads))
    m.memory = mem
    mem[OLD_IRQ:OLD_IRQ + 2] = [0x31, 0xEA]; mem[OLD_KEYLOG:OLD_KEYLOG + 2] = [0x48, 0xEB]   # as INSTALL writes them
    m.sp = 0xF0
    return m, mem


def go(m, pc, stop):
    m.pc = pc
    for _ in range(100):
        if m.pc == stop: return
        m.step()
    raise AssertionError("did not reach $%04X (at $%04X)" % (stop, m.pc))


def irq(first, second=None, d020=0x0E, saved=0x00):
    m, mem = cpu([first, first if second is None else second])
    mem[0xD020] = d020; mem[SAVED] = saved
    go(m, F_IRQ, 0xEA31)                      # ends in the old IRQ routine
    return list.__getitem__(mem, 0xD020), list.__getitem__(mem, SAVED)


def key(mods, now, last):
    m, mem = cpu()
    mem[0x028D] = mods; mem[0xCB] = now; mem[0xC5] = last
    go(m, F_KEY, 0xEB48)                      # ends in the old KEYLOG routine, the scan's state untouched
    assert (mem[0x028D], mem[0xCB], mem[0xC5]) == (mods, now, last)
    return mem.keys


# KEYLOG: CTRL (4) + C= (2) exactly, a key that was not down at the last scan
assert key(6, 0x04, 0x40) == [0x04], "CTRL+C=+F1: the SD unit's next image"
assert key(6, 0x05, 0x40) == [0x05], "CTRL+C=+F3: the USB unit's next image"
assert key(6, 0x04, 0x04) == [], "held since the last scan: once per press"
assert key(6, 0x05, 0x04) == [0x05], "F1 then F3 without letting go of CTRL+C="
for mods in (0, 1, 2, 4, 5, 7):
    assert key(mods, 0x04, 0x40) == [], "other modifiers: %d" % mods
assert key(6, 0x0A, 0x40) == [0x0A], "any other key with CTRL+C= goes to the register too (the Teensy ignores it)"
print("PASS vdhooks f_key: CTRL+C=+F1 / F3, once per press, other modifiers ignored")

# CINV: the border flash, as irq_border in the KERNAL build
assert irq(0xC2) == (0x02, 0x00)
assert irq(0xCF) == (0x0F, 0x00)
assert irq(0xB0, d020=0xF6) == (0xF6, 0xF6)           # save
assert irq(0xD0, d020=0x02, saved=0xF6) == (0xF6, 0xF6)
assert irq(0x80, saved=0x33) == (0x0E, 0x33)          # idle
for v in (0x00, 0xFF, 0xDE, 0xA0, 0xB1, 0xD1, 0xDF, 0xE0, 0xBF, 0x5A, 0xA5, 0x3C, 0x70):
    assert irq(v, saved=0x33) == (0x0E, 0x33), hex(v)
assert irq(0xC2, 0xC3) == (0x0E, 0x00)                # two reads that disagree do nothing
m, mem = cpu(); mem[OLD_IRQ:OLD_IRQ + 2] = [0x00, 0xC0]
go(m, F_IRQ, 0xC000)                                   # chains to whatever CINV held
print("PASS vdhooks f_irq: set / save / restore / idle / garbage / disagreeing reads, chains to the old CINV")

m, mem = cpu(); m.p &= ~0x04; m.memory[0x01F1:0x01F3] = [0x33, 0x12]; m.sp = 0xF0   # JSR'd from $1234: RTS to $1234
go(m, F_SEI, 0x1234)
assert m.p & 0x04, "f_sei sets I"
print("PASS vdhooks f_sei")
