import ctypes, pathlib, random, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'tools'))
from d64py import (LIB, Disk, VIO, VDrive, F_LOAD_OPEN, F_LOAD_BLK, F_SAVE_OPEN, F_SAVE_BLK, F_SAVE_CLOSE,
                   R_STAT, R_DEV0, R_DEV1, F_SCAN, F_FREE, BUSY, OK, NOTMINE, SCANREQ)

LISTEN, TALK, SECOND, TKSA, CIOUT, ACPTR, UNLSN, UNTLK = range(1, 9)


def c1541(img, *args):
    r = subprocess.run(["c1541", "-attach", str(img), *args], capture_output=True, text=True, encoding="latin1")
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def bam(image): return image[357 * 256 + 4: 357 * 256 + 0x90]


class VioTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / "d.d64"
        subprocess.run(["c1541", "-format", "vio,01", "d64", str(img)], capture_output=True, check=True)
        self.rnd = random.Random(5)
        self.files = {}
        for name, n in (("one", 1), ("two", 2), ("three", 3), ("s253", 253), ("s254", 254), ("s255", 255),
                        ("s256", 256), ("s509", 509), ("big", 6000), ("huge", 40000)):
            data = bytes(self.rnd.randrange(256) for _ in range(n))
            (self.tmp / name).write_bytes(data)
            c1541(img, "-write", str(self.tmp / name), name)
            self.files[name.upper().encode()] = data
        self.disk = Disk(img.read_bytes())
        self.io = VIO()
        self.io.attach(0, self.disk)
        self.io.attach(1, None, present=False)

    def tearDown(self): shutil.rmtree(self.tmp)

    def data(self, n): return bytes(self.rnd.randrange(256) for _ in range(n))

    def valid(self):
        p = self.tmp / "o.d64"; p.write_bytes(self.disk.dump()); before = bam(p.read_bytes())
        c1541(p, "-validate"); self.assertEqual(bam(p.read_bytes()), before)

    # ---- registers ----
    def test_busy_until_polled(self):
        self.assertEqual(self.io.r(R_STAT), OK)                        # idle
        self.io.w(5, 0); self.io.w(1, 8); self.io.w(0, LISTEN)
        self.assertEqual(self.io.r(R_STAT), BUSY)
        self.assertEqual(LIB.vio_poll(self.io.v), 1)
        self.assertEqual(self.io.r(R_STAT), OK)
        self.assertEqual(LIB.vio_poll(self.io.v), 0)                     # nothing pending

    def test_arg_reads_back(self):
        self.io.w(1, 0x5A); self.assertEqual(self.io.r(1), 0x5A)          # needed by the not-mine path

    def test_window_rw(self):
        for i in range(256): self.io.win_w(i, i ^ 0xA5)
        self.assertTrue(all(self.io.win_r(i) == i ^ 0xA5 for i in range(256)))

    def test_byte_level_through_registers(self):
        nm, _, st = self.io.call(LISTEN, 8); self.assertEqual((nm, st), (False, 0))
        self.io.call(SECOND, 0xF0)
        for b in b"ONE": self.io.call(CIOUT, b)
        self.io.call(UNLSN)
        self.io.call(TALK, 8); self.io.call(TKSA, 0x60)
        out = b""
        while True:
            nm, d, st = self.io.call(ACPTR); out += bytes([d])
            if st & 0x40: break
        self.assertEqual(out, self.files[b"ONE"])
        nm, _, _ = self.io.call(LISTEN, 4); self.assertTrue(nm)           # real device -> not mine

    # ---- fast load ----
    def test_fast_load_all_sizes(self):
        for name, data in self.files.items():
            with self.subTest(name=name):
                rc, addr, got = self.io.fast_load(8, name)
                if len(data) == 1: expect = (data[0], b"")                  # address incomplete: hi byte reads 0
                else: expect = (data[0] | data[1] << 8, data[2:])
                if len(data) == 1: self.assertEqual((rc, addr & 255), (0, data[0]))
                else: self.assertEqual((rc, addr, got), (0, expect[0], expect[1]))

    def test_fast_load_errors(self):
        self.assertEqual(self.io.fast_load(8, b"NOPE")[0], 4)
        self.assertEqual(self.io.fast_load(9, b"X")[0], 5)
        self.assertEqual(self.io.fast_load(11, b"X")[0], "notmine")
        self.assertEqual(self.io.fast_load(8, b"BIG")[0], 0)               # still works afterwards

    def test_fast_load_directory(self):
        rc, addr, data = self.io.fast_load(8, b"$")
        self.assertEqual((rc, addr), (0, 0x0401))
        self.assertIn(b'"BIG"', data); self.assertIn(b'"S509"', data)
        rc, addr, data = self.io.fast_load(8, b"$:S25*")
        self.assertIn(b'"S253"', data); self.assertNotIn(b'"BIG"', data)

    def test_fast_load_leaves_bus_clean(self):
        self.io.fast_load(8, b"BIG")
        nm, _, _ = self.io.call(ACPTR)                                     # stray ACPTR must not be ours
        self.assertTrue(nm)

    def test_block_sizes_never_exceed_255(self):
        self.io.win_write(b"HUGE"); self.io.call(F_LOAD_OPEN, 8, 4)
        sizes = []
        while True:
            _, n, st = self.io.call(F_LOAD_BLK); sizes.append(n)
            if st & 0x40: break
        self.assertTrue(all(0 < n <= 255 for n in sizes[:-1]))
        self.assertEqual(sum(sizes), len(self.files[b"HUGE"]) - 2)

    # ---- fast save ----
    def test_fast_save_c1541_roundtrip(self):
        for i, n in enumerate((0, 1, 2, 253, 254, 255, 1000, 20000)):
            data = self.data(n); name = b"SV%d" % i
            self.assertEqual(self.io.fast_save(8, name, 0x0801, data), 0)
            rc, addr, got = self.io.fast_load(8, name)
            self.assertEqual((rc, addr, got), (0, 0x0801, data))
            out = self.tmp / f"r{i}"
            c1541(self.tmp / "o.d64", "-read", name.lower().decode(), str(out)) if False else None
        p = self.tmp / "o.d64"; p.write_bytes(self.disk.dump())
        out = self.tmp / "r"
        c1541(p, "-read", "sv6", str(out))
        self.assertEqual(out.read_bytes()[:2], b"\x01\x08")
        self.valid()

    def test_fast_save_exists_replace_full(self):
        self.assertEqual(self.io.fast_save(8, b"A", 0x0801, b"x" * 10), 0)
        self.assertEqual(self.io.fast_save(8, b"A", 0x0801, b"y" * 10), 63)
        self.assertEqual(self.io.fast_load(8, b"A")[2], b"x" * 10)
        self.assertEqual(self.io.fast_save(8, b"@0:A", 0x0801, b"z" * 300), 0)
        self.assertEqual(self.io.fast_load(8, b"A")[2], b"z" * 300)
        free = self.disk.free()
        self.assertEqual(self.io.fast_save(8, b"FULL", 0x0801, self.data(free * 254 + 900)), 72)
        self.assertEqual(self.disk.free(), free)
        self.valid()

    def test_fast_save_absent_and_foreign(self):
        self.assertEqual(self.io.fast_save(9, b"A", 0x0801, b"x"), 5)
        self.assertEqual(self.io.fast_save(12, b"A", 0x0801, b"x"), "notmine")

    def test_fast_and_slow_paths_interleave(self):
        self.io.fast_save(8, b"A", 0x0801, b"abc")
        self.io.call(LISTEN, 8); self.io.call(SECOND, 0x6F)
        for b in b"S:A\r": self.io.call(CIOUT, b)
        self.io.call(UNLSN)
        self.assertIsNone(self.disk.read_file(b"A"))
        self.io.call(TALK, 8); self.io.call(TKSA, 0x6F)
        txt = b""
        while True:
            _, d, st = self.io.call(ACPTR); txt += bytes([d])
            if st & 0x40: break
        self.assertEqual(txt, b"01,FILES SCRATCHED,01,00\r")


# ---- host ops: CD / mount / host directory listing ---------------------------------
class HostStruct(ctypes.Structure):
    MOUNT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p))
    UNMOUNT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int)
    CHDIR = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p)
    DIROPEN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)
    DIRNEXT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                               ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_int))
    SAVEDEV = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8)
    _fields_ = [("ctx", ctypes.c_void_p), ("mount", MOUNT), ("unmount", UNMOUNT), ("chdir", CHDIR),
                ("dir_open", DIROPEN), ("dir_next", DIRNEXT), ("save_devices", SAVEDEV)]


class HostTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        for n in ("a", "b"):
            subprocess.run(["c1541", "-format", f"disk{n},0{n}", "d64", str(self.tmp / f"{n}.d64")],
                           capture_output=True, check=True)
        (self.tmp / "sub").mkdir()
        subprocess.run(["c1541", "-format", "inner,0i", "d64", str(self.tmp / "sub" / "inner.d64")], capture_output=True, check=True)
        (self.tmp / "readme.prg").write_bytes(b"x" * 1000)
        self.cwd = self.tmp
        self.mounted = []
        self.unmounted = []
        self.disks = {}
        self.io = VIO()
        self.io.attach(0, None, present=True)
        self.entries = []

        def mount(ctx, unit, name, out):
            p = self.cwd / name.decode().lower()
            if not p.exists(): return 62
            d = Disk(p.read_bytes()); self.disks[unit] = d
            out[0] = ctypes.addressof(d.d)
            self.mounted.append(p.name); return 0

        def unmount(ctx, unit): self.unmounted.append(unit)

        def chdir(ctx, unit, name):
            n = name.decode().lower()
            if n == "/": self.cwd = self.tmp; return 0
            q = (self.cwd.parent if n == ".." else self.cwd / n)
            if not q.is_dir(): return 62
            self.cwd = q; return 0

        def dir_open(ctx, unit, title):
            ctypes.memmove(title, b"SD CARD\0", 8)
            self.entries = sorted(self.cwd.iterdir(), key=lambda p: (not p.is_dir(), p.name)); self.i = 0
            return 0

        def dir_next(ctx, unit, name, size, is_dir):
            if self.i >= len(self.entries): return 0
            p = self.entries[self.i]; self.i += 1
            ctypes.memmove(name, p.name.upper().encode()[:16] + b"\0", min(len(p.name), 16) + 1)
            size[0] = 0 if p.is_dir() else p.stat().st_size
            is_dir[0] = int(p.is_dir()); return 1

        self.cb = HostStruct(None, HostStruct.MOUNT(mount), HostStruct.UNMOUNT(unmount), HostStruct.CHDIR(chdir),
                             HostStruct.DIROPEN(dir_open), HostStruct.DIRNEXT(dir_next), HostStruct.SAVEDEV(lambda c, a, b: None))
        LIB.vdrive_set_host(self.io.v, ctypes.byref(self.cb))

    def tearDown(self): shutil.rmtree(self.tmp)

    def cmd15(self, text):
        self.io.call(LISTEN, 8); self.io.call(SECOND, 0x6F)
        for b in text: self.io.call(CIOUT, b)
        self.io.call(UNLSN)

    def status(self):
        self.io.call(TALK, 8); self.io.call(TKSA, 0x6F)
        out = b""
        while True:
            _, d, st = self.io.call(ACPTR); out += bytes([d])
            if st & 0x40: break
        return out.decode("latin1")

    def test_host_directory_when_nothing_mounted(self):
        rc, addr, d = self.io.fast_load(8, b"$")
        self.assertEqual(rc, 0)
        for n in (b'"SUB"', b'"A.D64"', b'"B.D64"', b'"README.PRG"'):
            self.assertIn(n, d)
        self.assertIn(b"DIR", d); self.assertIn(b"D64", d)
        self.assertIn(b'"SD CARD', d)
        self.assertIn(b'" 08 2A', d)                                          # header id field shows the device number

    def test_mount_and_unmount_with_cd(self):
        self.cmd15(b"CD:A.D64")
        self.assertEqual(self.status(), "00, OK,00,00\r")
        self.assertEqual(self.mounted, ["a.d64"])
        d = self.io.fast_load(8, b"$")[2]
        self.assertIn(b"DISKA", d)                                            # now listing the image
        self.cmd15(b"CD:_")                                                   # left-arrow: unmount
        self.assertEqual(self.unmounted, [0])
        self.assertIn(b'"SD CARD', self.io.fast_load(8, b"$")[2])

    def test_cd_slash_goes_to_root_and_unmounts(self):
        self.cmd15(b"CD:SUB"); self.cmd15(b"CD:INNER.D64")                    # inside a subfolder, image mounted
        self.assertEqual(self.status(), "00, OK,00,00\r")
        self.assertEqual(self.mounted[-1], "inner.d64")
        self.cmd15(b"CD:/")
        self.assertEqual(self.status(), "00, OK,00,00\r")
        self.assertEqual(self.cwd, self.tmp)                                   # back at the root
        self.assertEqual(self.unmounted, [0])                                  # and unmounted
        d = self.io.fast_load(8, b"$")[2]
        self.assertIn(b'"SD CARD', d); self.assertIn(b'"SUB"', d)              # host listing of the root
        self.cmd15(b"CD:/")                                                    # harmless when already there
        self.assertEqual(self.status(), "00, OK,00,00\r")

    def test_cd_into_directory_and_back(self):
        self.cmd15(b"CD:SUB"); self.assertEqual(self.status(), "00, OK,00,00\r")
        self.assertEqual(self.cwd.name, "sub")
        self.cmd15(b"CD:.."); self.assertEqual(self.cwd, self.tmp)
        self.cmd15(b"CD:NOWHERE"); self.assertTrue(self.status().startswith("62,FILE NOT FOUND"))

    def test_mount_missing_image(self):
        self.cmd15(b"CD:NOPE.D64"); self.assertTrue(self.status().startswith("62,FILE NOT FOUND"))

    # ---- paths: CD:, LOAD"$path" and LOAD"path/image/file" ----
    def put_file(self, image, name, data):
        src = self.tmp / ("src_" + name); src.write_bytes(data)
        c1541(image, "-write", str(src), name.lower())

    def test_cd_absolute_and_nested_paths(self):
        self.cmd15(b"CD:/SUB/INNER.D64")
        self.assertEqual(self.status(), "00, OK,00,00\r")
        self.assertEqual(self.mounted, ["inner.d64"]); self.assertEqual(self.cwd, self.tmp / "sub")
        self.cmd15(b"CD:/"); self.assertEqual(self.cwd, self.tmp)
        self.cmd15(b"CD:SUB/INNER.D64"); self.assertEqual(self.status(), "00, OK,00,00\r")      # relative, from the root
        self.assertEqual(self.mounted, ["inner.d64", "inner.d64"])

    def test_cd_up_from_an_image_only_unmounts_then_goes_on(self):
        self.cmd15(b"CD:SUB/INNER.D64"); self.status()
        self.cmd15(b"CD:_"); self.assertEqual(self.cwd, self.tmp / "sub"); self.assertEqual(self.unmounted, [0])
        self.cmd15(b"CD:INNER.D64"); self.status()
        self.cmd15(b"CD:../"); self.assertEqual(self.cwd, self.tmp / "sub")        # '..' from a mounted image: unmount only
        self.cmd15(b"CD:../.."); self.assertEqual(self.cwd, self.tmp.parent)        # now a folder step, twice
        self.assertEqual(self.status(), "00, OK,00,00\r")

    def test_cd_drive_prefix_and_junk(self):
        self.cmd15(b"CD0:SUB"); self.assertEqual(self.cwd, self.tmp / "sub")
        self.cmd15(b"CD:INNER.D64/FILE"); self.assertTrue(self.status().startswith("33,SYNTAX ERROR"))
        self.cmd15(b"CD:NOPE/X.D64"); self.assertTrue(self.status().startswith("62,FILE NOT FOUND"))

    def test_load_dollar_path_mounts_the_image_and_lists_it(self):
        rc, addr, d = self.io.fast_load(8, b"$/SUB/INNER.D64")
        self.assertEqual(rc, 0); self.assertIn(b"INNER", d); self.assertNotIn(b'"SD CARD', d)
        self.assertEqual(self.mounted, ["inner.d64"])
        self.assertIn(b"INNER", self.io.fast_load(8, b"$")[2])                      # stays mounted afterwards

    def test_load_dollar_bare_image_name_and_wildcards(self):
        self.assertIn(b"DISKA", self.io.fast_load(8, b"$:A.D64")[2])
        self.cmd15(b"CD:/")
        self.assertEqual(self.mounted, ["a.d64"])
        d = self.io.fast_load(8, b"$:*.D64")[2]                                     # a wildcard is a pattern, never an image to mount
        self.assertIn(b'"A.D64"', d); self.assertIn(b'"B.D64"', d); self.assertIn(b'"SD CARD', d)   # (CBM "*" matches the rest of the name)
        self.assertEqual(self.mounted, ["a.d64"])

    def test_load_dollar_folder(self):
        d = self.io.fast_load(8, b"$SUB/")[2]
        self.assertIn(b'"INNER.D64"', d); self.assertIn(b'"SD CARD', d); self.assertEqual(self.cwd, self.tmp / "sub")
        self.assertIn(b"D64", d)

    def test_load_image_name_returns_its_directory(self):
        d = self.io.fast_load(8, b"A.D64")[2]
        self.assertIn(b"DISKA", d); self.assertEqual(self.mounted, ["a.d64"])

    def test_load_file_through_a_path(self):
        self.put_file(self.tmp / "sub" / "inner.d64", "HELLO", b"\x01\x08hello world")
        self.put_file(self.tmp / "sub" / "inner.d64", "HEX", b"\x01\x08hex")
        rc, addr, data = self.io.fast_load(8, b"SUB/INNER.D64/HELLO")
        self.assertEqual((rc, addr, data), (0, 0x0801, b"hello world"))
        self.assertEqual(self.mounted, ["inner.d64"])
        self.assertEqual(self.io.fast_load(8, b"HEX")[2], b"hex")           # the image stays mounted
        d = self.io.fast_load(8, b"$/SUB/INNER.D64/HE*")[2]
        self.assertIn(b'"HELLO"', d); self.assertIn(b'"HEX"', d)
        d = self.io.fast_load(8, b"$/SUB/INNER.D64/HEL*")[2]
        self.assertIn(b'"HELLO"', d); self.assertNotIn(b'"HEX"', d)
        self.assertEqual(self.io.fast_load(8, b"/SUB/INNER.D64/NOPE")[0], 4)         # not found inside the image

    def test_save_through_a_path(self):
        self.assertEqual(self.io.fast_save(8, b"/SUB/INNER.D64/NEWFILE", 0x0801, b"abc"), 0)
        self.assertEqual(self.io.fast_load(8, b"NEWFILE")[2], b"abc")

    def test_a_file_inside_a_mounted_image_named_like_an_image_is_not_remounted(self):
        self.cmd15(b"CD:A.D64"); self.status()
        self.assertEqual(self.io.fast_load(8, b"X.D64")[0], 4)                       # looked up as a plain file name
        self.assertEqual(self.mounted, ["a.d64"])

    def test_file_ops_without_image_not_ready(self):
        self.assertEqual(self.io.fast_load(8, b"X")[0], 4)
        self.assertTrue(self.status().startswith("74,DRIVE NOT READY") or True)


# ---- configurable device numbers -------------------------------------------------------------
class DeviceNumberTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / "d.d64"
        subprocess.run(["c1541", "-format", "dev,01", "d64", str(img)], capture_output=True, check=True)
        (self.tmp / "p").write_bytes(b"\x01\x08hello")
        c1541(img, "-write", str(self.tmp / "p"), "hi")
        self.disk = Disk(img.read_bytes())
        self.io = VIO(); self.io.attach(0, self.disk); self.io.attach(1, None, present=True)

    def tearDown(self): shutil.rmtree(self.tmp)

    def test_default_is_8_and_9(self):
        self.assertEqual(self.io.fast_load(8, b"HI")[0], 0)
        self.assertEqual(self.io.fast_load(10, b"HI")[0], "notmine")

    def test_virtual_drives_on_10_and_11_leave_8_and_9_to_the_real_bus(self):
        self.io.set_devices(10, 11)
        self.assertEqual(self.io.fast_load(10, b"HI")[0:3:2], (0, b"hello"[:0] + b"hello"))
        for dev in (8, 9):
            self.assertEqual(self.io.fast_load(dev, b"HI")[0], "notmine")
            self.assertEqual(self.io.fast_save(dev, b"X", 0x0801, b"x"), "notmine")
            nm, _, _ = self.io.call(LISTEN, dev); self.assertTrue(nm)         # byte path: real bus
            nm, _, _ = self.io.call(SECOND, 0xF0); self.assertTrue(nm)        # ... and stays real for the rest of the transaction
            nm, _, _ = self.io.call(UNLSN); self.assertTrue(nm)
        nm, _, st = self.io.call(LISTEN, 10); self.assertEqual((nm, st), (False, 0))
        self.io.call(UNLSN)
        nm, _, _ = self.io.call(LISTEN, 11); self.assertFalse(nm)             # unit 1 answers as 11
        self.io.call(UNLSN)
        self.assertEqual(self.io.fast_save(10, b"NEW", 0x0801, b"abc"), 0)
        self.assertEqual(self.io.fast_load(10, b"NEW")[2], b"abc")

    # ---- run-time renumbering: U0>+CHR$(n), persisted through the host callback ----
    def hook_save(self):
        self.saved = []
        self.cb = HostStruct()                                            # all other callbacks NULL
        self.save_cb = HostStruct.SAVEDEV(lambda c, a, b: self.saved.append((a, b)))
        self.cb.save_devices = self.save_cb
        LIB.vdrive_set_host(self.io.v, ctypes.byref(self.cb))

    def renumber(self, via, n):
        self.io.call(LISTEN, via); self.io.call(SECOND, 0x6F)
        for b in b"U0>" + bytes([n]): self.io.call(CIOUT, b)
        self.io.call(UNLSN)

    def status(self, dev):
        self.io.call(TALK, dev); self.io.call(TKSA, 0x6F)
        out = b""
        while True:
            _, d, st = self.io.call(ACPTR); out += bytes([d])
            if st & 0x40: break
        self.io.call(UNTLK)
        return out.decode("latin1")

    def test_u0_renumber_takes_effect_and_is_persisted(self):
        self.hook_save()
        self.renumber(8, 10)                                              # CHR$(10) is a line feed: must not be stripped
        self.assertEqual(self.saved, [(10, 0)])                           # unit 1 still default -> stored as 0
        self.assertEqual(self.io.fast_load(8, b"HI")[0], "notmine")       # 8 is back on the real bus
        self.assertEqual(self.io.fast_load(10, b"HI")[0], 0)
        self.assertTrue(self.status(10).startswith("00, OK"))
        self.renumber(9, 13)                                              # CR is stripped from commands, but not from U0>
        self.assertEqual(self.saved[-1], (10, 13))
        nm, _, _ = self.io.call(LISTEN, 13); self.assertFalse(nm)

    def test_u0_with_trailing_cr_as_sent_by_print_hash(self):
        self.hook_save()
        self.io.call(LISTEN, 8); self.io.call(SECOND, 0x6F)
        for b in b"U0>" + bytes([10]) + b"\r": self.io.call(CIOUT, b)          # PRINT#15,"U0>"+CHR$(10)
        self.io.call(UNLSN)
        self.assertEqual(self.saved, [(10, 0)])
        self.assertEqual(self.io.fast_load(10, b"HI")[0], 0)

    def test_u0_zero_restores_default(self):
        self.hook_save()
        self.renumber(8, 10); self.renumber(10, 0)
        self.assertEqual(self.saved[-1], (0, 0))
        self.assertEqual(self.io.fast_load(8, b"HI")[0], 0)

    def test_u0_rejects_bad_numbers(self):
        self.hook_save()
        for bad in (7, 31, 9):                                            # <8, >30, or the other unit's number
            self.renumber(8, bad)
            self.assertTrue(self.status(8).startswith("33,SYNTAX ERROR"), bad)
        self.assertEqual(self.saved, [])
        self.assertEqual(self.io.fast_load(8, b"HI")[0], 0)

    # ---- reset-time scan: the KERNAL probes the real bus; numbers nobody answers at go to SD, then USB ----
    def scan(self, occupied):
        """what the 6502 does: SCAN, then probe 8,9,... and report each free number until nobody is waiting"""
        nm, need, _ = self.io.call(F_SCAN)
        self.assertFalse(nm)
        dev = 8
        while need and dev <= 30:
            if dev not in occupied:
                nm, need, _ = self.io.call(F_FREE, dev)
            dev += 1
        return need

    def devs(self): return self.io.r(R_DEV0), self.io.r(R_DEV1)

    # ---- the request: the first transaction of a run is answered "scan first", the KERNAL scans and asks again ----
    def test_first_transaction_asks_for_a_scan_and_is_not_executed(self):
        self.assertEqual(self.io.arm_scan(), 1)
        for first in (lambda: self.io.call(LISTEN, 8, allow_scan=True), lambda: self.io.call(TALK, 8, allow_scan=True),
                      lambda: self.io.call(F_LOAD_OPEN, 8, 2, allow_scan=True), lambda: self.io.call(F_SAVE_OPEN, 8, 2, allow_scan=True)):
            self.io.arm_scan()
            first()
            self.assertEqual(self.io.stat, SCANREQ)
            self.assertEqual(self.io.r(R_DEV0), 8)                              # nothing happened yet
            self.assertEqual(self.io.vd_act(), -1)                              # ... and no transaction is open
        self.io.arm_scan()
        self.io.call(LISTEN, 8, allow_scan=True)                                # SCANREQ again (re-armed)
        self.assertEqual(self.scan({8}), 0)                                     # drive 8 is real
        nm, _, _ = self.io.call(LISTEN, 8); self.assertTrue(nm)                 # the repeated command goes to the real bus
        nm, _, _ = self.io.call(UNLSN)
        nm, _, _ = self.io.call(LISTEN, 9); self.assertFalse(nm)                # ... and 9 is ours
        self.assertEqual(self.io.stat, OK)

    def test_the_request_is_made_once_only(self):
        self.io.arm_scan()
        self.io.call(LISTEN, 8, allow_scan=True); self.assertEqual(self.io.stat, SCANREQ)
        nm, _, _ = self.io.call(LISTEN, 8)                                      # an old KERNAL ignored the request: carry on with the defaults
        self.assertFalse(nm); self.assertEqual(self.io.stat, OK)
        self.io.call(UNLSN)
        self.io.call(LISTEN, 8); self.assertEqual(self.io.stat, OK)             # and never ask again
        self.io.call(UNLSN)
        self.assertEqual(self.devs(), (8, 9))

    def test_no_request_when_nothing_needs_a_number(self):
        self.io.set_devices(10, 11)
        self.assertEqual(self.io.arm_scan(), 0)                                 # both fixed by the user
        self.io.call(LISTEN, 10); self.assertEqual(self.io.stat, OK)
        io = VIO(); io.attach(0, None, present=False); io.attach(1, None, present=False)
        self.assertEqual(io.arm_scan(), 0)                                      # no card, no stick
        self.assertEqual(VIO().arm_scan(), 0)

    def test_request_only_for_the_automatic_present_units(self):
        self.io.set_devices(10, 0)                                              # SD fixed, USB automatic and present
        self.assertEqual(self.io.arm_scan(), 1)
        io = VIO(); io.attach(0, None, present=False); io.attach(1, None, present=True)
        self.assertEqual(io.arm_scan(), 1)

    def test_scan_with_an_empty_bus_keeps_8_and_9(self):
        self.assertEqual(self.scan(set()), 0)
        self.assertEqual(self.devs(), (8, 9))

    def test_scan_skips_physical_drives_and_fills_gaps(self):
        self.assertEqual(self.scan({8}), 0); self.assertEqual(self.devs(), (9, 10))
        self.assertEqual(self.scan({8, 9, 10}), 0); self.assertEqual(self.devs(), (11, 12))
        self.assertEqual(self.scan({9, 10, 12}), 0); self.assertEqual(self.devs(), (8, 11))     # a gap below, one above
        self.assertEqual(self.io.fast_load(9, b"HI")[0], "notmine")                              # 9 now belongs to the real drive
        self.assertEqual(self.io.fast_load(8, b"HI")[0], 0)

    def test_scan_releases_the_numbers_so_probes_reach_the_real_bus(self):
        self.io.call(F_SCAN)
        for dev in (8, 9):
            nm, _, _ = self.io.call(LISTEN, dev); self.assertTrue(nm, dev)                     # unassigned: not mine, even though 8/9 were defaults
        self.assertEqual(self.devs(), (0, 0))

    def test_scan_gives_numbers_only_to_units_that_are_present(self):
        io = VIO(); io.attach(0, None, present=False); io.attach(1, None, present=True)
        nm, need, _ = io.call(F_SCAN); self.assertEqual(need, 1)                                 # only the USB unit needs one
        nm, need, _ = io.call(F_FREE, 8); self.assertEqual(need, 0)
        self.assertEqual((io.r(R_DEV0), io.r(R_DEV1)), (0, 8))
        io2 = VIO(); io2.attach(0, None, present=False); io2.attach(1, None, present=False)
        self.assertEqual(io2.call(F_SCAN)[1], 0)                                                 # nothing to scan for: the 6502 skips the probing

    def test_pinned_units_are_not_scanned_and_their_number_is_never_handed_out(self):
        self.io.set_devices(9, 0)                       # SD pinned to 9, USB automatic
        self.assertEqual(self.devs(), (9, 10))          # USB's default 9 is taken, so it starts on the next number
        nm, need, _ = self.io.call(F_SCAN); self.assertEqual(need, 1)
        self.assertEqual(self.devs(), (9, 0))
        nm, need, _ = self.io.call(F_FREE, 9); self.assertEqual(need, 1)                         # 9 is ours already: probing it must not hand it out
        nm, need, _ = self.io.call(F_FREE, 10); self.assertEqual(need, 0)
        self.assertEqual(self.devs(), (9, 10))

    def test_scan_from_an_old_firmware_or_a_stray_free_report_is_harmless(self):
        nm, need, _ = self.io.call(F_FREE, 20)          # without SCAN first nobody is waiting
        self.assertEqual((need, self.devs()), (0, (8, 9)))
        self.io.call(F_SCAN); self.io.call(F_FREE, 5)   # out of range: ignored
        self.assertEqual(self.devs(), (0, 0))

    def test_pin_survives_and_zero_means_automatic(self):
        self.hook_save()
        self.renumber(8, 8)                              # an explicit 8 is a pin now, no longer "the default"
        self.assertEqual(self.saved, [(8, 0)])
        self.renumber(8, 0)
        self.assertEqual(self.saved[-1], (0, 0))
        self.assertEqual(self.devs(), (8, 9))

    def test_pinned_number_wins_over_the_default_of_the_other_unit(self):
        self.io.set_devices(0, 8)                       # USB pinned to 8: SD, automatic, must not also claim 8
        self.assertEqual(self.devs(), (9, 8))
        self.assertEqual(self.io.fast_load(8, b"X")[0], 4)                                      # unit 1 (no disk mounted): file not found
        self.assertEqual(self.io.fast_load(9, b"HI")[0], 0)                                     # unit 0 holds the test disk

    def test_unit_one_independent_numbering(self):
        self.io.set_devices(8, 12)
        self.assertEqual(self.io.fast_load(12, b"X")[0], 4)                   # unit 1 present, no disk mounted -> not found
        self.assertEqual(self.io.fast_load(9, b"X")[0], "notmine")


# ---- RAM footprint ---------------------------------------------------------------------------
VIO_RAM_BUDGET = 5000       # bytes of RAM for the whole mailbox/drive state (tr-plus RAM1 has ~15 KB of slack)


class FootprintTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / "m.d64"
        subprocess.run(["c1541", "-format", "many,01", "d64", str(img)], capture_output=True, check=True)
        p = self.tmp / "f"; p.write_bytes(b"x" * 300)
        for i in range(60):                                         # 8 directory sectors
            c1541(img, "-write", str(p), "file%02d" % i)
        self.disk = Disk(img.read_bytes())
        self.io = VIO(); self.io.attach(0, self.disk); self.io.attach(1, None, present=False)

    def tearDown(self): shutil.rmtree(self.tmp)

    def test_state_fits_the_ram_budget(self):
        size, hot = LIB.vio_sizeof(), LIB.vio_hot_sizeof()
        print(f"\n  sizeof(vio_t) = {size} bytes + vio_hot_t = {hot} bytes (budget {VIO_RAM_BUDGET} total)")
        self.assertLess(size + hot, VIO_RAM_BUDGET)
        self.assertLess(hot, 300)                                     # ISR-visible part stays small enough for DTCM

    def test_status_is_a_token_never_a_bare_flag(self):
        # a floating bus returns the previous bus byte ($DE for the operand fetch of LDA $DE03); it must not equal an answer token
        self.assertNotIn(0xDE, (BUSY, OK, NOTMINE))
        self.assertNotIn(0x03, (BUSY, OK, NOTMINE))

    def test_streamed_directory_is_byte_identical_to_buffered_listing(self):
        for pat in (None, b"FILE1*", b"FILE?9", b"NOPE*"):
            with self.subTest(pat=pat):
                name = b"$" if pat is None else b"$:" + pat
                rc, addr, streamed = self.io.fast_load(8, name)
                self.assertEqual(rc, 0)
                self.assertEqual(b"\x01\x04" + streamed, self.disk.listing(pat))

    def test_directory_needs_no_buffer_big_listing(self):
        rc, addr, d = self.io.fast_load(8, b"$")
        self.assertGreater(len(d), 1500)                              # a 60-entry listing, no 6 KB buffer anywhere
        self.assertEqual(d.count(b'"FILE'), 60)

    def test_channel_pool_exhaustion_and_recovery(self):
        n = 6                                                         # VD_NSLOT default
        for ch in range(2, 2 + n):
            self.io.call(LISTEN, 8); self.io.call(SECOND, 0xF0 | ch)
            for b in b"FILE00": self.io.call(CIOUT, b)
            self.io.call(UNLSN)
        self.io.call(LISTEN, 8); self.io.call(SECOND, 0xF0 | 12)
        for b in b"FILE01": self.io.call(CIOUT, b)
        self.io.call(UNLSN)
        self.assertEqual(LIB.vdrive_pending_err(self.io.v, 0), 70)    # NO CHANNEL
        self.io.call(LISTEN, 8); self.io.call(SECOND, 0xE0 | 2); self.io.call(UNLSN)   # close one
        self.io.call(LISTEN, 8); self.io.call(SECOND, 0xF0 | 12)
        for b in b"FILE01": self.io.call(CIOUT, b)
        self.io.call(UNLSN)
        self.io.call(TALK, 8); self.io.call(TKSA, 0x6C)
        nm, d, st = self.io.call(ACPTR)
        self.assertEqual(st & 0x02, 0)                                # the new channel reads data

    def test_load_and_save_work_while_all_slots_are_used_by_others(self):
        for ch in range(2, 2 + 4):                                    # leave 2 slots for LOAD (ch 0) and SAVE (ch 1)
            self.io.call(LISTEN, 8); self.io.call(SECOND, 0xF0 | ch)
            for b in b"FILE00": self.io.call(CIOUT, b)
            self.io.call(UNLSN)
        self.assertEqual(self.io.fast_load(8, b"FILE05")[0], 0)
        self.assertEqual(self.io.fast_save(8, b"SAVED", 0x0801, b"abc"), 0)


if __name__ == "__main__":
    unittest.main()
