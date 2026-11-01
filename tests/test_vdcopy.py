#!/usr/bin/env python3
"""VDCOPY (util/vdcopy.s, machine code) run in py65 (tests/c64_sim.py answers its KERNAL calls) against the real drive code.

Both units are d64/vdrive.c through libd64: unit 8 plays the real floppy drive (the program only knows the vdrive units from $DE16/$DE17,
and those say 9), unit 9 is the SD card with a host folder behind it (CD: mounts, N:NAME.D64 creates, "!STATE" gives the TR menu's
colours). The real 1541's side of the sector copy (U1/U2/B-P on a true drive) runs in VICE (tools/vdrive_mock.py scan); this tests the
program's logic, its screens and the bytes it moves."""
import ctypes, pathlib, subprocess, sys, tempfile, unittest

try:
    import py65  # noqa: F401
except ImportError:
    print("SKIP py65 not installed (pip install py65): VDCOPY not run"); sys.exit(0)

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests")); sys.path.insert(0, str(ROOT / "tools"))
from c64_sim import C64
from d64py import LIB, Disk, VDrive
from test_vio import HostStruct
import make_util, suite_files

with tempfile.TemporaryDirectory() as _t:
    PRG = make_util.build(_t).read_bytes()

D64 = 174848
SD, FLOPPY = 9, 8


def c1541(*a): subprocess.run(["c1541", *a], check=True, capture_output=True)


def image(path, name, files=()):
    c1541("-format", name, "d64", str(path))
    for i, (fn, data) in enumerate(files):
        src = path.parent / ("f%d.prg" % i); src.write_bytes(data)
        c1541("-attach", str(path), "-write", str(src), fn)
    return path.read_bytes()


class Bus:
    """The KERNAL's view of the serial bus for c64_sim: each logical file maps to LISTEN/TALK on the vdrive mailbox."""
    def __init__(self, vd):
        self.vd, self.files, self.st = vd, {}, 0
        self.units = {FLOPPY, SD}

    def open(self, lf, dev, sa, name):
        self.files[lf] = (dev, sa); self.vd.listen(dev, 0xF0 | sa, name.encode("latin1"))

    def close(self, lf):
        dev, sa = self.files.pop(lf); self.vd.listen(dev, 0xE0 | sa)

    def put(self, lf, text):
        dev, sa = self.files[lf]; self.vd.listen(dev, 0x60 | sa, text.encode("latin1"))

    def getline(self, lf):
        dev, _ = self.files[lf]
        return self.vd.talk(dev, 0x6F)[0].decode("latin1").rstrip("\r")

    def get(self, lf):
        dev, sa = self.files[lf]
        data, st = self.vd.talk(dev, 0x60 | sa, n=1)
        self.st = st
        return data[0] if data else 13


class T(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.card = self.tmp / "card"; self.card.mkdir()
        self.game = image(self.card / "game.d64", "game disk,g1", [("hello", b"\x01\x08" + bytes(range(256)) * 3), ("more", bytes(5000))])
        self.floppy = Disk(image(self.tmp / "floppy.d64", "old floppy,f1", [("old", bytes(300))]))
        self.colours = None
        self.disks = {}
        self.vd = VDrive()
        LIB.vdrive_set_devices(self.vd.v, ctypes.c_uint8(FLOPPY), ctypes.c_uint8(SD))
        cb = self.cb = HostStruct()

        def mount(ctx, unit, name, out):
            p = self.card / name.decode().lower()
            if not p.is_file(): return 62
            d = self.disks[p] = Disk(p.read_bytes()); out[0] = ctypes.addressof(d.d); return 0
        def create(ctx, unit, name, size, out):
            p = self.card / name.decode().lower()
            if p.exists(): return 63
            p.write_bytes(bytes(size)); return mount(ctx, unit, name, out)
        self.state = b""
        def hf_open(ctx, unit, kind, write):
            if kind != 1: return 62
            self.state = b"\r/\r" + (self.colours.encode() + b"\r" if self.colours else b""); return 0
        def hf_read(ctx, unit, buf, n):
            part, self.state = self.state[:n], self.state[n:]
            ctypes.memmove(buf, part, len(part)); return len(part)
        def chdir(ctx, unit, name):                 # only the card's root here
            return 0 if name.decode() in ("/", "..") else 62
        cb.chdir = HostStruct.CHDIR(chdir)
        cb.mount = HostStruct.MOUNT(mount); cb.create = HostStruct.CREATE(create)
        cb.unmount = HostStruct.UNMOUNT(lambda c, u: None)
        cb.hf_open = HostStruct.HFOPEN(hf_open); cb.hf_read = HostStruct.HFREAD(hf_read)
        cb.hf_close = HostStruct.HFCLOSE(lambda c, u, w: None)
        LIB.vdrive_set_host(self.vd.v, ctypes.byref(cb))
        self.vd.attach(0, self.floppy)
        self.vd.attach(1, None)

    def start(self, *keys, mem=None):
        self.b = C64(PRG, Bus(self.vd), {56854: SD, 56855: 0, **(mem or {})})
        self.b.press(*keys)
        return self.b.run(max_steps=60_000_000)

    def keys(self, *keys):
        self.b.press(*keys)
        return self.b.run(max_steps=60_000_000)

    def row(self, r): return self.b.screen.row(r)
    def text(self): return self.b.screen.text()
    def cell(self, t, s): return self.b.mem[0x0400 + (4 + s) * 40 + 3 + t], self.b.mem[0xD800 + (4 + s) * 40 + 3 + t]
    def sd(self, name):
        """an image on the card as the drive sees it (a mounted image is changed in memory, not written back here)"""
        d = self.disks.get(self.card / name)
        return d.dump() if d else (self.card / name).read_bytes()

    def test_menu(self):
        self.assertEqual(self.start(), "key")
        t = self.text()
        self.assertIn("VDCOPY", self.row(0))
        self.assertIn("VDRIVE: SD UNIT 9", t)
        self.assertIn(" F  COPY FILES", t)
        self.assertIn(" D  COPY A WHOLE DISK, SECTOR BY SECTOR", t)
        self.assertEqual(self.keys("Q"), "end")

    def test_theme_defaults_and_from_state(self):
        self.start()
        self.assertEqual((self.b.mem[53280], self.b.mem[53281]), (4, 0))
        self.assertEqual({self.b.screen.col[0][c] for c in range(39)}, {4})
        self.assertTrue(all(self.b.screen.rev[0][c] for c in range(39)))
        self.assertEqual((self.b.mem[0x0427], self.b.mem[0xD827]), (0xA0, 4), "the banner's last column")
        self.colours = "6E1F3C5"                    # background 6, border 14, banner 1, time 15, option 3, sources 12, name 5
        self.start(mem={53280: 2, 53281: 11, 0x0286: 1})
        self.assertEqual((self.b.mem[53280], self.b.mem[53281]), (14, 6))
        r = next(r for r in range(25) if "COPY FILES" in self.row(r))
        c = self.row(r).index("F")
        self.assertEqual((self.b.screen.col[r][c], self.b.screen.rev[r][c]), (3, True))      # a key: reverse, option colour
        self.assertEqual(self.b.screen.col[r][self.row(r).index("COPY")], 12)              # what it does: sources colour
        self.assertEqual(self.keys("Q"), "end")
        self.assertEqual((self.b.mem[53280], self.b.mem[53281], self.b.mem[0x0286]), (2, 11, 1), "Q gives back the colours found at the start")

    def test_image_to_floppy_with_format(self):
        self.assertEqual(self.start("D", "9\r", "8\r", "GAME.D64\r"), "key")
        t = self.text()
        self.assertIn('DISK "GAME DISK" ID G1', t)
        self.assertIn("THE DISK IN UNIT 8 WILL BE OVERWRITTEN.", t)
        self.assertEqual(self.keys("F"), "key")
        self.assertIn("DONE, 0 READ ERRORS", self.row(1))
        self.assertIn("PRESS A KEY", self.row(0))
        self.assertEqual(self.floppy.dump(), self.game, "the floppy is the image, byte for byte")
        self.assertEqual(self.cell(1, 0), (0x51, 13))       # done: a ball in the name colour
        self.assertEqual(self.cell(35, 16)[0], 0x51)
        self.assertNotIn(self.cell(35, 17)[0], (0x51, 0x2E), "track 35 has 17 sectors")
        self.assertEqual(self.cell(18, 18)[0], 0x51)
        self.assertNotIn(self.cell(18, 19)[0], (0x51, 0x2E))
        self.assertEqual(self.vd.status(FLOPPY)[:2], "00")
        self.assertEqual(self.keys(" "), "key")
        self.assertIn("COPY FILES", self.text(), "back in the menu")

    def test_floppy_to_new_image(self):
        want = self.floppy.dump()
        self.assertEqual(self.start("D", "8\r", "9\r", "GAME\r"), "key")
        self.assertIn("ERROR 63,FILE EXISTS", self.text(), "GAME.D64 is there: the N: error is shown, nothing is written")
        self.assertEqual((self.card / "game.d64").read_bytes(), self.game)
        self.keys(" ")
        self.assertEqual(self.keys("D", "8\r", "9\r", "COPY\r"), "key")
        self.assertIn("DONE, 0 READ ERRORS", self.row(1))
        self.assertTrue((self.card / "copy.d64").exists(), ".D64 added to the name")
        self.assertEqual(self.sd("copy.d64"), want)

    def test_overwrite_mounted_image(self):
        self.vd.listen(SD, 0x6F, b"CD:GAME.D64")
        want = self.floppy.dump()
        self.assertEqual(self.start("D", "8\r", "9\r", "\r"), "key")
        self.assertIn("WRITE OVER THE IMAGE ON UNIT 9 (Y/N)?", self.text())
        self.keys("N")
        self.assertIn("COPY FILES", self.text(), "N: back to the menu")
        self.assertEqual(self.sd("game.d64"), self.game)
        self.keys("D", "8\r", "9\r", "\r", "Y")
        self.assertIn("DONE", self.row(1))
        self.assertEqual(self.sd("game.d64"), want)

    def test_read_errors_counted_and_copied(self):
        bt, bs = suite_files.bad_image(self.card / "bad.d64")
        bad = (self.card / "bad.d64").read_bytes()
        self.assertEqual(self.start("D", "9\r", "8\r", "BAD.D64\r", "W"), "key")
        self.assertIn("DONE, 2 READ ERRORS", self.row(1))
        self.assertEqual(self.floppy.dump(), bad[:D64], "the sectors' data copied as the drive gave it")
        self.assertEqual(self.cell(bt, bs), (0xB3, 10), "error 23: a reverse 3 in the time colour")
        self.assertEqual(self.cell(*suite_files.BAD_FREE), (0xB1, 10))

    def test_write_protected_destination_stops(self):
        self.vd.listen(FLOPPY, 0x6F, b"W-1")
        self.assertEqual(self.start("D", "9\r", "8\r", "GAME.D64\r", "W"), "key")
        self.assertIn("26,WRITE PROTECT ON AT 1/0", self.row(1))
        self.assertEqual(self.cell(1, 0)[0], 0x2B, "the sector being copied")
        self.assertEqual(self.cell(1, 1)[0], 0x2E)

    def test_no_image_mounted(self):
        self.assertEqual(self.start("D", "9\r", "8\r", "\r"), "key")
        self.assertIn("ERROR 74,DRIVE NOT READY", self.text())

    def test_not_a_1541_disk(self):
        c1541("-format", "two,22", "d71", str(self.card / "two.d71"))
        self.assertEqual(self.start("D", "9\r", "8\r", "TWO.D71\r"), "key")
        self.assertIn("DOES NOT LOOK LIKE A 1541 DISK", self.text())
        self.keys("N")
        self.assertIn("COPY FILES", self.text())

    def test_cancel_real_destination(self):
        want = self.floppy.dump()
        self.start("D", "9\r", "8\r", "GAME.D64\r", "Q")
        self.assertIn("COPY FILES", self.text())
        self.assertEqual(self.floppy.dump(), want)

    def test_files_mode_still_copies(self):
        self.vd.listen(SD, 0x6F, b"CD:GAME.D64")
        self.assertEqual(self.start("F", "9\r", "8\r"), "key")
        self.assertIn('"HELLO"', self.text())
        self.keys("A\r", "N")
        self.assertIn("2 COPIED, 0 SKIPPED, 0 FAILED", self.text())
        self.assertEqual(self.floppy.read_file(b"HELLO"), b"\x01\x08" + bytes(range(256)) * 3)
        self.keys("\r")
        self.assertIn("COPY FILES", self.text(), "RETURN at the prompt: back to the menu")


if __name__ == "__main__":
    unittest.main()
