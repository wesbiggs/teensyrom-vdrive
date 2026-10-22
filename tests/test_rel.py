"""REL files through the bus protocol (OPEN ",L,", P, PRINT#, INPUT#), checked against an independent reading of the side sectors
and against c1541 -validate (which rebuilds the BAM from the data chains and the side sectors of REL files). Also the V command,
which must rebuild the same BAM as c1541 -validate."""
import ctypes, pathlib, random, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'tools'))
from d64py import LIB, Disk, VDrive

GEOM = {"d64": (18, 1, 0), "d71": (18, 1, 0), "d81": (40, 3, 1)}       # directory track, first directory sector, super side sector


def c1541(img, *args):
    r = subprocess.run(["c1541", "-attach", str(img), *args], capture_output=True, text=True, encoding="latin1")
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


class RelBase(unittest.TestCase):
    FMT = "d64"

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / ("d." + self.FMT)
        subprocess.run(["c1541", "-format", "rdisk,rd", self.FMT, str(img)], capture_output=True, check=True)
        self.disk = Disk(img.read_bytes())
        self.vd = VDrive()
        self.vd.attach(0, self.disk)
        self.vd.attach(1, None, present=False)
        self.rnd = random.Random(7)

    def tearDown(self): shutil.rmtree(self.tmp)

    # ---- helpers ----
    def cmd(self, text):
        self.vd.listen(8, 0x6F, text); return self.vd.status()

    def pos(self, ch, rec, byte=1):
        return self.cmd(b"P" + bytes([0x60 | ch, rec & 0xFF, rec >> 8, byte]))

    def put(self, ch, rec, data, byte=1):
        self.pos(ch, rec, byte)
        self.vd.listen(8, 0x60 | ch, data)
        return self.vd.status()

    def get(self, ch, rec, byte=1):
        self.pos(ch, rec, byte)
        return self.vd.talk(8, 0x60 | ch)

    def sector(self, t, s):
        off = LIB.d64_sector_offset(self.disk.d, t, s)
        assert off >= 0, (t, s)
        return self.disk.dump()[off: off + 256]

    def entry(self, name):
        dt, ds, _ = GEOM[self.FMT]
        t, s = dt, ds
        while t:
            b = self.sector(t, s)
            for i in range(8):
                p = b[i * 32: i * 32 + 32]
                if p[2] and p[5:21].rstrip(b"\xa0") == name: return p
            t, s = b[0], b[1]
        return None

    def structure(self, name):
        """Walk a REL file the way a 1541/1581 would and check every invariant; returns (reclen, data blocks, record bytes)."""
        e = self.entry(name)
        self.assertIsNotNone(e)
        self.assertEqual(e[2], 0x84)
        reclen, sst, sss = e[23], e[21], e[22]
        d81 = GEOM[self.FMT][2]
        groups = []
        if d81:
            sup = self.sector(sst, sss)
            self.assertEqual(sup[2], 0xFE)
            sst, sss = sup[0], sup[1]
            groups = [(sup[3 + 2 * g], sup[4 + 2 * g]) for g in range(126) if sup[3 + 2 * g]]
            self.assertEqual(groups[0], (sst, sss))
        ptrs, ss_list, t, s = [], [], sst, sss
        while t:
            b = self.sector(t, s)
            ss_list.append((t, s, b))
            k = len(ss_list) - 1
            self.assertEqual(b[2], k % 6); self.assertEqual(b[3], reclen)
            n = 120 if b[0] else (b[1] - 15) // 2
            if not b[0]: self.assertEqual(b[1], 15 + 2 * n)
            ptrs += [(b[16 + 2 * i], b[17 + 2 * i]) for i in range(n)]
            t, s = b[0], b[1]
        for k, (t, s, b) in enumerate(ss_list):                # every side sector lists all of its group
            g = ss_list[k - k % 6: k - k % 6 + 6]
            want = b"".join(bytes([gt, gs]) for gt, gs, _ in g).ljust(12, b"\0")
            self.assertEqual(b[4:16], want, k)
        if d81: self.assertEqual(groups, [(ss_list[k][0], ss_list[k][1]) for k in range(0, len(ss_list), 6)])
        chain, data, t, s = [], b"", e[3], e[4]                 # the data chain must be the side-sector pointers, in order
        while t:
            b = self.sector(t, s); chain.append((t, s))
            data += b[2:] if b[0] else b[2: b[1] + 1]
            t, s = b[0], b[1]
        self.assertEqual(chain, ptrs)
        self.assertEqual(e[30] | e[31] << 8, len(chain) + len(ss_list) + d81)
        self.assertEqual(len(data) % reclen if len(data) >= reclen else 0, len(data) % reclen)
        return reclen, len(chain), data

    def valid(self):
        p = self.tmp / ("out." + self.FMT); p.write_bytes(self.disk.dump())
        before = p.read_bytes(); c1541(p, "-validate")
        self.assertEqual(p.read_bytes(), before, "c1541 -validate changed the BAM")


class RelTests(RelBase):
    def test_create_write_read(self):
        self.vd.open(8, 2, b"PHONE,L," + bytes([30]))
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        reclen, blocks, data = self.structure(b"PHONE")
        self.assertEqual((reclen, blocks), (30, 1))
        self.assertEqual(len(data), 254 // 30 * 30)             # the first block holds 8 empty records
        self.assertEqual(data[:30], b"\xff" + b"\0" * 29)
        self.assertEqual(self.put(2, 1, b"ALICE\r"), "00, OK,00,00\r")
        self.put(2, 3, b"CAROL\r")
        self.assertEqual(self.get(2, 1), (b"ALICE\r", 0x40))
        self.assertEqual(self.get(2, 2), (b"\xff", 0x40))       # an empty record reads as CHR$(255)
        self.assertEqual(self.get(2, 3), (b"CAROL\r", 0x40))
        self.assertEqual(self.get(2, 1, 3), (b"ICE\r", 0x40))   # from byte 3 of the record
        self.vd.close(8, 2)
        self.valid()

    def test_records_follow_each_other_without_p(self):
        self.vd.open(8, 2, b"SEQREC,L," + bytes([10]))
        self.pos(2, 1)
        for i in range(5): self.vd.listen(8, 0x62, b"R%d" % i)    # one PRINT# per record
        self.pos(2, 1)
        got = [self.vd.talk(8, 0x62)[0] for _ in range(5)]
        self.assertEqual(got, [b"R%d" % i for i in range(5)])

    def test_record_is_zero_filled_after_the_data(self):
        self.vd.open(8, 2, b"Z,L," + bytes([20]))
        self.put(2, 1, b"ABCDEFGHIJKLMNOPQRST")
        self.put(2, 1, b"XY")
        _, _, data = self.structure(b"Z")
        self.assertEqual(data[:20], b"XY" + b"\0" * 18)
        self.put(2, 1, b"123", byte=5)                          # writing from byte 5 keeps 1-4, clears the rest
        _, _, data = self.structure(b"Z")
        self.assertEqual(data[:20], b"XY\0\0" + b"123" + b"\0" * 13)

    def test_records_spanning_blocks(self):
        self.vd.open(8, 2, b"SPAN,L," + bytes([100]))
        recs = {i: bytes(self.rnd.randrange(1, 256) for _ in range(100)) for i in range(1, 31)}
        for i, r in recs.items(): self.assertEqual(self.put(2, i, r)[:2], "00")
        for i, r in recs.items(): self.assertEqual(self.get(2, i)[0], r, i)
        reclen, blocks, data = self.structure(b"SPAN")
        self.assertEqual(blocks, (30 * 100 + 253) // 254)
        self.assertEqual(data[:3000], b"".join(recs.values()))
        self.vd.close(8, 2)
        self.vd.open(8, 3, b"SPAN")                              # reopened without ,L: same records
        for i, r in recs.items(): self.assertEqual(self.get(3, i)[0], r, i)
        self.valid()

    def test_position_past_the_end_then_write_grows_the_file(self):
        self.vd.open(8, 2, b"GROW,L," + bytes([50]))
        self.assertEqual(self.pos(2, 200), "50,RECORD NOT PRESENT,00,00\r")
        self.assertEqual(self.vd.talk(8, 0x62)[1] & 0x02, 0x02)  # nothing to read there
        self.assertEqual(self.vd.status(), "50,RECORD NOT PRESENT,00,00\r")
        self.put(2, 200, b"LAST")
        self.assertEqual(self.get(2, 200)[0], b"LAST")
        self.assertEqual(self.get(2, 150)[0], b"\xff")          # the records in between are empty
        reclen, blocks, data = self.structure(b"GROW")
        self.assertEqual(blocks, (200 * 50 + 253) // 254)
        self.assertEqual(len(data), blocks * 254 // 50 * 50)    # the last block is filled with whole empty records
        empty = b"\xff" + b"\0" * 49
        want = empty * 199 + b"LAST" + b"\0" * 46 + empty * (len(data) // 50 - 200)
        self.assertEqual(data, want)                            # every other record is empty, block boundaries included
        self.assertEqual(self.pos(2, len(data) // 50), "00, OK,00,00\r")
        self.assertEqual(self.pos(2, len(data) // 50 + 1), "50,RECORD NOT PRESENT,00,00\r")
        self.valid()

    def test_overflow_in_record(self):
        self.vd.open(8, 2, b"OVF,L," + bytes([5]))
        self.assertEqual(self.put(2, 1, b"0123456789"), "51,OVERFLOW IN RECORD,00,00\r")
        self.assertEqual(self.get(2, 1)[0], b"01234")
        self.assertEqual(self.get(2, 2)[0], b"\xff")            # the rest did not spill into record 2
        self.assertEqual(self.pos(2, 1, 6), "51,OVERFLOW IN RECORD,00,00\r")

    def test_record_length_bytes_that_look_like_syntax(self):
        for n, ln in ((b"COMMA", 44), (b"RLEN", 82), (b"WLEN", 87), (b"CR", 13)):
            self.vd.open(8, 2, n + b",L," + bytes([ln]))
            self.assertEqual(self.vd.status(), "00, OK,00,00\r", n)
            self.assertEqual(self.structure(n)[0], ln)
            self.vd.close(8, 2)

    def test_record_number_bytes_that_look_like_cr(self):
        self.vd.open(8, 2, b"NUM,L," + bytes([4]))
        self.vd.listen(8, 0x6F, b"P" + bytes([0x62, 13, 0, 1]) + b"\r")   # PRINT#15,"P"CHR$(98)CHR$(13)CHR$(0)CHR$(1)
        self.vd.listen(8, 0x62, b"R13")
        self.vd.listen(8, 0x6F, b"P" + bytes([0x62, 13]))                # short form: byte 1 of record 13
        self.assertEqual(self.vd.talk(8, 0x62)[0], b"R13")
        self.vd.listen(8, 0x6F, b"P" + bytes([0x62, 0, 0]))              # record 0 = record 1
        self.assertEqual(self.vd.talk(8, 0x62)[0], b"\xff")

    def test_open_errors(self):
        self.vd.open(8, 2, b"NOPE")
        self.assertEqual(self.vd.status(), "62,FILE NOT FOUND,00,00\r")
        self.vd.open(8, 2, b"NOPE,L")                            # no record length: cannot be created
        self.assertEqual(self.vd.status(), "62,FILE NOT FOUND,00,00\r")
        self.vd.open(8, 2, b"BAD,L," + bytes([255]))
        self.assertTrue(self.vd.status().startswith("33,"))
        self.vd.open(8, 2, b"R,L," + bytes([10])); self.vd.close(8, 2)
        self.vd.open(8, 2, b"R,L," + bytes([11]))                # another record length
        self.assertEqual(self.vd.status(), "50,RECORD NOT PRESENT,00,00\r")
        self.vd.open(8, 2, b"R,L," + bytes([10]))
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.vd.open(8, 3, b"S,S,W"); self.vd.listen(8, 0x63, b"x"); self.vd.close(8, 3)
        self.vd.open(8, 3, b"S,L," + bytes([10]))
        self.assertEqual(self.vd.status(), "64,FILE TYPE MISMATCH,00,00\r")
        self.assertEqual(self.cmd(b"P" + bytes([0x64, 1, 0, 1])), "70,NO CHANNEL,00,00\r")
        self.assertEqual(self.cmd(b"P"), "30,SYNTAX ERROR,00,00\r")

    def test_listing_shows_rel(self):
        self.vd.open(8, 2, b"LIST,L," + bytes([60])); self.vd.close(8, 2)
        d = self.vd.load(8, b"$")
        self.assertRegex(d, rb'"LIST" +REL ')

    def test_scratch_frees_data_and_side_sectors(self):
        free = self.disk.free()
        self.vd.open(8, 2, b"BIGREL,L," + bytes([254]))
        self.put(2, 300, b"X")                                   # 300 blocks: 3 side sectors
        self.vd.close(8, 2)
        self.assertEqual(self.structure(b"BIGREL")[1], 300)
        self.assertEqual(self.disk.free(), free - 300 - 3 - GEOM[self.FMT][2])
        self.assertEqual(self.cmd(b"S:BIGREL"), "01,FILES SCRATCHED,01,00\r")
        self.assertEqual(self.disk.free(), free)
        self.valid()

    def test_copy_rel(self):
        self.vd.open(8, 2, b"SRC,L," + bytes([77]))
        recs = {i: bytes(self.rnd.randrange(1, 256) for _ in range(77)) for i in range(1, 400, 7)}
        for i, r in recs.items(): self.put(2, i, r)
        self.vd.close(8, 2)
        self.assertEqual(self.cmd(b"C:DST=SRC"), "00, OK,00,00\r")
        a, b = self.structure(b"SRC"), self.structure(b"DST")
        self.assertEqual(a, b)
        self.vd.open(8, 3, b"DST")
        for i, r in recs.items(): self.assertEqual(self.get(3, i)[0], r, i)
        self.assertEqual(self.cmd(b"C:BOTH=SRC,DST"), "64,FILE TYPE MISMATCH,00,00\r")
        self.valid()

    def test_disk_full_while_growing(self):
        free = self.disk.free()
        self.vd.open(8, 2, b"FULL,L," + bytes([254]))
        st = self.put(2, 65000, b"X")
        self.assertTrue(st.startswith("72,DISK FULL") or st.startswith("52,FILE TOO LARGE"), st)
        self.vd.close(8, 2)
        reclen, blocks, data = self.structure(b"FULL")           # still a well-formed file, as far as it got
        self.valid()
        self.cmd(b"S:FULL")
        self.assertEqual(self.disk.free(), free)

    def test_two_rel_files_interleaved(self):
        self.vd.open(8, 2, b"A,L," + bytes([16])); self.vd.open(8, 3, b"B,L," + bytes([16]))
        for i in range(1, 60):
            self.put(2, i, b"A%d" % i); self.put(3, i, b"B%d" % i)
        for i in range(1, 60):
            self.assertEqual(self.get(2, i)[0], b"A%d" % i); self.assertEqual(self.get(3, i)[0], b"B%d" % i)
        self.vd.close(8, 2); self.vd.close(8, 3)
        self.structure(b"A"); self.structure(b"B")
        self.valid()


class ValidateTests(RelBase):
    """V rebuilds the BAM; it must agree with c1541 -validate on the same disk."""
    BAM = {"d64": ((18, 0),), "d71": ((18, 0), (53, 0)), "d81": ((40, 1), (40, 2))}

    def bam(self, img=None):
        img = img if img is not None else self.disk.dump()
        out = b""
        for t, s in self.BAM[self.FMT]:
            off = LIB.d64_sector_offset(self.disk.d, t, s)
            out += img[off: off + 256]
        return out

    def populate(self):
        self.vd.save(8, b"PROG", bytes(self.rnd.randrange(256) for _ in range(5000)))
        self.vd.open(8, 2, b"SEQ,S,W"); self.vd.listen(8, 0x62, b"x" * 3000); self.vd.close(8, 2)
        self.vd.open(8, 2, b"REL,L," + bytes([200])); self.put(2, 150, b"END"); self.vd.close(8, 2)
        self.vd.open(8, 2, b"GONE,S,W"); self.vd.listen(8, 0x62, b"y" * 600); self.vd.close(8, 2)
        self.cmd(b"S:GONE")
        w = ctypes.create_string_buffer(LIB.d64_sizeof_wfile())                   # written but never closed: a splat
        self.assertEqual(LIB.d64_create(self.disk.d, b"SPLAT", 1, 0, w), 0)
        self.assertEqual(LIB.d64_write(self.disk.d, w, b"z" * 700, ctypes.c_size_t(700)), 700)
        self.assertEqual({e["name"]: e["type"] for e in self.disk.entries()}[b"SPLAT"], 0x01)

    def test_validate_matches_c1541(self):
        self.populate()
        ref = self.tmp / ("ref." + self.FMT); ref.write_bytes(self.disk.dump())
        c1541(ref, "-validate")
        img = bytearray(self.disk.dump())                                          # scramble the counts and bitmaps
        ranges = {"d64": {(18, 0): [range(4, 0x90)]},
                  "d71": {(18, 0): [range(4, 0x90), range(0xDD, 0x100)], (53, 0): [range(0, 105)]},
                  "d81": {(40, 1): [range(16, 256)], (40, 2): [range(16, 256)]}}[self.FMT]
        for (t, s), rs in ranges.items():
            off = LIB.d64_sector_offset(self.disk.d, t, s)
            for rg in rs:
                for i in rg: img[off + i] ^= self.rnd.randrange(256)
        self.disk = Disk(bytes(img)); self.vd.attach(0, self.disk)
        self.assertEqual(self.cmd(b"V"), "00, OK,00,00\r")
        self.assertEqual(self.bam(), self.bam(ref.read_bytes()))
        names = sorted(e["name"] for e in self.disk.entries())
        self.assertEqual(names, [b"PROG", b"REL", b"SEQ"])                         # the splat entry is gone
        self.assertEqual(self.vd.load(8, b"PROG")[:2], self.disk.read_file(b"PROG")[:2])
        self.valid()

    def test_validate_frees_blocks_no_file_uses(self):
        free = self.disk.free()
        self.assertEqual(self.cmd(b"B-A:0,1,0"), "00, OK,00,00\r")
        self.assertEqual(self.disk.free(), free - 1)
        self.assertEqual(self.cmd(b"V0"), "00, OK,00,00\r")
        self.assertEqual(self.disk.free(), free)


class ValidateD71Tests(ValidateTests):
    FMT = "d71"


class ValidateD81Tests(ValidateTests):
    FMT = "d81"


class RelD71Tests(RelBase):
    FMT = "d71"

    def test_file_too_large_at_720_blocks(self):
        self.vd.open(8, 2, b"HUGE,L," + bytes([254]))
        self.assertEqual(self.put(2, 720, b"END"), "00, OK,00,00\r")
        self.assertEqual(self.structure(b"HUGE")[1], 720)
        self.assertEqual(self.put(2, 721, b"MORE"), "52,FILE TOO LARGE,00,00\r")
        self.assertEqual(self.get(2, 720)[0], b"END")
        self.vd.close(8, 2)
        self.valid()


class RelD81Tests(RelTests):
    FMT = "d81"

    def test_more_than_one_group(self):
        self.vd.open(8, 2, b"GROUPS,L," + bytes([254]))
        self.assertEqual(self.put(2, 900, b"END"), "00, OK,00,00\r")           # 900 blocks: 8 side sectors in 2 groups
        self.put(2, 1, b"FIRST"); self.put(2, 721, b"G2")
        self.assertEqual(self.get(2, 900)[0], b"END"); self.assertEqual(self.get(2, 721)[0], b"G2")
        self.assertEqual(self.get(2, 1)[0], b"FIRST")
        self.vd.close(8, 2)
        self.assertEqual(self.structure(b"GROUPS")[1], 900)
        self.vd.open(8, 3, b"GROUPS")
        self.assertEqual(self.get(3, 721)[0], b"G2")
        self.valid()


if __name__ == "__main__":
    unittest.main()
