#!/usr/bin/env python3
"""The border-flash routine (irq_border, patches/vdrive.s) run in a 6502 simulator (py65; skipped when it is not installed).

The KERNAL is assembled onto a synthetic 8 KB image that only has the `JSR $FFEA` at $EA31 the patch retargets, so no Commodore
ROM is needed. $DE18 (the BORDER register) answers from a script, $D020 / $9C are plain memory."""
import pathlib, subprocess, sys, tempfile

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("SKIP py65 not installed (pip install py65): irq_border not simulated"); sys.exit(0)

ROOT = pathlib.Path(__file__).resolve().parent.parent
stock = bytearray(8192); stock[0xA31:0xA34] = bytes([0x20, 0xEA, 0xFF])
with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t); (t / "stock.bin").write_bytes(stock)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), str(t / "stock.bin"), str(t / "k.bin")], check=True, capture_output=True)
    img = (t / "k.bin").read_bytes()


class Mem(list):
    def __init__(self, data, reads): super().__init__(data); self.reads = reads; self.n = 0
    def __getitem__(self, i):
        if i == 0xDE18:
            v = self.reads[min(self.n, len(self.reads) - 1)]; self.n += 1; return v
        return list.__getitem__(self, i)


def run(first, second=None, d020=0x0E, z9c=0x00):
    m = MPU()
    m.memory[0xE000:0xE000 + 8192] = list(img)
    entry = m.memory[0xEA32] | (m.memory[0xEA33] << 8)
    mem = Mem(m.memory, [first, first if second is None else second])
    m.memory = mem
    mem[0xD020] = d020; mem[0x9C] = z9c
    m.pc, m.sp = entry, 0xF0
    for _ in range(100):
        if m.pc == 0xF69B: break               # UDTIM: the routine ended where the stock IRQ code goes on
        m.step()
    assert m.pc == 0xF69B, hex(m.pc)
    return list.__getitem__(mem, 0xD020), list.__getitem__(mem, 0x9C)


assert int.from_bytes(img[0xA32:0xA34], "little") >= 0xF8E2, "JSR $FFEA operand retargeted"
assert run(0xC2) == (0x02, 0x00)                      # set border colour 2
assert run(0xCF) == (0x0F, 0x00)
assert run(0xB0, d020=0xF6) == (0xF6, 0xF6)           # save: $9C = what $D020 reads
assert run(0xD0, d020=0x02, z9c=0xF6) == (0xF6, 0xF6) # restore from $9C
assert run(0x80, z9c=0x33) == (0x0E, 0x33)            # idle
for v in (0x00, 0xFF, 0xDE, 0xA0, 0xB1, 0xD1, 0xDF, 0xE0, 0xC0 - 1, 0x5A, 0xA5, 0x3C, 0x70):
    assert run(v, z9c=0x33) == (0x0E, 0x33), hex(v)   # anything but the exact request values does nothing
assert run(0xC2, 0xC3) == (0x0E, 0x00)                # two reads that disagree do nothing
print("PASS irq_border: set / save / restore / idle / garbage / disagreeing reads")
