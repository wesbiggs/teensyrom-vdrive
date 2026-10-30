"""A C64 machine-code program run in py65 with the KERNAL calls it makes answered in Python (a test aid, used by tests/test_vdconfig.py).

The program is loaded from its PRG and started at the SYS address of its BASIC stub, as if BASIC had called it. A JSR to one of the KERNAL
jump-table entries below is carried out here and returns like the real routine: SETLFS, SETNAM, OPEN, CLOSE, CHKIN, CHKOUT, CLRCHN,
CHRIN, CHROUT, GETIN, PLOT, LISTEN, SECOND, UNLSN. Any other jump into the ROM area is an error. Files go to a drive object
(open / close / get / getline / put / st, and .units for "present"), the screen is a 40x25 model with cursor addressing, reverse and colour
codes, keys come from a script. run() returns "key" when the program waits for a key it does not have and "end" when it returns to BASIC.
Kept from the real KERNAL on purpose: CHRIN from a serial file answers CR without reading while ST ($90) is not 0, every read ORs its
bits into ST, and bytes sent to a file reach the drive at UNLISTEN (CLRCHN / CLOSE), so a command is one PRINT#."""
from py65.devices.mpu6502 import MPU

COLOR_OF = {144: 0, 5: 1, 28: 2, 159: 3, 156: 4, 30: 5, 31: 6, 158: 7, 129: 8, 149: 9, 150: 10, 151: 11, 152: 12, 153: 13, 154: 14, 155: 15}
STATUS = 0x90
END = 0xFF00            # the return address handed to the program: reaching it means "back in BASIC"


class Screen:
    def __init__(self):
        self.ch = [[" "] * 40 for _ in range(25)]
        self.rev = [[False] * 40 for _ in range(25)]
        self.col = [[1] * 40 for _ in range(25)]
        self.r = self.c = 0
        self.reverse = False
        self.color = 1
        self.scrolled = 0

    def clear(self):
        for r in range(25):
            for c in range(40): self.ch[r][c] = " "; self.rev[r][c] = False; self.col[r][c] = self.color
        self.r = self.c = 0

    def put(self, code):
        if code == 147: self.clear(); return
        if code == 19: self.r = self.c = 0; return
        if code == 18: self.reverse = True; return
        if code == 146: self.reverse = False; return
        if code in COLOR_OF: self.color = COLOR_OF[code]; return
        if code == 17: self.r = min(24, self.r + 1); return
        if code == 145: self.r = max(0, self.r - 1); return
        if code == 13: self.c = 0; self.newline(); self.reverse = False; return
        if code & 0x7F < 32: return
        self.ch[self.r][self.c] = chr(code); self.rev[self.r][self.c] = self.reverse; self.col[self.r][self.c] = self.color
        self.c += 1
        if self.c >= 40: self.c = 0; self.newline()    # the C64 wraps as soon as the last column is written (and scrolls on the last row)

    def newline(self):
        if self.r < 24: self.r += 1
        else:
            self.scrolled += 1
            self.ch = self.ch[1:] + [[" "] * 40]; self.rev = self.rev[1:] + [[False] * 40]; self.col = self.col[1:] + [[1] * 40]

    def row(self, r): return "".join(self.ch[r]).rstrip()
    def text(self): return "\n".join(self.row(r) for r in range(25))


def petscii(s): return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)


class C64:
    def __init__(self, prg, drive, mem=None):
        prg = bytes(prg)
        self.cpu = c = MPU()
        self.mem = c.memory
        load = prg[0] | prg[1] << 8
        self.mem[load:load + len(prg) - 2] = list(prg[2:])
        for a, v in (mem or {}).items(): self.mem[a] = v
        i = prg.index(b"\x9e") + 1
        start = int(prg[i:prg.index(b"\0", i)].decode().strip())
        self.screen, self.drive, self.keys = Screen(), drive, []
        self.files, self.inp, self.outp, self.wbuf, self.cmdbuf = {}, None, None, bytearray(), {}
        self.lfs, self.name, self.ldev, self.steps = (0, 0, 0), "", None, 0
        c.sp = 0xF6
        self.push((END - 1) >> 8); self.push((END - 1) & 0xFF)
        c.pc = start
        self.traps = {0xFFBA: self.setlfs, 0xFFBD: self.setnam, 0xFFC0: self.open, 0xFFC3: self.close, 0xFFC6: self.chkin, 0xFFC9: self.chkout,
                      0xFFCC: self.clrchn, 0xFFCF: self.chrin, 0xFFD2: self.chrout, 0xFFE4: self.getin, 0xFFF0: self.plot,
                      0xFFB1: self.listen, 0xFF93: self.second, 0xFFAE: self.unlsn}

    # ---- CPU helpers ----
    def push(self, v): self.mem[0x100 + self.cpu.sp] = v; self.cpu.sp = (self.cpu.sp - 1) & 0xFF

    def pull(self): self.cpu.sp = (self.cpu.sp + 1) & 0xFF; return self.mem[0x100 + self.cpu.sp]

    def ret(self, a=None, x=None, y=None, carry=None):
        c = self.cpu
        if a is not None: c.a = a & 0xFF
        if x is not None: c.x = x & 0xFF
        if y is not None: c.y = y & 0xFF
        if carry is not None: c.p = (c.p | c.CARRY) if carry else (c.p & ~c.CARRY)
        c.p = (c.p & ~(c.ZERO | c.NEGATIVE)) | (c.ZERO if c.a == 0 else 0) | (c.a & 0x80)

    def run(self, max_steps=20_000_000):
        """run until the program returns to BASIC ("end") or waits for a key it does not have ("key")"""
        c = self.cpu
        while True:
            pc = c.pc
            if pc == END: return "end"
            t = self.traps.get(pc)
            if t:
                if t() == "wait": return "key"
                lo = self.pull(); hi = self.pull(); c.pc = ((hi << 8) | lo) + 1
                continue
            if pc >= 0xA000 and not 0xC000 <= pc < 0xD000: raise RuntimeError("call into the ROM at $%04X, not modelled" % pc)
            c.step()
            self.steps += 1
            if self.steps > max_steps: raise RuntimeError("runaway program at $%04X" % c.pc)

    def press(self, *keys):
        for k in keys:
            if isinstance(k, str): self.keys.extend(ord(petscii(ch)) for ch in k)     # unshifted keys give the upper case codes
            else: self.keys.append(k)

    # ---- KERNAL ----
    def setlfs(self): self.lfs = (self.cpu.a, self.cpu.x, self.cpu.y)

    def setnam(self):
        a = self.cpu.x | self.cpu.y << 8
        self.name = bytes(self.mem[a:a + self.cpu.a]).decode("latin1")

    def open(self):
        lf, dev, sa = self.lfs
        if lf in self.files: return self.ret(a=2, carry=1)
        if dev not in self.drive.units: self.mem[STATUS] |= 0x80; return self.ret(a=5, carry=1)
        self.files[lf] = (dev, sa)
        self.drive.open(lf, dev, sa, self.name)
        self.ret(carry=0)

    def flush(self):
        if self.outp is not None and self.wbuf: self.drive.put(self.outp, self.wbuf.decode("latin1"))
        self.wbuf = bytearray()

    def close(self):
        lf = self.cpu.a
        if lf == self.outp: self.flush(); self.outp = None
        if lf == self.inp: self.inp = None
        if self.files.pop(lf, None) is not None: self.drive.close(lf)
        self.cmdbuf.pop(lf, None)
        self.ret(carry=0)

    def chkin(self):
        lf = self.cpu.x
        if lf not in self.files: return self.ret(a=3, carry=1)
        self.inp = lf; self.ret(carry=0)

    def chkout(self):
        lf = self.cpu.x
        if lf not in self.files: return self.ret(a=3, carry=1)
        self.flush(); self.outp = lf; self.ret(carry=0)

    def clrchn(self):
        self.flush(); self.inp = self.outp = None

    def chrin(self):
        if self.inp is None: raise RuntimeError("CHRIN from the keyboard is not modelled")
        if self.mem[STATUS]: return self.ret(a=13)
        dev, sa = self.files[self.inp]
        if sa == 15:
            buf = self.cmdbuf.get(self.inp) or bytearray((self.drive.getline(self.inp) + "\r").encode("latin1"))
            b = buf.pop(0); st = 0 if buf else 64
            self.cmdbuf[self.inp] = buf
        else:
            b = self.drive.get(self.inp); st = self.drive.st
        self.mem[STATUS] |= st
        self.ret(a=b, carry=0)

    def chrout(self):
        if self.outp is None: self.screen.put(self.cpu.a)
        else: self.wbuf.append(self.cpu.a)
        self.ret(carry=0)

    def getin(self):
        if not self.keys: return "wait"
        self.ret(a=self.keys.pop(0), carry=0)

    def plot(self):
        if self.cpu.p & self.cpu.CARRY: return self.ret(x=self.screen.r, y=self.screen.c)
        self.screen.r, self.screen.c = min(24, self.cpu.x), min(39, self.cpu.y)

    def listen(self): self.ldev = self.cpu.a

    def second(self):
        if self.ldev not in self.drive.units: self.mem[STATUS] |= 0x80

    def unlsn(self): pass
