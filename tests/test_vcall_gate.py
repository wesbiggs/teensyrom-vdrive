#!/usr/bin/env python3
"""The mailbox gate (vd_here, called by vcall in patches/vdrive.s) in a 6502 simulator (py65; skipped when it is not installed).

A KERNAL in a ROM socket (a MEGA65's C64 core, a burnt EPROM) runs with no VDrive handler too: no TR, another Special IO, the TR
menu's own handler. vcall then must not write the mailbox at all and hand every call straight to the real bus, not wait for its
~1 s time-out. With the handler there, a late (bit-mixed) read of ID0 must not send a transaction to the real bus: vd_here tries
4 times. Runs LISTEN ($ED0C, the hook) on a synthetic stock image as in test_reset_hook.py; reaching $F0A4 (stock LISTEN's
ATN send) means the real bus."""
import pathlib, subprocess, sys, tempfile

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("SKIP py65 not installed (pip install py65): vd_here not simulated"); sys.exit(0)

ROOT = pathlib.Path(__file__).resolve().parent.parent
stock = bytearray(8192); stock[0xA31:0xA34] = bytes([0x20, 0xEA, 0xFF]); stock[0x1D9:0x1DB] = bytes([0xA2, 0x01]); stock[0x5EE:0x5F3] = bytes([0xA2, 0x09, 0x78, 0x86, 0xC6])
stock[0x429:0x447] = bytes.fromhex("a973a0e4201eaba53738e52baaa538e52c20cdbda960a0e4201eab4c44a6")   # INITMS from $E429 (the banner patch)
stock[0x49A:0x4AC] = b" 64K RAM SYSTEM  \0"                                       # the stock line's text (the banner reuses it)
with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t); (t / "stock.bin").write_bytes(stock)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), str(t / "stock.bin"), str(t / "k.bin")], check=True, capture_output=True)
    img = (t / "k.bin").read_bytes()

CMD, ARG, DATA, STAT, ST, ID0, ID1 = 0xDE10, 0xDE11, 0xDE12, 0xDE13, 0xDE14, 0xDE19, 0xDE1A
RET = 0x1234                                                    # LISTEN's caller: a JSR from $1231


class Mem(list):
    """IO1 as a handler would answer it: `regs` maps an address to a value or a list (one per read, the last one sticks);
    `answer` is the STAT token once a command is written (None: STAT stays BUSY). IO1 and IO2 writes are recorded."""
    def __init__(self, data, regs, answer):
        super().__init__(data); self.regs = dict(regs); self.answer = answer; self.writes = []; self.cmd = False; self.reads = 0; self.win = {}
    def __getitem__(self, i):
        if 0xDE00 <= i < 0xE000: self.reads += 1
        if i == STAT and self.cmd: return self.answer if self.answer is not None else 0x80
        if i in (DATA, ST) and self.cmd: return 0
        if i in self.win: return self.win[i]                    # the IO2 window reads back what was written to it
        if i in self.regs:
            v = self.regs[i]
            if isinstance(v, list): return v.pop(0) if len(v) > 1 else v[0]
            return v
        if 0xDE00 <= i < 0xE000: return 0xDE                  # unanswered IO1 / IO2: the bus floats
        return list.__getitem__(self, i)
    def __setitem__(self, i, v):
        if 0xDE00 <= i < 0xE000:
            self.writes.append((i, v))
            if i == CMD: self.cmd = True
            if i >= 0xDF80: self.win[i] = v
        list.__setitem__(self, i, v)


def listen(regs, answer=0x5A, steps=3_000_000):
    """LISTEN 8 through the hook: ("mailbox"|"bus", mailbox writes, cycles, X, Y)"""
    m = MPU()
    m.memory[0xE000:0xE000 + 8192] = list(img)
    mem = Mem(m.memory, regs, answer)
    m.memory = mem
    m.sp = 0xFD; mem[0x1FF] = (RET - 1) >> 8; mem[0x1FE] = (RET - 1) & 0xFF
    m.a, m.x, m.y, m.pc = 8, 0x33, 0x44, 0xED0C
    for _ in range(steps):
        if m.pc == RET: return "mailbox", mem.writes, m.processorCycles, m.x, m.y
        if m.pc == 0xF0A4: return "bus", mem.writes, m.processorCycles, m.x, m.y
        m.step()
    return "timeout?", mem.writes, m.processorCycles, m.x, m.y


HERE = {ID0: 0xD6, ID1: 0x64}

# the handler answers: LISTEN 8 goes through the mailbox, X/Y kept
what, writes, _, x, y = listen(HERE)
assert what == "mailbox" and (CMD, 1) in writes and (ARG, 8) in writes and (x, y) == (0x33, 0x44), (what, writes, x, y)
# ... and says "not mine" (a real drive at 8): the real bus
what, writes, _, _, _ = listen(HERE, answer=0xA5)
assert what == "bus" and (CMD, 1) in writes, (what, writes)

# no handler (no TR, another Special IO): straight to the real bus, nothing but nothing written past ARG, in well under the time-out
what, writes, cyc, x, y = listen({})
assert what == "bus" and writes == [], (what, writes)
assert cyc < 300 and (x, y) == (0x33, 0x44), (cyc, x, y)
# the TR menu's own handler: $DE19/$DE1A are a SID's init address, $DE13 a stored EEPROM byte that may even read like a token
for sid_init in (0x0000, 0x1000, 0xC000, 0x6400):
    for pwrup in (0x5A, 0xA5, 0x3C, 0x00):
        what, writes, cyc, _, _ = listen({ID0: sid_init >> 8, ID1: sid_init & 0xFF, STAT: pwrup})
        assert what == "bus" and writes == [] and cyc < 300, (hex(sid_init), pwrup, what, writes, cyc)

# a late (bit-mixed) ID0 read with the handler there: up to 3 misses still reach the mailbox, a 4th miss in a row does not
for misses in (1, 2, 3):
    what, writes, _, _, _ = listen({ID0: [0xD4] * misses + [0xD6], ID1: 0x64})
    assert what == "mailbox" and (CMD, 1) in writes, (misses, what, writes)
what, writes, _, _, _ = listen({ID0: [0xD4] * 4 + [0xD6], ID1: 0x64})
assert what == "bus" and not any(w[0] == CMD for w in writes), (what, writes)



def fload(regs):
    """LOAD"NAME",8 at the fast-LOAD hook ($F4C8, after SEARCHING FOR): ("mailbox"|"stock", IO writes)"""
    m = MPU()
    m.memory[0xE000:0xE000 + 8192] = list(img)
    mem = Mem(m.memory, regs, 0x5A)
    m.memory = mem
    name = b"NAME"
    for k, c in enumerate(name): mem[0x0300 + k] = c
    mem[0x93], mem[0xB7], mem[0xBB], mem[0xBC], mem[0xBA] = 0, len(name), 0x00, 0x03, 8
    m.sp, m.x, m.pc = 0xFD, 1, 0xF4C8
    for _ in range(100_000):
        if m.pc == 0xF3D5: return "stock", mem.writes          # the displaced `JSR $F3D5`: the serial open on the real bus
        if any(w[0] == CMD for w in mem.writes): return "mailbox", mem.writes
        m.step()
    raise AssertionError("ran away at $%04X" % m.pc)

# the fast LOAD fills the IO2 window and LEN before its command: with no handler, none of that
for regs in ({}, {ID0: 0x10, ID1: 0x03}):
    what, writes = fload(regs)
    assert what == "stock" and writes == [], (regs, what, writes)
what, writes = fload(HERE)
assert what == "mailbox" and (0xDE15, 4) in writes and (0xDF80, ord("N")) in writes and (ARG, 8) in writes, (what, writes)

print("PASS vd_here: mailbox with the handler / real bus at once without it (also under the TR menu) / late ID reads / fast LOAD open")
