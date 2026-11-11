"""The vector engine (d64/vdrive_vec.c) against a Python stand-in for the agent (vector/vdagent.s): the script ops, INSTALL and its
fast-path vectors, chaining, the re-hook after RESTOR, a call that arrives while a ROM routine runs, and contexts left over by
RUN/STOP+RESTORE. The whole thing runs in VICE in `make test-agent`; this covers the cases VICE cannot easily set up.
C128Test: the same engine with the C128's 128-mode KERNAL (docs/DESIGN.md 56) against a model of its MMU: two RAM banks, the ROMs of
the system bank, the common page at $0000-$03FF and the configuration register at $FF00."""
import ctypes, pathlib, shutil, struct, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
from d64py import LIB, Disk, VIO, VVec, R_CMD, R_STAT, WIN_SIZE

BASE = 0xDE08
C_VEC, C_CONT = 20, 21
OK = 0x5A
E = dict(INSTALL=0, OPEN=1, CLOSE=2, CHKIN=3, CKOUT=4, CLRCH=5, BASIN=6, BSOUT=7, CLALL=8, LOAD=9, SAVE=10, MAIN=11)
RTS, CTRAMP, ENTRIES, F_BSOUT, F_BASIN, F_GETIN, OLD = BASE + 241, BASE, BASE + 12, BASE + 48, BASE + 59, BASE + 70, BASE + 6
STOCK = {0x0302: 0xA483, 0x031A: 0xF34A, 0x031C: 0xF291, 0x031E: 0xF20E, 0x0320: 0xF250, 0x0322: 0xF333, 0x0324: 0xF157,
         0x0326: 0xF1CA, 0x032A: 0xF13E, 0x032C: 0xF32F, 0x0330: 0xF4A5, 0x0332: 0xF5ED}


class Agent:
    """runs the engine's scripts against a 64 KB memory image, as vector/vdagent.s does on the C64"""
    def __init__(self, vv, io):
        self.vv, self.io = vv, io
        self.mem = bytearray(65536)
        for a, v in STOCK.items():
            self.mem[a:a + 2] = struct.pack("<H", v)
            if a >= 0x0314: self.mem[0xFD30 + a - 0x0314:0xFD32 + a - 0x0314] = struct.pack("<H", v)   # RESTOR's table
        self.mem[0xE449:0xE44B] = struct.pack("<H", STOCK[0x0302])                                   # INITV's ($E447 = $0300)
        self.mem[0xFFD5:0xFFD8] = b"\x4C\x9E\xF4"                                                    # the jump table's LOAD
        self.zp = dict(st=0, nfiles=0, dfltn=0, dflto=3)

    def w16(self, a): return self.mem[a] | self.mem[a + 1] << 8
    def rd(self, a, n): return bytes(self.mem[a:a + n])                      # what the agent's PEEK / POKE see
    def wr(self, a, d): self.mem[a:a + len(d)] = d

    def send(self, cmd):
        self.io.w(R_CMD, cmd)
        assert self.vv.poll() == 1
        assert self.io.r(R_STAT) == OK
        return self.io.win_read(WIN_SIZE)

    def frame(self, lo, a=0, x=0, y=0, p=0x20):
        z = self.zp
        self.io.win_write(bytes([y, x, a, p, lo & 0xFF, z["st"], z["nfiles"], z["dfltn"], z["dflto"]]))
        return self.run(self.send(C_VEC))

    def call(self, entry, **regs): return self.frame(ENTRIES + 3 * E[entry] + 2, **regs)
    def result(self, **regs): return self.frame(CTRAMP + 5, **regs)

    def run(self, s):
        """execute ops until a GO; returns (target, a, x, y, p). A GO to ctramp is returned as is (the caller plays the routine)"""
        i = 0
        while True:
            op = s[i]; i += 1
            if op == 1:                                               # POKE lo hi n data (last byte first)
                a, n = s[i] | s[i + 1] << 8, s[i + 2]
                self.wr(a, bytes(reversed(s[i + 3:i + 3 + n]))); i += 3 + n
            elif op == 2:                                             # PEEK lo hi n -> window, C_CONT
                a, n = s[i] | s[i + 1] << 8, s[i + 2]
                self.io.win_write(self.rd(a, n)); s = self.send(C_CONT); i = 0
            elif op == 4:                                             # CONT
                s = self.send(C_CONT); i = 0
            else:
                assert op == 0, op
                y, p, a, x = s[i:i + 4]
                return (s[126] | s[127] << 8, a, x, y, p)


class VVecTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / "t.d64"
        subprocess.run(["c1541", "-format", "vvec,01", "d64", str(img)], capture_output=True, check=True)
        self.io = VIO()
        self.disk = Disk(img.read_bytes())
        self.io.attach(0, self.disk)
        self.io.attach(1, None, present=False)
        self.io.set_devices(8, 0)                                     # fixed: no bus scan in these tests
        self.vv = VVec(self.io, BASE)
        self.ag = Agent(self.vv, self.io)

    def tearDown(self): shutil.rmtree(self.tmp)

    def install(self):
        t = self.ag.call("INSTALL", a=1, x=2, y=3)
        self.assertEqual(t[:4], (RTS, 1, 2, 3))
        self.assertFalse(t[4] & 1, "C clear")

    def test_install_hooks_vectors_and_fast_paths(self):
        self.install()
        m = self.ag
        self.assertEqual(m.w16(0x031A), ENTRIES + 3 * E["OPEN"])
        self.assertEqual(m.w16(0x0330), ENTRIES + 3 * E["LOAD"])
        self.assertEqual(m.w16(0x0302), ENTRIES + 3 * E["MAIN"])
        self.assertEqual(m.w16(0x0326), F_BSOUT)
        self.assertEqual(m.w16(0x0324), F_BASIN)
        self.assertEqual(m.w16(0x032A), F_GETIN)
        self.assertEqual((m.w16(OLD), m.w16(OLD + 2), m.w16(OLD + 4)), (0xF1CA, 0xF157, 0xF13E), "the fast paths' old vectors")
        before = bytes(m.mem[0x0300:0x0334])
        self.install()                                                # again: nothing changes, the old vectors stay the stock ones
        self.assertEqual(bytes(m.mem[0x0300:0x0334]), before)
        self.assertEqual(m.call("CLRCH")[0], 0xF333, "CLRCHN with nothing of ours chains to the stock routine")

    def test_launch_install_only_over_the_roms_vectors(self):
        """the menu's PRG loader calls INSTALL with A = $A5: an autostart PRG that loaded over the vectors keeps its own"""
        m = self.ag
        m.mem[0x0326:0x0328] = struct.pack("<H", 0x02A7)            # the PRG's own CHROUT vector
        before = bytes(m.mem[0x0300:0x0334])
        t = m.call("INSTALL", a=0xA5)
        self.assertEqual((t[0], t[4] & 1), (RTS, 1), "refused, C set")
        self.assertEqual(bytes(m.mem[0x0300:0x0334]), before, "nothing changed")
        m.mem[0x0326:0x0328] = struct.pack("<H", 0xF1CA)
        m.mem[0x0302:0x0304] = struct.pack("<H", 0x1234)            # IMAIN changed (a BASIC wedge in the PRG)
        t = m.call("INSTALL", a=0xA5)
        self.assertEqual(t[4] & 1, 1)
        m.mem[0x0302:0x0304] = struct.pack("<H", STOCK[0x0302])
        t = m.call("INSTALL", a=0xA5)
        self.assertEqual((t[0], t[4] & 1), (RTS, 0), "the ROM's vectors: hooked")
        self.assertEqual(m.w16(0x0330), ENTRIES + 3 * E["LOAD"])

    def test_install_keeps_a_wedge_as_old_vector(self):
        self.ag.mem[0x0330:0x0332] = struct.pack("<H", 0xC123)       # a wedge installed before us
        self.install()
        self.ag.mem[0xBA] = 1; self.ag.mem[0xB7] = 1                  # the tape: not ours
        self.assertEqual(self.ag.call("LOAD", a=0)[0], 0xC123, "chains to the wedge, not the ROM")

    def test_chain_keeps_registers(self):
        self.install()
        self.ag.mem[0xBA] = 1; self.ag.mem[0xB7] = 1                  # the tape: not ours
        self.assertEqual(self.ag.call("LOAD", a=1, x=0x34, y=0x12, p=0x21), (0xF4A5, 1, 0x34, 0x12, 0x21))

    def test_main_rehooks_after_restor_only(self):
        self.install()
        m = self.ag
        for a, v in STOCK.items():                                    # RESTOR: $0314-$0333 back to the ROM's, $0302 stays ours
            if a != 0x0302: m.mem[a:a + 2] = struct.pack("<H", v)
        self.assertEqual(m.call("MAIN")[0], 0xA483)
        self.assertEqual(m.w16(0x0330), ENTRIES + 3 * E["LOAD"], "hooked again")
        m.mem[0x0330:0x0332] = struct.pack("<H", 0xC000)              # a wedge after us: left alone
        self.assertEqual(m.call("MAIN")[0], 0xA483)
        self.assertEqual(m.w16(0x0330), 0xC000)

    def test_call_during_rom_routine_and_not_found(self):
        """LOAD calls "SEARCHING FOR" (CALL); CLRCHN arrives meanwhile (nested); then the result; then FILE NOT FOUND"""
        self.install()
        m = self.ag
        name = b"NOPE"
        m.mem[0x2000:0x2004] = name
        m.mem[0xB7], m.mem[0xBA], m.mem[0xBB], m.mem[0xBC], m.mem[0xB9] = len(name), 8, 0x00, 0x20, 0
        t = m.call("LOAD", a=0)
        self.assertEqual(t[0], CTRAMP, "a CALL")
        self.assertEqual(m.w16(CTRAMP + 1), 0xF5AF, "of SEARCHING FOR")
        self.assertEqual(m.call("CLRCH")[0], 0xF333, "a call meanwhile is answered on its own")
        t = m.result()
        self.assertEqual(t[0], 0xF704, "FILE NOT FOUND")

    def test_load_and_save_round_trip(self):
        self.install()
        m = self.ag
        data = bytes(range(256)) * 3
        m.mem[0x4000:0x4000 + len(data)] = data
        m.mem[0x2000:0x2003] = b"BIN"
        m.mem[0xB7], m.mem[0xBA], m.mem[0xBB], m.mem[0xBC] = 3, 8, 0x00, 0x20
        m.mem[0xC1:0xC3] = b"\x00\x40"; m.mem[0xAE:0xB0] = struct.pack("<H", 0x4000 + len(data))
        t = m.call("SAVE")
        self.assertEqual((t[0], m.w16(CTRAMP + 1)), (CTRAMP, 0xF68F), "SAVING")
        self.assertEqual(m.result()[0], RTS)
        m.mem[0x4000:0x5000] = bytes(0x1000)
        m.mem[0xB9] = 1                                               # LOAD"BIN",8,1: the file's own address
        t = m.call("LOAD", a=0)
        self.assertEqual(m.w16(CTRAMP + 1), 0xF5AF)
        t = m.result()
        self.assertEqual((t[0], m.w16(CTRAMP + 1)), (CTRAMP, 0xF5D2), "LOADING")
        t = m.result()
        end = 0x4000 + len(data)
        self.assertEqual(t[:4], (RTS, 0, end & 0xFF, end >> 8), "X/Y = the end address")
        self.assertEqual(bytes(m.mem[0x4000:end]), data)
        self.assertEqual(m.mem[0x90], 0x40)
        # VERIFY against a changed byte: ST bit 4, memory untouched
        m.mem[0x4100] ^= 0xFF
        m.call("LOAD", a=1); m.result(); m.result()
        self.assertEqual(m.mem[0x90] & 0x10, 0x10)
        self.assertNotEqual(m.mem[0x4100], data[0x100])

    def test_leftover_contexts_are_dropped(self):
        """RUN/STOP+RESTORE in the middle of a LOAD: the engine waits for PEEK data that never comes; the next call starts afresh"""
        self.install()
        m = self.ag
        m.mem[0xB7], m.mem[0xBA] = 1, 8
        m.io.win_write(bytes([0, 0, 0, 0x20, (ENTRIES + 3 * E["LOAD"] + 2) & 0xFF, 0, 0, 0, 3]))
        s = m.send(C_VEC)
        self.assertEqual(s[0], 2, "the LOAD asks for memory first (PEEK)")
        self.assertEqual(m.call("BASIN")[0], 0xF157, "a new call is answered (keyboard: chained)")
        t = m.run(m.send(C_CONT))                                    # a CONT nobody waits for: back to the caller, C set
        self.assertEqual((t[0], t[1], t[4] & 1), (RTS, 5, 1))


# ---- the C128 in 128 mode (docs/DESIGN.md 56) ----
STOCK128 = {0x0302: 0x4DC6, 0x031A: 0xEFBD, 0x031C: 0xF188, 0x031E: 0xF106, 0x0320: 0xF14C, 0x0322: 0xF226, 0x0324: 0xEF06,
            0x0326: 0xEF79, 0x032A: 0xEEEB, 0x032C: 0xF222, 0x0330: 0xF26C, 0x0332: 0xF54E,
            0x0314: 0xFA65, 0x0316: 0xB003, 0x0318: 0xFA40, 0x0328: 0xF66E, 0x032E: 0xB006,   # (these five: RESTOR's, not hooked)
            0x0A00: 0x4003}         # BASIC 7's warm start: the re-hook entry's vector (IMAIN is called with bank 14 in, no I/O)
LAT128 = 0x0362                                                       # LAT, FAT $036C, SAT $0376
ERR128 = lambda n: 0xF67C + 3 * (n - 1)


class Agent128(Agent):
    """the agent on a C128 in 128 mode: what a PEEK / POKE reaches depends on the MMU's configuration register at $FF00"""
    def __init__(self, vv, io):
        self.ram = [bytearray(65536), bytearray(65536)]
        self.rom = bytearray(b"\xEA" * 65536)                       # BASIC 7 at $4000-$BFFF, editor and KERNAL at $C000-$FFFF
        self.io_page = bytearray(4096)
        self.cr = 0x00                                                # the system bank: ROMs and I/O in, RAM bank 0 below $4000
        self.mem = self.ram[0]                                        # Agent's helpers (w16, the zero page) see bank 0 / the common page
        self.vv, self.io = vv, io
        for a, v in STOCK128.items():
            self.mem[a:a + 2] = struct.pack("<H", v)
            if 0x0314 <= a < 0x0334: self.rom[0xE073 + a - 0x0314:0xE075 + a - 0x0314] = struct.pack("<H", v)   # RESTOR's table
        self.rom[0x4269:0x426B] = struct.pack("<H", STOCK128[0x0302])                              # BASIC 7's init table ($4267 = $0300)
        self.rom[0xFFD5:0xFFD8] = b"\x4C\x65\xF2"                                                 # the jump table's LOAD: JMP $F265
        self.zp = dict(st=0, nfiles=0, dfltn=0, dflto=3)
        self.switches = []                                            # every value written to $FF00

    def w16(self, a): return struct.unpack("<H", self.rd(a, 2))[0]

    def where(self, a, write):
        """(array, index) a CPU access at `a` reaches under the current configuration"""
        cr = self.cr
        if a < 0x0400: return self.ram[0], a                          # the common page
        if 0xD000 <= a < 0xE000 and not cr & 1: return self.io_page, a - 0xD000
        rom = (a < 0x8000 and not cr & 0x02) if a >= 0x4000 else False
        rom = rom or (0x8000 <= a < 0xC000 and not cr & 0x0C) or (a >= 0xC000 and not cr & 0x30)
        if rom and not write: return self.rom, a                      # a write to a ROM's area goes to the RAM below it
        return self.ram[(cr >> 6) & 1], a

    def rd(self, a, n):
        out = bytearray()
        for k in range(a, a + n):
            if k == 0xFF00: out.append(self.cr); continue
            arr, i = self.where(k, False); out.append(arr[i])
        return bytes(out)

    def wr(self, a, d):
        for k, b in enumerate(d):                                     # the agent stores from the top down: the order does not matter
            if a + k == 0xFF00: self.cr = b; self.switches.append(b); continue
            arr, i = self.where(a + k, True); arr[i] = b


class C128Test(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / "t.d64"
        subprocess.run(["c1541", "-format", "vvec,01", "d64", str(img)], capture_output=True, check=True)
        self.io = VIO()
        self.io.attach(0, Disk(img.read_bytes()))
        self.io.attach(1, None, present=False)
        self.io.set_devices(8, 0)
        self.vv = VVec(self.io, BASE, VVec.C128)
        self.ag = Agent128(self.vv, self.io)

    def tearDown(self): shutil.rmtree(self.tmp)

    def install(self):
        t = self.ag.call("INSTALL", a=1, x=2, y=3)
        self.assertEqual(t[:4], (RTS, 1, 2, 3))
        self.assertFalse(t[4] & 1)

    def name(self, text, bank=1, at=0xF000):
        """SETNAM + SETBNK as BASIC 7 does it: the name in bank `bank`"""
        m = self.ag
        m.ram[bank][at:at + len(text)] = text
        m.mem[0xB7], m.mem[0xBB], m.mem[0xBC], m.mem[0xC7] = len(text), at & 0xFF, at >> 8, bank

    def test_install_and_the_c128s_vectors(self):
        m = self.ag
        self.assertEqual(m.call("MAIN")[0], 0x4003, "before INSTALL: BASIC 7's warm start")
        self.install()
        self.assertEqual(m.w16(0x0330), ENTRIES + 3 * E["LOAD"])
        self.assertEqual(m.w16(0x0A00), ENTRIES + 3 * E["MAIN"], "the re-hook entry on $0A00")
        self.assertEqual(m.w16(0x0302), 0x4DC6, "IMAIN left to BASIC 7")
        before = bytes(m.mem[0x0300:0x0334]) + bytes(m.mem[0x0A00:0x0A02])
        self.install()
        self.assertEqual(bytes(m.mem[0x0300:0x0334]) + bytes(m.mem[0x0A00:0x0A02]), before, "a second INSTALL changes nothing")
        self.assertEqual((m.w16(OLD), m.w16(OLD + 2), m.w16(OLD + 4)), (0xEF79, 0xEF06, 0xEEEB), "the C128's CHROUT, CHRIN, GETIN")
        self.assertEqual(m.call("CLRCH")[0], 0xF226)
        self.assertEqual(m.switches, [], "no bank switch for calls that read no memory of the caller's")

    def test_install_tells_the_machine_by_itself(self):
        """the firmware serves one agent for both modes: INSTALL reads the KERNAL's jump table and picks the profile"""
        vv = VVec(self.io, BASE)                                      # a C64 engine, as after vvec_init alone
        m = Agent128(vv, self.io)
        self.assertEqual(m.call("MAIN")[0], 0xA483, "before INSTALL: the C64's numbers")
        t = m.call("INSTALL", a=0xA5)
        self.assertEqual((t[0], t[4] & 1), (RTS, 0), "the C128's ROM tables matched")
        self.assertEqual((m.w16(OLD), m.w16(OLD + 2)), (0xEF79, 0xEF06))
        c64 = Agent(vv, self.io)                                      # GO64 / a reset into C64 mode: the same engine, back to the C64
        c64.call("INSTALL", a=0xA5)
        self.assertEqual(c64.w16(OLD), 0xF1CA)

    def test_launch_install_against_the_c128_roms_tables(self):
        m = self.ag
        m.mem[0x0A00:0x0A02] = struct.pack("<H", 0x1300)             # the program's own warm start
        self.assertEqual(m.call("INSTALL", a=0xA5)[4] & 1, 1, "refused")
        m.mem[0x0A00:0x0A02] = struct.pack("<H", 0x4003)
        m.mem[0x0330:0x0332] = struct.pack("<H", 0x1300)             # the program's own LOAD
        self.assertEqual(m.call("INSTALL", a=0xA5)[4] & 1, 1, "refused")
        m.mem[0x0330:0x0332] = struct.pack("<H", STOCK128[0x0330])
        t = m.call("INSTALL", a=0xA5)
        self.assertEqual((t[0], t[4] & 1), (RTS, 0), "RESTOR's table ($E073) and BASIC 7's warm start: hooked")
        self.assertEqual(m.w16(0x031A), ENTRIES + 3 * E["OPEN"])

    def test_restor_then_warm_start_hooks_again(self):
        """RUN/STOP+RESTORE: the NMI routine runs RESTOR, then JMP ($0A00), which RESTOR leaves to us"""
        self.install()
        m = self.ag
        for a, v in STOCK128.items():
            if 0x0314 <= a < 0x0334: m.mem[a:a + 2] = struct.pack("<H", v)
        self.assertEqual(m.call("MAIN")[0], 0x4003, "on to BASIC 7's warm start")
        self.assertEqual(m.w16(0x0330), ENTRIES + 3 * E["LOAD"], "hooked again")
        self.assertEqual(m.w16(0x0A00), ENTRIES + 3 * E["MAIN"])
        self.assertEqual(m.w16(OLD), 0xEF79, "the fast paths' old vectors: still the ROM's, not ours")

    def test_save_load_verify_in_bank_1(self):
        """name and data in bank 1 (BASIC 7 keeps strings there; BLOAD / BSAVE take a bank); bank 0 under them stays as it was"""
        self.install()
        m = self.ag
        data = bytes((i * 7) & 0xFF for i in range(700))
        m.ram[1][0x5000:0x5000 + len(data)] = data
        m.ram[0][0x5000:0x6000] = b"\x11" * 0x1000
        self.name(b"DATA")
        m.mem[0xBA], m.mem[0xC6] = 8, 1
        m.mem[0xC1:0xC3] = b"\x00\x50"; m.mem[0xAE:0xB0] = struct.pack("<H", 0x5000 + len(data))
        t = m.call("SAVE")
        self.assertEqual((t[0], m.w16(CTRAMP + 1)), (CTRAMP, 0xF5BC), "SAVING (the C128's)")
        self.assertEqual(m.result()[0], RTS)
        self.assertEqual(m.cr, 0x00, "the caller's configuration back")
        self.assertIn(0x7E, m.switches, "bank 1 switched in (with I/O) to read it")
        m.ram[1][0x5000:0x6000] = bytes(0x1000)
        m.mem[0xB9] = 1
        t = m.call("LOAD", a=0)
        self.assertEqual(m.w16(CTRAMP + 1), 0xF50F, "SEARCHING FOR (the C128's)")
        t = m.result()
        self.assertEqual(m.w16(CTRAMP + 1), 0xFC4A, "LOADING (the C128's, which copies $AE/$AF to $AC/$AD)")
        self.assertEqual(m.w16(0xAE), 0x5000, "$AE/$AF = the start for it")
        t = m.result()
        end = 0x5000 + len(data)
        self.assertEqual(t[:4], (RTS, 0, end & 0xFF, end >> 8))
        self.assertEqual(bytes(m.ram[1][0x5000:end]), data, "loaded into bank 1")
        self.assertEqual(bytes(m.ram[0][0x5000:0x6000]), b"\x11" * 0x1000, "bank 0 untouched")
        self.assertEqual((m.cr, m.mem[0x90]), (0x00, 0x40))
        m.ram[1][0x5123] ^= 0xFF
        m.call("LOAD", a=1); m.result(); m.result()
        self.assertEqual(m.mem[0x90] & 0x10, 0x10, "VERIFY ERROR in bank 1")
        self.assertEqual(m.cr, 0x00)

    def test_save_from_bank_0_under_the_roms(self):
        """bank 0 above $4000 is ROM in the system bank: the engine must read the RAM, not BASIC 7"""
        self.install()
        m = self.ag
        data = bytes(range(256)) * 2
        m.ram[0][0x9000:0x9200] = data
        self.name(b"LOW", bank=0, at=0x1300)
        m.mem[0xBA], m.mem[0xC6] = 8, 0
        m.mem[0xC1:0xC3] = b"\x00\x90"; m.mem[0xAE:0xB0] = b"\x00\x92"
        m.call("SAVE"); m.result()
        m.ram[0][0x9000:0x9200] = bytes(0x200)
        m.mem[0xB9] = 1
        m.call("LOAD", a=0); m.result(); m.result()
        self.assertEqual(bytes(m.ram[0][0x9000:0x9200]), data)
        self.assertEqual(m.cr, 0x00)

    def test_caller_configuration_is_restored(self):
        """a program running with BASIC's ROMs switched out ($0E) gets $0E back, not the system bank"""
        self.install()
        m = self.ag
        m.cr = 0x0E
        m.ram[1][0x2000:0x2010] = bytes(range(16))
        self.name(b"CFG")
        m.mem[0xBA], m.mem[0xC6] = 8, 1
        m.mem[0xC1:0xC3] = b"\x00\x20"; m.mem[0xAE:0xB0] = b"\x10\x20"
        m.call("SAVE"); m.result()
        self.assertEqual(m.cr, 0x0E)
        self.assertEqual(m.switches[-1], 0x0E)

    def test_file_table_and_close_with_carry(self):
        """OPEN / CHKIN / CLOSE use the C128's file table at $0362; CLOSE with C set leaves channel 15 open on the drive (DCLOSE)"""
        self.install()
        m = self.ag
        self.name(b"I0", bank=0, at=0x1300)
        m.mem[0xB8], m.mem[0xB9], m.mem[0xBA] = 15, 15, 8
        t = m.call("OPEN")
        self.assertEqual((t[0], m.w16(CTRAMP + 1)), (CTRAMP, 0xEFBD), "the stock OPEN fills the table")
        m.mem[LAT128], m.mem[LAT128 + 10], m.mem[LAT128 + 20] = 15, 8, 0x6F    # what it does
        m.zp["nfiles"] = 1
        self.assertEqual(m.result()[0], RTS)
        t = m.call("CHKIN", x=15)
        self.assertEqual(t[:2], (RTS, 8), "A = the device")
        self.assertEqual((m.mem[0x99], m.mem[0xBA], m.mem[0xB9]), (8, 8, 0x6F))
        t = m.call("CLOSE", a=15, p=0x21)
        self.assertEqual(t[0], 0xF188, "chained")
        self.assertEqual(m.mem[LAT128 + 20], 0x6F, "C set: the SA keeps bit 7 clear, as the stock routine sends nothing then")
        t = m.call("CLOSE", a=15, p=0x20)
        self.assertEqual(m.mem[LAT128 + 20], 0xEF, "C clear: closed on the drive, the stock routine only drops the entry")

    def test_not_found_uses_the_c128s_error_exit(self):
        self.install()
        m = self.ag
        self.name(b"NOPE")
        m.mem[0xBA], m.mem[0xB9] = 8, 0
        m.call("LOAD", a=0)
        self.assertEqual(m.result()[0], ERR128(4), "FILE NOT FOUND")
        self.assertEqual(m.cr, 0x00)


if __name__ == "__main__":
    unittest.main()
