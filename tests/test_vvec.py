"""The vector engine (d64/vdrive_vec.c) against a Python stand-in for the agent (vector/vdagent.s): the script ops, INSTALL and its
fast-path vectors, chaining, the re-hook after RESTOR, a call that arrives while a ROM routine runs, and contexts left over by
RUN/STOP+RESTORE. The whole thing runs in VICE in `make test-agent`; this covers the cases VICE cannot easily set up."""
import ctypes, pathlib, shutil, struct, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
from d64py import LIB, Disk, VIO, VVec, R_CMD, R_STAT, WIN_SIZE

BASE = 0xDE08
C_VEC, C_CONT = 18, 19
OK = 0x5A
E = dict(INSTALL=0, OPEN=1, CLOSE=2, CHKIN=3, CKOUT=4, CLRCH=5, BASIN=6, BSOUT=7, CLALL=8, LOAD=9, SAVE=10, MAIN=11)
RTS, CTRAMP, ENTRIES, F_BSOUT, F_BASIN, F_GETIN, OLD = BASE + 241, BASE, BASE + 12, BASE + 48, BASE + 59, BASE + 70, BASE + 6
HOOKS = 0xDF10                                                        # vector/vdhooks.s on hardware (MBX + $10)
H_OLD_IRQ, H_OLD_KEYLOG, H_SEI, H_IRQ, H_KEY = HOOKS, HOOKS + 2, HOOKS + 4, HOOKS + 7, HOOKS + 44
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
        self.mem[0x0314:0x0316] = struct.pack("<H", 0xEA31)                                         # CINV, as RESTOR sets it
        self.mem[0x028F:0x0291] = struct.pack("<H", 0xEB48)                                         # KEYLOG, as CINT sets it
        self.zp = dict(st=0, nfiles=0, dfltn=0, dflto=3)

    def w16(self, a): return self.mem[a] | self.mem[a + 1] << 8

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
                self.mem[a:a + n] = bytes(reversed(s[i + 3:i + 3 + n])); i += 3 + n
            elif op == 2:                                             # PEEK lo hi n -> window, C_CONT
                a, n = s[i] | s[i + 1] << 8, s[i + 2]
                self.io.win_write(bytes(self.mem[a:a + n])); s = self.send(C_CONT); i = 0
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

    def hooked(self):
        """INSTALL with the hook block: KEYLOG is PEEKed, the block's SEI is CALLed, then everything is written and the caller's P comes back"""
        m = self.ag
        t = m.call("INSTALL", a=1, x=2, y=3, p=0x20)
        self.assertEqual((t[0], m.w16(CTRAMP + 1)), (CTRAMP, H_SEI), "a CALL of the hook block's SEI first")
        t = m.result(a=1, x=2, y=3, p=0x24)                           # back from SEI: I set
        self.assertEqual(t[:4], (RTS, 1, 2, 3))
        self.assertEqual(t[4] & 0x05, 0, "I and C clear again: the caller's own P")
        return t

    def test_hooks_install(self):
        self.vv.set_hooks(HOOKS)
        m = self.ag
        m.mem[0x0314:0x0316] = struct.pack("<H", 0xC000)             # a program's own IRQ routine before us: chained to
        self.hooked()
        self.assertEqual((m.w16(0x0314), m.w16(0x028F)), (H_IRQ, H_KEY))
        self.assertEqual((m.w16(H_OLD_IRQ), m.w16(H_OLD_KEYLOG)), (0xC000, 0xEB48))
        self.assertEqual(m.w16(0x0330), ENTRIES + 3 * E["LOAD"], "and the other vectors as before")
        self.hooked()                                                 # again: the old values stay (not our own hooks)
        self.assertEqual((m.w16(H_OLD_IRQ), m.w16(H_OLD_KEYLOG)), (0xC000, 0xEB48))

    def test_hooks_without_block(self):
        """no hook block (the default): neither vector is touched and INSTALL needs no CALL"""
        self.install()
        self.assertEqual((self.ag.w16(0x0314), self.ag.w16(0x028F)), (0xEA31, 0xEB48))

    def test_hooks_main_rehooks_after_run_stop_restore(self):
        self.vv.set_hooks(HOOKS)
        m = self.ag
        self.hooked()
        for a, v in STOCK.items():                                    # RESTOR (CINV too) and CINT (KEYLOG)
            if a != 0x0302: m.mem[a:a + 2] = struct.pack("<H", v)
        m.mem[0x0314:0x0316] = struct.pack("<H", 0xEA31); m.mem[0x028F:0x0291] = struct.pack("<H", 0xEB48)
        t = m.call("MAIN", p=0x20)
        self.assertEqual((t[0], m.w16(CTRAMP + 1)), (CTRAMP, H_SEI))
        t = m.result(p=0x24)
        self.assertEqual((t[0], t[4] & 0x04), (0xA483, 0), "chained to BASIC's loop with the caller's I flag")
        self.assertEqual((m.w16(0x0314), m.w16(0x028F), m.w16(0x0330)), (H_IRQ, H_KEY, ENTRIES + 3 * E["LOAD"]))
        self.assertEqual((m.w16(H_OLD_IRQ), m.w16(H_OLD_KEYLOG)), (0xEA31, 0xEB48))
        self.assertEqual(m.call("MAIN")[0], 0xA483, "nothing to do: no CALL")

    def test_launch_install_refused_keeps_hooks_off(self):
        self.vv.set_hooks(HOOKS)
        m = self.ag
        m.mem[0x0326:0x0328] = struct.pack("<H", 0x02A7)            # an autostart PRG's vector: refused
        t = m.call("INSTALL", a=0xA5, p=0x20)
        self.assertEqual(t[0], CTRAMP)
        t = m.result(a=0xA5, p=0x24)
        self.assertEqual((t[0], t[4] & 0x05), (RTS, 1), "refused (C set), I clear again")
        self.assertEqual((m.w16(0x0314), m.w16(0x028F)), (0xEA31, 0xEB48))


if __name__ == "__main__":
    unittest.main()
