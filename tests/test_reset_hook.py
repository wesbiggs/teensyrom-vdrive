#!/usr/bin/env python3
"""The cold-start hook (rst_hook, patches/vdrive.s) run in a 6502 simulator (py65; skipped when it is not installed).

$FCE2 (the RESET vector, also SYS64738) jumps to rst_hook, which sends VIO_MENU only when the ID registers $DE19/$DE1A read
$D6 $64 (the VDrive handler answers); otherwise the stock cold start goes on at $FCE5. Synthetic stock image as in
test_irq_border.py (no Commodore ROM needed); the IO1 registers answer from a script, writes to them are recorded."""
import pathlib, subprocess, sys, tempfile

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("SKIP py65 not installed (pip install py65): rst_hook not simulated"); sys.exit(0)

ROOT = pathlib.Path(__file__).resolve().parent.parent
stock = bytearray(8192); stock[0xA31:0xA34] = bytes([0x20, 0xEA, 0xFF]); stock[0x1D9:0x1DB] = bytes([0xA2, 0x01])
stock[0x429:0x447] = bytes.fromhex("a973a0e4201eaba53738e52baaa538e52c20cdbda960a0e4201eab4c44a6")   # INITMS from $E429 (the banner patch)
stock[0x1CE2:0x1CE7] = bytes.fromhex("a2ff789ad8")                                                     # the cold start: LDX #$FF / SEI / TXS / CLD
with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t); (t / "stock.bin").write_bytes(stock)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), str(t / "stock.bin"), str(t / "k.bin")], check=True, capture_output=True)
    img = (t / "k.bin").read_bytes()

CMD, STAT, ID0, ID1 = 0xDE10, 0xDE13, 0xDE19, 0xDE1A


class Mem(list):
    """IO1 as a Teensy handler would answer it: `regs` maps an address to its value (or a list, one per read); `answer` is the STAT
    token after a command (None: never answers, STAT stays BUSY)."""
    def __init__(self, data, regs, answer):
        super().__init__(data); self.regs = dict(regs); self.answer = answer; self.writes = []; self.cmd = False
    def __getitem__(self, i):
        if i == STAT and self.cmd: return self.answer if self.answer is not None else 0x80
        if i in self.regs:
            v = self.regs[i]
            if isinstance(v, list): return v.pop(0) if len(v) > 1 else v[0]
            return v
        if 0xDE00 <= i < 0xDF00: return 0xDE                  # unanswered IO1: the bus floats to the high address byte
        return list.__getitem__(self, i)
    def __setitem__(self, i, v):
        if 0xDE00 <= i < 0xDF00:
            self.writes.append((i, v))
            if i == CMD: self.cmd = True
        list.__setitem__(self, i, v)


def run(regs, answer=0x5A, steps=2_000_000):
    m = MPU()
    m.memory[0xE000:0xE000 + 8192] = list(img)
    mem = Mem(m.memory, regs, answer)
    m.memory = mem
    m.pc, m.sp = 0xFCE2, 0x00
    m.p |= m.DECIMAL                                            # state at power-on is undefined: the stock path does CLD itself
    spin = 0
    for _ in range(steps):
        if m.pc == 0xFCE5: return "stock", mem.writes, m.sp
        pc = m.pc; m.step()
        spin = spin + 1 if m.pc == pc else 0
        if spin > 50: return "spin", mem.writes, m.sp           # `BCC *`: waiting for the Teensy's reset into the menu
    raise AssertionError("ran away at $%04X" % m.pc)


assert img[0x1CE2:0x1CE5] == bytes([0x4C]) + img[0x1CE3:0x1CE5] and img[0x1CE5:0x1CE7] == bytes.fromhex("9ad8"), "JMP at $FCE2, TXS / CLD kept"

# VDrive answers: VIO_MENU goes out, then the CPU waits
what, writes, _ = run({ID0: 0xD6, ID1: 0x64})
assert what == "spin" and writes == [(CMD, 16)], (what, writes)

# the TR menu's own handler (a launch): $DE19/$DE1A are a SID's init address (0 with none), the menu ignores writes there anyway
for sid_init in (0x0000, 0x1000, 0x1003, 0xC000, 0x6400, 0xD6FF - 0x0100):
    what, writes, sp = run({ID0: sid_init >> 8, ID1: sid_init & 0xFF})
    assert what == "stock" and writes == [] and sp == 0xFF, (hex(sid_init), what, writes)

# no TR, or plain Kernal Replace: nothing answers, every read is $DE
what, writes, _ = run({})
assert what == "stock" and writes == [], (what, writes)

# a late (bit-mixed) first read that disagrees with the second: no command
what, writes, _ = run({ID0: [0xD6, 0xD4], ID1: 0x64})
assert what == "stock" and writes == [], (what, writes)
what, writes, _ = run({ID0: 0xD6, ID1: 0x65})
assert what == "stock" and writes == [], (what, writes)

# IDs read right but the Teensy never answers (vcall's ~1 s time-out) or says "not mine": the stock cold start after all
what, writes, sp = run({ID0: 0xD6, ID1: 0x64}, answer=None)
assert what == "stock" and writes == [(CMD, 16)] and sp == 0xFF, (what, writes)
what, writes, _ = run({ID0: 0xD6, ID1: 0x64}, answer=0xA5)
assert what == "stock" and writes == [(CMD, 16)], (what, writes)

print("PASS rst_hook: menu request / launch under the menu handler / no handler / late reads / no answer")
