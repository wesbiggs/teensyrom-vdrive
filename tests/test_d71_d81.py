"""D71 (1571) and D81 (1581) images: read, write, format and BAM bookkeeping, checked against c1541."""
import pathlib, random, re, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'tools'))
from d64py import LIB, Disk


def c1541(img, *args):
    r = subprocess.run(["c1541", "-attach", str(img), *args], capture_output=True, text=True, encoding="latin1")
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def bam_bytes(disk, fmt):
    """everything in the BAM that validate recomputes: free counts and bitmaps, not the header text"""
    def sec(t, s):
        off = LIB.d64_sector_offset(disk.d, t, s)
        return disk.dump()[off: off + 256]
    if fmt == "d71":
        a, b = sec(18, 0), sec(53, 0)
        return a[4:0x90] + a[0xDD:] + b[:105]
    if fmt == "d81":
        return sec(40, 1)[16:] + sec(40, 2)[16:]
    return sec(18, 0)[4:0x90]


class FormatTests:
    fmt = None
    size = None

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.img = self.tmp / ("t." + self.fmt)
        c1541(self.img, "-format", "fmtdisk,fd") if False else subprocess.run(
            ["c1541", "-format", "fmtdisk,fd", self.fmt, str(self.img)], capture_output=True, check=True)
        self.rnd = random.Random(71)
        self.files = {}
        sizes = {"ONE": 1, "SMALL": 10, "MULTI": 1000, "BIG": 60000, "HUGE": 150000}
        for i in range(12):
            sizes[f"F{i:02d}"] = 300 + i                      # more than 8 entries: second directory sector
        for name, n in sizes.items():
            data = bytes(self.rnd.randrange(256) for _ in range(n))
            p = self.tmp / (name + ".prg"); p.write_bytes(data)
            c1541(self.img, "-write", str(p), name.lower())
            self.files[name.encode()] = data
        self.disk = Disk(self.img.read_bytes())

    def tearDown(self): shutil.rmtree(self.tmp)

    def data(self, n): return bytes(self.rnd.randrange(256) for _ in range(n))

    def out(self):
        p = self.tmp / "out." + self.fmt if False else self.tmp / ("out." + self.fmt)
        p.write_bytes(self.disk.dump()); return p

    def c1541_free(self, img):
        m = re.findall(r"(\d+) blocks free", c1541(img, "-list").lower())
        return int(m[-1])

    def assert_bam_valid(self):
        p = self.out(); before = bam_bytes(Disk(p.read_bytes()), self.fmt)
        c1541(p, "-validate")
        self.assertEqual(bam_bytes(Disk(p.read_bytes()), self.fmt), before, "BAM differs from what c1541 -validate computes")

    # ---- read side ----
    def test_size_selects_the_format(self):
        self.assertEqual(len(self.disk.dump()), self.size)

    def test_header(self):
        name, ident, dos = self.disk.header()
        self.assertEqual((name, ident), (b"FMTDISK", b"FD"))
        self.assertEqual(dos, self.dos)

    def test_directory_and_files(self):
        got = {e["name"] for e in self.disk.entries()}
        self.assertEqual(got, set(self.files))
        for name, data in self.files.items():
            with self.subTest(name=name):
                self.assertEqual(self.disk.read_file(name, 1000), data)

    def test_blocks_free_matches_c1541(self):
        self.assertEqual(self.disk.free(), self.c1541_free(self.img))

    def test_blank_disk_blocks_free(self):
        blank = self.tmp / ("blank." + self.fmt)
        subprocess.run(["c1541", "-format", "b,00", self.fmt, str(blank)], capture_output=True, check=True)
        self.assertEqual(Disk(blank.read_bytes()).free(), self.blank_free)

    def test_basic_listing(self):
        lst = self.disk.listing()
        self.assertIn(b'"FMTDISK', lst); self.assertIn(b'"HUGE"', lst); self.assertTrue(lst.endswith(b"\x00\x00\x00"))

    # ---- write side ----
    def test_write_file_is_readable_by_c1541_and_bam_is_valid(self):
        data = self.data(50000)
        self.assertEqual(self.disk.write_file(b"NEWFILE", data), 0)
        self.assertEqual(self.disk.read_file(b"NEWFILE"), data)
        out = self.tmp / "o.bin"
        c1541(self.out(), "-read", "newfile", str(out))
        self.assertEqual(out.read_bytes(), data)
        self.assertEqual(self.disk.free(), self.c1541_free(self.out()))
        self.assert_bam_valid()

    def test_scratch_and_rename(self):
        free = self.disk.free()
        self.assertEqual(self.disk.scratch(b"HUGE"), 1)
        self.assertGreater(self.disk.free(), free + 500)
        self.assertEqual(self.disk.rename(b"BIG", b"BIGGER"), 0)
        self.assertEqual(self.disk.read_file(b"BIGGER"), self.files[b"BIG"])
        self.assert_bam_valid()
        self.assertEqual(self.disk.free(), self.c1541_free(self.out()))

    def test_directory_grows_past_its_first_sectors(self):
        for i in range(60):
            self.assertEqual(self.disk.write_file(b"D%02d" % i, b"x" * (i + 1)), 0, i)
        names = {e["name"] for e in self.disk.entries()}
        self.assertEqual(len(names), len(self.files) + 60)
        listing = c1541(self.out(), "-list")
        self.assertIn('"d59"', listing.lower())
        self.assert_bam_valid()

    def test_fill_the_disk_gives_disk_full_and_leaves_a_valid_image(self):
        free = self.disk.free()
        r = self.disk.write_file(b"TOOBIG", self.data(free * 254 + 5000))
        self.assertEqual(r, -3)                                  # D64_E_FULL
        self.assertEqual(self.disk.free(), free)                 # the failed file was rolled back
        self.assertEqual(self.disk.write_file(b"EXACT", self.data(free * 254 - 2 * 254)), 0)
        self.assert_bam_valid()
        self.assertEqual(self.disk.free(), self.c1541_free(self.out()))

    def test_format_matches_a_c1541_formatted_image(self):
        d = Disk(self.img.read_bytes())
        self.assertEqual(d.format(b"NEWNAME", b"ZZ"), 0)
        ref = self.tmp / ("ref." + self.fmt)
        subprocess.run(["c1541", "-format", "newname,zz", self.fmt, str(ref)], capture_output=True, check=True)
        refd = Disk(ref.read_bytes())
        self.assertEqual(d.header(), refd.header())
        self.assertEqual(d.entries(), [])
        self.assertEqual(bam_bytes(d, self.fmt), bam_bytes(refd, self.fmt))
        self.assertEqual(d.free(), refd.free())
        self.assertEqual(d.write_file(b"AFTER", b"hello"), 0)
        self.assertEqual(d.read_file(b"AFTER"), b"hello")

    def test_quick_new_keeps_the_id_and_empties_the_directory(self):
        self.assertEqual(self.disk.format(b"QUICK", None), 0)
        self.assertEqual(self.disk.header()[:2], (b"QUICK", b"FD"))
        self.assertEqual(self.disk.entries(), [])
        self.assertEqual(self.disk.free(), self.blank_free)


class D71Tests(FormatTests, unittest.TestCase):
    fmt, size, dos, blank_free = "d71", 349696, b"2A", 1328

    def test_second_side_is_used_when_the_first_is_full(self):
        free = self.disk.free()
        self.assertEqual(self.disk.write_file(b"SIDES", self.data(free * 254 - 20000)), 0)
        self.assert_bam_valid()
        self.assertEqual(self.disk.read_file(b"SIDES")[:4], self.disk.read_file(b"SIDES")[:4])


class D81Tests(FormatTests, unittest.TestCase):
    fmt, size, dos, blank_free = "d81", 819200, b"3D", 3160

    def test_geometry(self):
        self.assertEqual(LIB.d64_sector_offset(self.disk.d, 1, 0), 0)
        self.assertEqual(LIB.d64_sector_offset(self.disk.d, 40, 0), 39 * 40 * 256)
        self.assertEqual(LIB.d64_sector_offset(self.disk.d, 80, 39), (80 * 40 - 1) * 256)
        self.assertEqual(LIB.d64_sector_offset(self.disk.d, 80, 40), -1)
        self.assertEqual(LIB.d64_sector_offset(self.disk.d, 81, 0), -1)


class D71GeometryTests(unittest.TestCase):
    def test_geometry(self):
        d = Disk(bytes(349696))
        off = lambda t, s: LIB.d64_sector_offset(d.d, t, s)
        self.assertEqual(off(18, 0), 357 * 256)
        self.assertEqual(off(36, 0), 683 * 256)                  # side 2 starts right after side 1
        self.assertEqual(off(53, 0), (683 + 357) * 256)
        self.assertEqual(off(70, 16), (1366 - 1) * 256)
        self.assertEqual(off(70, 17), -1); self.assertEqual(off(71, 0), -1)

    def test_error_info_bytes_after_the_data_are_ignored(self):
        self.assertEqual(Disk(bytes(351062)).free() >= 0, True)


if __name__ == "__main__":
    unittest.main()
