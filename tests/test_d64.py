import ctypes, os, re, random, shutil, subprocess, tempfile, unittest, pathlib

import sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'tools'))
from d64py import LIB, Disk


def c1541(*args):
    r = subprocess.run(["c1541", *args], capture_output=True, text=True, encoding="latin1")
    assert r.returncode == 0, r.stdout + r.stderr


class D64Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp())
        rnd = random.Random(64)
        cls.files = {}
        sizes = {"ONE": 1, "SMALL": 10, "FULL254": 254, "OVER255": 255, "MULTI": 1000,
                 "BIG": 60000, "HUGE": 100000}
        for i in range(10):
            sizes[f"F{i:02d}"] = 300 + i
        img = cls.tmp / "t.d64"
        c1541("-format", "testdisk,td", "d64", str(img))
        for name, size in sizes.items():
            data = bytes(rnd.randrange(256) for _ in range(size))
            p = cls.tmp / (name + ".prg"); p.write_bytes(data)
            cls.files[name.encode()] = data
            c1541("-attach", str(img), "-write", str(p), name.lower())
        p = cls.tmp / "seq.bin"; p.write_bytes(b"HELLO\rWORLD\r")
        c1541("-attach", str(img), "-write", str(p), "seqfile,s")
        cls.files[b"SEQFILE"] = b"HELLO\rWORLD\r"
        c1541("-attach", str(img), "-delete", "f03")      # scratched entry
        del cls.files[b"F03"]
        cls.image = img.read_bytes()
        cls.disk = Disk(cls.image)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def test_header(self):
        self.assertEqual(self.disk.header(), (b"TESTDISK", b"TD", b"2A"))

    def test_directory_matches_files(self):
        got = {e["name"]: e for e in self.disk.entries()}
        self.assertEqual(set(got), set(self.files))           # F03 scratched; 2nd dir sector walked
        self.assertGreater(len(got), 8)

    def test_file_contents(self):
        for name, data in self.files.items():
            for chunk in (1, 100, 256, 5000):
                with self.subTest(name=name, chunk=chunk):
                    got = self.disk.read_file(name, chunk)
                    # c1541 stores PRG data after its own 2-byte load address? no: raw bytes as given
                    self.assertEqual(got, data)

    def test_scratched_not_found(self):
        self.assertEqual(self.disk.find(b"F03")[0], 0)

    def test_wildcards(self):
        self.assertEqual(self.disk.find(b"F0*")[0], 1)
        self.assertEqual(self.disk.find(b"FULL2?4")[0], 1)
        self.assertEqual(self.disk.find(b"NOPE*")[0], 0)

    def _ref(self):
        r = subprocess.run(["c1541", "-attach", str(self.tmp / "t.d64"), "-list"],
                           capture_output=True, text=True, encoding="latin1")
        ents, free = {}, None
        for ln in r.stdout.splitlines():
            m = re.match(r'\s*(\d+)\s+"([^"]*)"', ln)
            if m: ents[m.group(2).upper().encode("latin1")] = int(m.group(1))
            m = re.match(r"\s*(\d+) blocks free", ln, re.I)
            if m: free = int(m.group(1))
        return ents, free

    def test_blocks_free_matches_c1541(self):
        ents, free = self._ref()
        self.assertIsNotNone(free)
        self.assertEqual(LIB.d64_blocks_free(self.disk.d), free)

    def test_block_counts_match_c1541(self):
        ents, free = self._ref()
        # first entry of -list is the disk header (blocks=0)
        ents.pop(b"TESTDISK        ", None)
        got = {e["name"]: e["blocks"] for e in self.disk.entries()}
        self.assertEqual(got, ents)

    def test_basic_listing_structure(self):
        lst = self.disk.listing()
        self.assertEqual(lst[:2], b"\x01\x04")
        # walk the link chain: each link must point at the next line, last = 0
        pos, addr = 2, 0x0401
        lines = []
        while True:
            link = lst[pos] | lst[pos + 1] << 8
            if link == 0: break
            self.assertEqual(link, 0x0401 + pos + (lst[pos + 4:].index(0) + 5) - 2)
            num = lst[pos + 2] | lst[pos + 3] << 8
            text = lst[pos + 4: lst.index(0, pos + 4)]
            lines.append((num, text))
            pos = link - 0x0401 + 2
        self.assertEqual(lines[0][1], b'\x12"TESTDISK        " TD 2A')
        self.assertTrue(lines[-1][1].startswith(b"BLOCKS FREE."))
        self.assertEqual(lines[-1][0], LIB.d64_blocks_free(self.disk.d))
        names = [t.split(b'"')[1] for _, t in lines[1:-1]]
        self.assertEqual(set(names), set(self.files))
        big = [t for n, t in lines if b'"BIG' in t][0]
        self.assertTrue(big.endswith(b" PRG "), big)
        seq = [t for n, t in lines if b"SEQFILE" in t][0]
        self.assertIn(b"SEQ", seq)

    def test_listing_with_pattern(self):
        lst = self.disk.listing(b"F0?")
        self.assertIn(b'"F00"', lst)
        self.assertNotIn(b'"BIG"', lst)

    def _off(self, d, t, s):
        return LIB.d64_sector_offset(d.d, t, s)

    def test_file_chain_loop_is_an_error(self):
        d = Disk(self.image)
        r, e = d.find(b"MULTI")
        t, s = e.raw[1], e.raw[2]
        off = self._off(d, t, s)
        d.image[off], d.image[off + 1] = t, s            # sector links to itself
        f = ctypes.create_string_buffer(LIB.d64_sizeof_file())
        LIB.d64_file_open(d.d, e, f)
        buf = ctypes.create_string_buffer(300000)
        self.assertEqual(LIB.d64_file_read(d.d, f, buf, ctypes.c_size_t(300000)), -2)

    def test_dir_chain_loop_is_an_error(self):
        d = Disk(self.image)
        off = self._off(d, 18, 1)
        d.image[off], d.image[off + 1] = 18, 1
        it = ctypes.create_string_buffer(LIB.d64_sizeof_dir())
        e = ctypes.create_string_buffer(LIB.d64_sizeof_entry())
        LIB.d64_dir_first(d.d, it)
        rs = []
        for _ in range(1000):
            r = LIB.d64_dir_next(d.d, it, e)
            rs.append(r)
            if r <= 0: break
        self.assertEqual(rs[-1], -2)

    def test_bad_link_track_is_an_error(self):
        d = Disk(self.image)
        r, e = d.find(b"SMALL")
        off = self._off(d, e.raw[1], e.raw[2])
        d.image[off] = 99                                 # next track out of range
        f = ctypes.create_string_buffer(LIB.d64_sizeof_file())
        LIB.d64_file_open(d.d, e, f)
        buf = ctypes.create_string_buffer(1000)
        self.assertEqual(LIB.d64_file_read(d.d, f, buf, ctypes.c_size_t(1000)), -1)

    def test_geometry(self):
        self.assertEqual(self._off(self.disk, 1, 0), 0)
        self.assertEqual(self._off(self.disk, 18, 0), 357 * 256)
        self.assertEqual(self._off(self.disk, 35, 16), 682 * 256)
        self.assertEqual(self._off(self.disk, 35, 17), -1)
        self.assertEqual(self._off(self.disk, 36, 0), -1)


if __name__ == "__main__":
    unittest.main()
