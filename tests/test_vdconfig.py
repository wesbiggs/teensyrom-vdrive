#!/usr/bin/env python3
"""util/vdconfig.bas (VDrive Config.prg) run in the small BASIC interpreter (tests/basic_interp.py) against a model of the drive.

The model follows d64/vdrive.c and firmware/IOH_VDrive.c: "!CONFIG[,W]" / "!STATE" host text files, "$path" listings by path (they do not change
what is mounted), "CD:" with paths, the error channel, and the rule that closing channel 15 closes every channel of the unit. The real channel
code is tested in tests/fw_host; this tests the program's logic, screen layout and keystrokes."""
import pathlib, re, sys, unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from basic_interp import Basic, BasicError

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = (ROOT / "util" / "vdconfig.bas").read_text()
UP, DOWN, RET, DEL, LEFTARROW = 145, 17, 13, 20, 95
IMG = b"d64"


class Chan:
    def __init__(self, kind, dev, data=b"", name=""):
        self.kind, self.dev, self.data, self.pos, self.name, self.out = kind, dev, data, 0, name, b""


class Unit:
    def __init__(self, title, files):
        self.title, self.files = title, dict(files)       # files: lower-case absolute path -> bytes, or None for a folder
        self.mounted = None
        self.cwd = ""
        self.status = "00, OK,00,00"


class FakeDrive:
    def __init__(self, sd, usb=None):
        self.units = {8: Unit("SD", sd)}
        if usb is not None: self.units[9] = Unit("USB", usb)
        self.chans, self.st = {}, 0
        self.log = []

    # ---- host side helpers ----
    def unit(self, dev): return self.units[dev]

    def children(self, u, folder):
        folder = folder.rstrip("/")
        out = []
        for p, v in sorted(u.files.items()):
            par, _, name = p.rpartition("/")
            if par == folder and name and not name.startswith("."): out.append((name, v))
        return out

    @staticmethod
    def bas_line(blocks, text):
        return bytes([0x01, 0x04, blocks & 255, blocks >> 8]) + text + b"\0"

    def listing(self, u, folder):
        out = bytes([1, 4])
        out += self.bas_line(0, b"\x12\"" + (u.title + ":" + (folder or "/")).upper().encode().ljust(16)[:16] + b"\" 08 2A")
        for name, v in self.children(u, folder):
            n = name.upper()
            if len(n) > 16: continue
            kind = "DIR" if v is None else (n[-3:] if n[-4:-3] == "." and n[-3:] in ("D64", "D71", "D81") else "PRG")
            blocks = 0 if v is None else (len(v) + 253) // 254
            out += self.bas_line(blocks, b" " * (3 if blocks < 10 else 2 if blocks < 100 else 1 if blocks < 1000 else 0) + b'"' + n.encode() + b'"' + b" " * (16 - len(n)) + b" " + kind.encode() + b" ")
        out += self.bas_line(0, b"BLOCKS FREE.             ")
        return out + b"\0\0"

    def set_status(self, u, code, msg=""):
        u.status = "%02d,%s,00,00" % (code, " " + msg if msg else " OK")

    def command(self, dev, text):
        u = self.unit(dev)
        t = text.strip("\r")
        self.log.append((dev, t))
        if t.upper().startswith("CD:"):
            arg = t[3:]
            if arg in ("/",): u.mounted = None; u.cwd = ""; self.set_status(u, 0); return
            if arg == "_":
                if u.mounted: u.mounted = None
                elif u.cwd: u.cwd = u.cwd.rpartition("/")[0]
                self.set_status(u, 0); return
            p = arg.lower()
            if p in u.files:
                if u.files[p] is None: u.mounted = None; u.cwd = p
                else: u.mounted = p; u.cwd = p.rpartition("/")[0]
                self.set_status(u, 0)
            else: self.set_status(u, 62, "FILE NOT FOUND")
            return
        self.set_status(u, 31, "SYNTAX ERROR")

    # ---- the interpreter's I/O interface ----
    def open(self, lf, dev, sa, name):
        if dev not in self.units: raise BasicError("device not present %d" % dev)
        if lf in self.chans: raise BasicError("FILE OPEN lf=%d" % lf)
        u = self.unit(dev)
        if sa == 15:
            self.chans[lf] = Chan("cmd", dev)
            if name: self.command(dev, name)
            return
        if any(c.dev == dev and c.name == str(sa) for c in self.chans.values()): raise BasicError("secondary %d busy on %d" % (sa, dev))
        if name.startswith("$"):
            spec = name[1:].lower()
            if "/" in spec:
                folder = spec.rstrip("/") if spec != "/" else ""
                if folder and folder not in u.files: self.set_status(u, 62, "FILE NOT FOUND"); self.chans[lf] = Chan("none", dev, name=str(sa)); return
                self.chans[lf] = Chan("r", dev, self.listing(u, folder), str(sa))
            else:
                self.chans[lf] = Chan("r", dev, self.listing(u, u.cwd), str(sa))
            self.set_status(u, 0)
            return
        if name.startswith("!"):
            base, _, flag = name.partition(",")
            if base == "!STATE":
                if flag.upper() == "W": self.set_status(u, 26, "WRITE PROTECT ON"); self.chans[lf] = Chan("none", dev, name=str(sa)); return
                path = (u.mounted or "").upper()
                self.chans[lf] = Chan("r", dev, (path + "\r" + (u.cwd.upper() or "/") + "\r").encode(), str(sa)); self.set_status(u, 0); return
            if base == "!CONFIG":
                if flag.upper() == "W":
                    self.chans[lf] = Chan("w", dev, name=str(sa)); self.chans[lf].target = "/vdrive-config.txt"
                    u.files["/vdrive-config.txt"] = b""; self.set_status(u, 0); return
                data = u.files.get("/vdrive-config.txt")
                if data is None: self.set_status(u, 62, "FILE NOT FOUND"); self.chans[lf] = Chan("none", dev, name=str(sa)); return
                text = data.decode().replace("\r", "").replace("\n", "\r").upper().encode()
                self.chans[lf] = Chan("r", dev, text, str(sa)); self.set_status(u, 0); return
        self.set_status(u, 62, "FILE NOT FOUND"); self.chans[lf] = Chan("none", dev, name=str(sa))

    def close(self, lf):
        c = self.chans.pop(lf, None)
        if c is None: return
        if c.kind == "cmd":
            for k in [k for k, v in self.chans.items() if v.dev == c.dev]: self.chans.pop(k)    # closing channel 15 closes everything on the unit
            self.closed_all = getattr(self, "closed_all", 0) + 1

    def get(self, lf):
        c = self.chans.get(lf)
        if c is None or c.kind != "r": self.st = 66; return ""
        if c.pos >= len(c.data): self.st = 66; return ""
        b = c.data[c.pos]; c.pos += 1
        self.st = 64 if c.pos >= len(c.data) else 0
        return chr(b) if b else ""

    def getline(self, lf):
        c = self.chans[lf]
        if c.kind != "cmd": raise BasicError("INPUT# from a data channel")
        u = self.unit(c.dev); s = u.status; self.set_status(u, 0); return s

    def put(self, lf, text):
        c = self.chans[lf]
        if c.kind == "cmd": self.command(c.dev, text)
        elif c.kind == "w":
            u = self.unit(c.dev)
            u.files[c.target] += text.replace("\r", "\n").lower().encode()
        else: raise BasicError("PRINT# to a read channel")


def make_sd():
    f = {"/games": None, "/games/zork.d64": IMG * 100, "/games/sub": None, "/games/sub/deep.d64": IMG, "/util": None, "/util/tools.d64": IMG,
         "/big.d81": IMG, "/readme.txt": b"hello", "/.vdrive": b"/big.d81", "/ninechars_ok.d64": IMG}
    return f


CONFIG = ("# my disks\r\n/GAMES/ZORK.D64, 5, 2\r\n/UTIL/TOOLS.D64,7\r\n\r\n/BIG.D81\r\n/GONE/NOWHERE.D64, 2, 9\r\n").lower()


class T(unittest.TestCase):
    def start(self, sd=None, usb=None, cfg=CONFIG, usb_cfg=None, devs=(8, 9), mounted=None):
        files = make_sd() if sd is None else sd
        if cfg is not None: files["/vdrive-config.txt"] = cfg.encode()
        ufiles = None
        if usb is not False and devs[1]:
            ufiles = {"/usb.d64": IMG, "/disks": None, "/disks/a.d64": IMG}
            if usb_cfg is not None: ufiles["/vdrive-config.txt"] = usb_cfg.encode()
        self.drive = FakeDrive(files, ufiles)
        if mounted: self.drive.units[8].mounted = mounted; self.drive.units[8].cwd = mounted.rpartition("/")[0]
        self.b = Basic(SRC, self.drive, {56854: devs[0], 56855: devs[1], 53280: 14, 53281: 6, 646: 14})
        self.assertEqual(self.b.run(), "key")
        return self.b

    def keys(self, *k):
        self.b.press(*k)
        r = self.b.run()
        self.assertEqual(r, "key", "program ended: " + self.b.screen.text())
        self.assertEqual(self.b.screen.scrolled, 0, "the screen scrolled:\n" + self.b.screen.text())
        return self.b.screen

    def row(self, r): return self.b.screen.row(r)

    def entry(self, r):
        m = re.match(r"^[> ][* ]\s*(\d+) (\S+)\s+(\d+)\s+(\d+)X$", self.row(r))
        return m.groups() if m else None

    def config(self, dev=8): return self.drive.units[dev].files.get("/vdrive-config.txt", b"").decode()

    # ---------------------------------------------------------------- tests
    def test_first_screen(self):
        self.start(mounted="/games/zork.d64")
        self.assertIn("VDRIVE CONFIG", self.row(0))
        self.assertEqual(self.row(1), "SD  8: /GAMES/ZORK.D64")
        self.assertEqual(self.row(2), "USB 9: (NOTHING MOUNTED)")
        self.assertEqual(self.row(3), "LIST: SD (4 ENTRIES)")
        self.assertTrue(re.match(r"^>\*\s+1 /GAMES/ZORK\.D64\s+5\s+2X$", self.row(4)), self.row(4))
        self.assertTrue(re.match(r"^ {2}\s*2 /UTIL/TOOLS\.D64\s+7\s+1X$", self.row(5)), self.row(5))
        self.assertTrue((self.entry(6) or ())[:2] == tuple(["3", "/BIG.D81"]), self.row(6))
        self.assertTrue((self.entry(7) or ())[:2] == tuple(["4", "/GONE/NOWHERE.D64"]), self.row(7))
        self.assertEqual(self.row(8), "")
        self.assertTrue(self.row(21).startswith("RETURN:MOUNT"))
        self.assertEqual(self.row(23), "T:SD/USB LIST R:REFRESH Q:QUIT")
        # the colour cell: reverse, in colour 5 (green) for entry 1, nothing else on that row is reversed after the path
        cells = [c for c in range(40) if self.b.screen.rev[4][c] and self.b.screen.ch[4][c] == " "]
        self.assertTrue(cells and all(self.b.screen.col[4][c] == 5 for c in cells[-2:]), cells)

    def test_colour_codes(self):
        self.start()
        for r, col in ((4, 5), (5, 7), (6, 1)):
            cells = [c for c in range(40) if self.b.screen.rev[r][c] and self.b.screen.ch[r][c] == " " and c > 28]
            self.assertTrue(cells and all(self.b.screen.col[r][c] == col for c in cells), (r, cells))

    def test_cursor_and_mount(self):
        self.start()
        s = self.keys(DOWN)
        self.assertTrue(self.row(5).startswith(">") and self.row(4).startswith(" "))
        self.keys(RET)                                   # mount entry 2
        self.assertEqual(self.drive.units[8].mounted, "/util/tools.d64")
        self.assertEqual(self.row(1), "SD  8: /UTIL/TOOLS.D64")
        self.assertEqual(self.row(20), "MOUNTED")
        self.assertTrue(self.row(5).startswith(">*"))
        self.keys(UP, UP)                                # stays on the first entry
        self.assertTrue(self.row(4).startswith(">"))
        self.keys(DOWN, DOWN, DOWN, RET)                 # entry 4 does not exist
        self.assertTrue(self.row(20).startswith("ERROR 62"), self.row(20))
        self.assertEqual(self.drive.units[8].mounted, "/util/tools.d64")

    def test_unmount(self):
        self.start(mounted="/big.d81")
        self.keys("u")
        self.assertIsNone(self.drive.units[8].mounted)
        self.assertEqual(self.row(20), "UNMOUNTED"); self.assertEqual(self.row(1), "SD  8: (NOTHING MOUNTED)")
        self.keys("u")
        self.assertEqual(self.row(20), "NOTHING MOUNTED")
        self.assertEqual([c for d, c in self.drive.log if c.upper() == "CD:_"], ["CD:_"])         # a second U must not climb out of the folder

    def test_delete_and_move(self):
        self.start()
        self.keys("d")
        self.assertTrue(self.row(20).startswith("DELETE ENTRY 1?"))
        self.keys("n")
        self.assertEqual(self.config(), CONFIG)           # nothing written
        self.keys(DOWN, "+")                              # entry 2 moves down
        lines = self.config().split("\n")
        order = [l.split(",")[0] for l in lines if l.startswith("/")]
        self.assertEqual(order, ["/games/zork.d64", "/big.d81", "/util/tools.d64", "/gone/nowhere.d64"])
        self.assertEqual(lines[0], "# my disks")                                  # comments stay in place
        self.assertTrue((self.entry(6) or ())[:2] == tuple(["3", "/UTIL/TOOLS.D64"]) and self.row(6).startswith(">"), self.row(6))
        self.keys("-")                                                           # back up: the cursor follows the entry
        order = [l.split(",")[0] for l in self.config().split("\n") if l.startswith("/")]
        self.assertEqual(order, ["/games/zork.d64", "/util/tools.d64", "/big.d81", "/gone/nowhere.d64"])
        self.keys("-")
        order = [l.split(",")[0] for l in self.config().split("\n") if l.startswith("/")]
        self.assertEqual(order[0], "/util/tools.d64")
        self.keys("-")                                                           # already first: nothing happens
        self.keys("d", "y")
        order = [l.split(",")[0] for l in self.config().split("\n") if l.startswith("/")]
        self.assertEqual(order, ["/games/zork.d64", "/big.d81", "/gone/nowhere.d64"])
        self.assertEqual(self.row(20), "SAVED")
        self.assertTrue(self.row(3).startswith("LIST: SD (3 ENTRIES)"))
        self.keys(DOWN, DOWN, DOWN, DOWN)
        self.keys("d", "y", "d", "y", "d", "y")
        self.assertEqual([l for l in self.config().split("\n") if l.startswith("/")], [])
        self.assertEqual(self.row(3), "LIST: SD (0 ENTRIES)")
        self.keys("d", "e", RET)                                                  # on an empty list these do nothing
        self.assertEqual(self.row(4), "")

    def test_add_through_browser(self):
        self.start(mounted="/big.d81")
        self.keys("a")
        self.assertEqual(self.row(3), "BROWSE: /")
        names = [self.row(r) for r in range(4, 12)]
        self.assertTrue(any("GAMES" in n and "DIR" in n for n in names) and any("BIG.D81" in n and "D81" in n for n in names), names)
        self.assertFalse(any("README" in n for n in names), "plain files are not listed")
        self.assertTrue(self.row(4).startswith(">"))
        idx = next(i for i, n in enumerate(names) if "GAMES" in n)
        self.keys(*([DOWN] * idx), RET)                  # into GAMES
        self.assertEqual(self.row(3), "BROWSE: /GAMES")
        self.assertTrue(self.row(4).strip().startswith(">") and ".." in self.row(4))
        self.assertEqual(self.drive.units[8].mounted, "/big.d81", "browsing must not change the mounted image")
        names = [self.row(r) for r in range(5, 9)]
        sub = next(i for i, n in enumerate(names) if "SUB" in n)
        self.keys(*([DOWN] * (sub + 1)), RET)            # into GAMES/SUB
        self.assertEqual(self.row(3), "BROWSE: /GAMES/SUB")
        self.keys(DOWN, RET)                             # DEEP.D64
        self.assertEqual(self.row(23).startswith("COLOUR 0-15: 1"), True, self.row(23))
        self.keys(DEL, "1", "2", RET)
        self.assertTrue(self.row(23).startswith("FLASHES 0-20: 1"), self.row(23))
        self.keys(DEL, "4", RET)
        self.assertEqual(self.config().split("\n")[-2], "/games/sub/deep.d64, 12, 4")
        self.assertEqual(self.row(20), "SAVED")
        self.assertEqual(self.row(3), "LIST: SD (5 ENTRIES)")
        self.assertTrue(self.row(8).startswith(">"), self.row(8))                # cursor on the new entry
        self.assertEqual(self.drive.units[8].mounted, "/big.d81")

    def test_browse_up_and_cancel(self):
        self.start()
        self.keys("a", DOWN, RET)                        # into the first directory
        self.assertTrue(self.row(3).startswith("BROWSE: /"), self.row(3))
        self.keys(RET)                                   # ".." goes back to the root
        self.assertEqual(self.row(3), "BROWSE: /")
        self.keys(LEFTARROW)
        self.assertTrue("VDRIVE CONFIG" in self.row(0) and self.row(3).startswith("LIST:"))
        self.assertEqual(self.config(), CONFIG)

    def test_browse_and_mount_any_image(self):
        self.start()
        self.keys("b")
        names = [self.row(r) for r in range(4, 12)]
        k = next(i for i, n in enumerate(names) if "BIG.D81" in n)
        self.keys(*([DOWN] * k), RET)
        self.assertEqual(self.drive.units[8].mounted, "/big.d81")
        self.assertEqual(self.row(1), "SD  8: /BIG.D81"); self.assertEqual(self.row(20), "MOUNTED")
        self.assertEqual(self.config(), CONFIG, "mounting from the browser does not touch the list")

    def test_edit(self):
        self.start()
        self.keys("e")
        self.assertTrue(self.row(23).startswith("PATH(?=BROWSE): /GAMES/ZORK.D64"), self.row(23))
        for _ in range(len("zork.d64")): self.keys(DEL)
        self.keys(*"ZORK2.D64", RET)
        self.assertTrue(self.row(23).startswith("COLOUR 0-15: 5"))
        self.keys(DEL, "9", RET)
        self.keys(DEL, "0", RET)                         # 0 flashes = silent but valid
        self.assertEqual(self.config().split("\n")[1], "/games/zork2.d64, 9, 0")
        self.assertEqual(self.config().split("\n")[0], "# my disks")
        self.keys("e", LEFTARROW)                        # cancel keeps everything
        self.assertEqual(self.config().split("\n")[1], "/games/zork2.d64, 9, 0")
        self.keys("e", *([DEL] * 30), "X", RET)          # not a path
        self.assertEqual(self.row(20), "PATH MUST START WITH /")
        self.keys("e", *([DEL] * 30), "?", RET)          # browse for a new path
        self.assertTrue(self.row(3).startswith("BROWSE"))
        self.keys(LEFTARROW)

    def test_bad_numbers_are_ignored(self):
        self.start()
        self.keys("e", RET, "9", "9", RET)               # colour 99 -> kept (5)
        self.keys("9", "9", RET)
        self.assertEqual(self.config().split("\n")[1], "/games/zork.d64, 5, 2")

    def test_toggle_to_usb(self):
        self.start(usb_cfg="/usb.d64, 3, 3\n/disks/a.d64\n")
        self.keys("t")
        self.assertEqual(self.row(3), "LIST: USB (2 ENTRIES)")
        self.assertTrue((self.entry(4) or ())[:2] == tuple(["1", "/USB.D64"]), self.row(4))
        self.keys(RET)
        self.assertEqual(self.drive.units[9].mounted, "/usb.d64")
        self.assertEqual(self.row(2), "USB 9: /USB.D64")
        self.assertEqual(self.drive.units[8].mounted, None)
        self.keys("d", "y")
        self.assertEqual(self.config(9), "/disks/a.d64\n")
        self.assertEqual(self.config(8), CONFIG)
        self.keys("t")
        self.assertEqual(self.row(3), "LIST: SD (4 ENTRIES)")

    def test_no_usb_drive(self):
        self.start(devs=(8, 0))
        self.assertEqual(self.row(2), "USB  : NO DRIVE")
        self.keys("t")
        self.assertEqual(self.row(20), "NO DRIVE ON THAT UNIT")
        self.assertEqual(self.row(3), "LIST: SD (4 ENTRIES)")

    def test_only_usb(self):
        self.start(devs=(0, 9), usb_cfg="/usb.d64\n")
        self.assertEqual(self.row(1), "SD   : NO DRIVE")
        self.assertEqual(self.row(3), "LIST: USB (1 ENTRIES)")
        self.keys(RET)
        self.assertEqual(self.drive.units[9].mounted, "/usb.d64")

    def test_no_drive_at_all(self):
        self.drive = FakeDrive({})
        self.b = Basic(SRC, self.drive, {56854: 0, 56855: 0})
        self.assertEqual(self.b.run(), "end")
        self.assertIn("NOT ACTIVE", self.b.screen.text())

    def test_missing_and_empty_config(self):
        self.start(cfg=None)
        self.assertEqual(self.row(3), "LIST: SD (0 ENTRIES)")
        self.keys("a")                                  # browse + add creates the file
        names = [self.row(r) for r in range(4, 12)]
        k = next(i for i, n in enumerate(names) if "BIG.D81" in n)
        self.keys(*([DOWN] * k), RET, DEL, "2", RET, DEL, "3", RET)
        self.assertEqual(self.config(), "/big.d81, 2, 3\n")

    def test_parser_cases(self):
        cfg = ("  /A/B,C.D64 , 3 , 4  \r\n/NO/NUMBERS.D64\r\n/ONE.D64, 99\r\n/TOOMANY.D64, 1, 99\r\n/COMMA,IN,NAME.D64,6\r\n"
               "not a path\r\n#/commented.d64\r\n/SPACED.D64 ,12,0\r\n").lower()
        self.start(cfg=cfg)
        rows = [self.row(r) for r in range(4, 12)]
        got = [re.match(r"^.. *(\d+) (\S+) +(\d+) +(\d+)X$", r) for r in rows if r.strip()]
        self.assertEqual(len(got), 6, rows)
        self.assertEqual([g.groups() for g in got], [
            ("1", "/A/B,C.D64", "3", "4"), ("2", "/NO/NUMBERS.D64", "1", "1"), ("3", "/ONE.D64", "3", "1"),
            ("4", "/TOOMANY.D64", "1", "20"), ("5", "/COMMA,IN,NAME.D64", "6", "1"), ("6", "/SPACED.D64", "12", "0")])

    def test_unparsed_lines_survive_a_save(self):
        self.start()
        self.keys("-", "d", "n")                        # no change
        self.keys("+")                                  # entry 1 swaps with 2
        text = self.config()
        self.assertIn("# my disks\n", text)
        self.assertEqual(text.count("\n\n"), 1, "the blank line stays")

    def test_long_names_and_many_entries(self):
        lines = "".join("/DIR%02d/IMAGE_%02d.D64,%d,%d\n" % (i, i, i % 16, i % 5) for i in range(32)).lower()
        self.start(cfg=lines)
        self.assertEqual(self.row(3), "LIST: SD (32 ENTRIES)")
        self.keys(*([DOWN] * 31))
        self.assertTrue(self.row(19).startswith(">"), self.row(19))        # last visible row after scrolling
        self.assertTrue((self.entry(19) or ())[:2] == tuple(["32", "/DIR31/IMAGE_31.D64"]), self.row(19))
        self.keys("a")                                                      # full
        self.assertEqual(self.row(20), "LIST IS FULL")
        self.keys(UP, *([UP] * 20))
        self.assertTrue(self.row(4).startswith(">") or self.row(4).split()[1] == "1")

    def test_refresh_shows_a_mount_done_elsewhere(self):
        self.start()
        self.drive.units[8].mounted = "/util/tools.d64"                    # the alt button switched the disk meanwhile
        self.keys("r")
        self.assertEqual(self.row(1), "SD  8: /UTIL/TOOLS.D64"); self.assertTrue(self.row(5).startswith(" *"))

    def test_quit_restores_the_screen(self):
        self.start()
        self.b.press("q")
        self.assertEqual(self.b.run(), "end")
        self.assertEqual((self.b.mem[53280], self.b.mem[53281], self.b.mem[646]), (14, 6, 14))
        self.assertEqual(self.drive.chans, {})
        self.assertTrue(self.b.screen.row(0).endswith("BYE") or "BYE" in self.b.screen.text())

    def test_never_closes_the_command_channel_while_working(self):
        self.start()
        self.keys(RET, "u", "a", LEFTARROW, "t", "t")
        self.assertEqual(getattr(self.drive, "closed_all", 0), 0)


if __name__ == "__main__":
    unittest.main(verbosity=1)
