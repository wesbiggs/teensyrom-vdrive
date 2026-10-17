import ctypes, pathlib, random, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'tools'))
from d64py import LIB, Disk

E_FULL, E_EXISTS, E_NAME = -3, -4, -5


def c1541(img, *args):
    r = subprocess.run(["c1541", "-attach", str(img), *args], capture_output=True, text=True, encoding="latin1")
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def bam_bytes(image):          # 18/0 BAM area (tracks 1-35)
    off = 357 * 256
    return image[off + 4: off + 0x90]


class WriteTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / "blank.d64"
        c1541(img, "-format", "blank,bk", "d64", str(img)) if False else subprocess.run(
            ["c1541", "-format", "blank,bk", "d64", str(img)], capture_output=True, check=True)
        self.blank = img.read_bytes()
        self.d = Disk(self.blank)
        self.rnd = random.Random(7)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def data(self, n): return bytes(self.rnd.randrange(256) for _ in range(n))

    def img_file(self):
        p = self.tmp / "out.d64"; p.write_bytes(self.d.dump()); return p

    def assert_valid(self):
        """c1541 `validate` rebuilds the BAM from file chains; ours must already match."""
        p = self.img_file()
        before = bam_bytes(p.read_bytes())
        c1541(p, "-validate")
        self.assertEqual(bam_bytes(p.read_bytes()), before, "BAM differs from what validate computes")

    # ---- creation & read-back ----
    def test_roundtrip_sizes_read_by_c1541(self):
        sizes = [0, 1, 2, 253, 254, 255, 508, 509, 1000, 30000]
        for i, n in enumerate(sizes):
            name = b"F%02d" % i
            payload = self.data(n)
            self.assertEqual(self.d.write_file(name, payload, chunk=77), 0, name)
            self.assertEqual(self.d.read_file(name), payload)
        self.assert_valid()
        p = self.img_file()
        for i, n in enumerate(sizes):
            if n == 0: continue          # c1541 -read returns 254 bytes for an empty file (its quirk); on-disk format is identical
            out = self.tmp / f"x{i}"
            c1541(p, "-read", "f%02d" % i, str(out))
            # c1541 read of a PRG keeps raw data; compare to what we read back through our own reader
            self.assertEqual(out.read_bytes(), self.d.read_file(b"F%02d" % i))

    def test_blocks_and_free_match_c1541(self):
        for n in (1, 254, 255, 5000):
            self.assertEqual(self.d.write_file(b"B%d" % n, self.data(n)), 0)
        p = self.img_file()
        out = c1541(p, "-list")
        free = int([l for l in out.splitlines() if "blocks free" in l.lower()][0].split()[0])
        self.assertEqual(self.d.free(), free)
        ents = {e["name"]: e["blocks"] for e in self.d.entries()}
        self.assertEqual(ents[b"B1"], 1); self.assertEqual(ents[b"B254"], 1)
        self.assertEqual(ents[b"B255"], 2); self.assertEqual(ents[b"B5000"], 20)

    def test_file_is_closed_prg_in_directory(self):
        self.d.write_file(b"HELLO", b"abc", type=2)
        self.d.write_file(b"SEQ", b"abc", type=1)
        t = {e["name"]: e["type"] for e in self.d.entries()}
        self.assertEqual(t[b"HELLO"], 0x82); self.assertEqual(t[b"SEQ"], 0x81)
        self.assertIn(b'"HELLO"', self.d.listing())

    # ---- errors ----
    def test_exists_and_replace(self):
        self.assertEqual(self.d.write_file(b"A", b"one"), 0)
        free = self.d.free()
        self.assertEqual(self.d.write_file(b"A", b"two"), E_EXISTS)
        self.assertEqual(self.d.free(), free)                        # failed create leaks nothing
        self.assertEqual(self.d.write_file(b"A", self.data(600), replace=True), 0)
        self.assertEqual(len(self.d.read_file(b"A")), 600)
        self.assertEqual(sum(1 for e in self.d.entries() if e["name"] == b"A"), 1)
        self.assert_valid()

    def test_bad_names(self):
        for bad in (b"", b"A" * 17, b"A*", b"A?B", b"A,B", b"A:B"):
            self.assertEqual(self.d.write_file(bad, b"x"), E_NAME, bad)

    def test_disk_full_cleans_up(self):
        free0 = self.d.free()
        self.assertEqual(self.d.write_file(b"KEEP", self.data(1000)), 0)
        free1 = self.d.free()
        r = self.d.write_file(b"TOOBIG", self.data(free1 * 254 + 500), chunk=4096)
        self.assertEqual(r, E_FULL)
        self.assertEqual(self.d.free(), free1)                         # every sector returned
        self.assertIsNone(self.d.read_file(b"TOOBIG"))
        self.assertEqual({e["name"] for e in self.d.entries()}, {b"KEEP"})
        self.assert_valid()

    def test_fill_exactly_then_full(self):
        free = self.d.free()
        self.assertEqual(self.d.write_file(b"FILL", self.data(free * 254 - 1 * 254)), 0)   # leaves 1 block
        self.assertEqual(self.d.free(), 1)
        self.assertEqual(self.d.write_file(b"LAST", b"x"), 0)
        self.assertEqual(self.d.free(), 0)
        self.assertEqual(self.d.write_file(b"NOPE", b"x"), E_FULL)
        self.assert_valid()

    # ---- scratch / rename / format ----
    def test_scratch_frees_and_reuses(self):
        base = self.d.free()
        for n in range(6):
            self.d.write_file(b"S%d" % n, self.data(2000))
        used = base - self.d.free()
        self.assertEqual(self.d.scratch(b"S3"), 1)
        self.assertEqual(self.d.scratch(b"S*"), 5)
        self.assertEqual(self.d.free(), base)
        self.assertEqual(self.d.entries(), [])
        self.assertGreater(used, 0)
        self.assert_valid()
        self.assertEqual(self.d.write_file(b"AGAIN", self.data(3000)), 0)   # reuses freed sectors/slots
        self.assert_valid()

    def test_scratch_locked_is_skipped(self):
        self.d.write_file(b"LOCK", b"x")
        r, e = self.d.find(b"LOCK")
        # set lock bit directly in dir sector
        off = LIB.d64_sector_offset(self.d.d, 18, 1)
        self.d.image[off + 2] = bytes([self.d.image[off + 2][0] | 0x40])
        self.assertEqual(self.d.scratch(b"LOCK"), 0)
        self.assertIsNotNone(self.d.read_file(b"LOCK"))

    def test_rename(self):
        self.d.write_file(b"OLD", b"data")
        self.d.write_file(b"OTHER", b"x")
        self.assertEqual(self.d.rename(b"OLD", b"NEW"), 0)
        self.assertIsNone(self.d.read_file(b"OLD"))
        self.assertEqual(self.d.read_file(b"NEW"), b"data")
        self.assertEqual(self.d.rename(b"NEW", b"OTHER"), E_EXISTS)
        self.assertEqual(self.d.rename(b"GONE", b"Z"), -8)

    def test_directory_extends_beyond_one_sector(self):
        for i in range(40):                                  # 8 per sector => 5 dir sectors
            self.assertEqual(self.d.write_file(b"D%02d" % i, b"x" * (i + 1)), 0)
        self.assertEqual(len(self.d.entries()), 40)
        for i in range(40):
            self.assertEqual(self.d.read_file(b"D%02d" % i), b"x" * (i + 1))
        self.assert_valid()
        out = c1541(self.img_file(), "-list")
        self.assertIn("d39", out)

    def test_directory_limit_144(self):
        n = 0
        while self.d.write_file(b"E%03d" % n, b"x") == 0:
            n += 1
            if n > 200: break
        self.assertEqual(n, 144)
        self.assert_valid()

    def test_format_full_and_quick(self):
        self.d.write_file(b"X", self.data(5000))
        self.assertEqual(self.d.format(b"NEWDISK", b"ID"), 0)
        self.assertEqual(self.d.header(), (b"NEWDISK", b"ID", b"2A"))
        self.assertEqual(self.d.entries(), [])
        self.assertEqual(self.d.free(), 664)
        self.assert_valid()
        self.d.write_file(b"Y", b"abc")
        self.assertEqual(self.d.format(b"QUICK"), 0)                 # quick keeps id
        self.assertEqual(self.d.header(), (b"QUICK", b"ID", b"2A"))
        self.assertEqual(self.d.entries(), [])
        self.assertEqual(self.d.free(), 664)
        c1541(self.img_file(), "-list")

    def test_format_matches_c1541_bam(self):
        ref = bam_bytes(self.blank)
        self.d.write_file(b"JUNK", self.data(900))
        self.d.format(b"BLANK", b"BK")
        self.assertEqual(bam_bytes(self.d.dump()), ref)             # identical BAM to a c1541-formatted disk

    def test_read_only_disk(self):
        ro = Disk(self.blank)
        ro.d.raw  # keep ref
        ctypes.memset(ctypes.addressof(ro.d) + ctypes.sizeof(ctypes.c_void_p), 0, ctypes.sizeof(ctypes.c_void_p))  # write fn = NULL
        self.assertEqual(ro.write_file(b"A", b"x"), -6)


if __name__ == "__main__":
    unittest.main()
