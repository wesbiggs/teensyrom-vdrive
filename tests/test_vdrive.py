import pathlib, random, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'tools'))
from d64py import LIB, Disk, VDrive, VD_LISTEN, VD_TALK, VD_CIOUT, VD_ACPTR


def c1541(img, *args):
    r = subprocess.run(["c1541", "-attach", str(img), *args], capture_output=True, text=True, encoding="latin1")
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def bam(image):
    return image[357 * 256 + 4: 357 * 256 + 0x90]


class VDriveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        img = self.tmp / "d.d64"
        subprocess.run(["c1541", "-format", "vdisk,vd", "d64", str(img)], capture_output=True, check=True)
        self.rnd = random.Random(3)
        self.seed = {}
        for name, n in (("seed", 700), ("other", 40)):
            data = bytes(self.rnd.randrange(256) for _ in range(n))
            (self.tmp / name).write_bytes(data)
            c1541(img, "-write", str(self.tmp / name), name)
            self.seed[name.upper().encode()] = data
        self.disk = Disk(img.read_bytes())
        self.vd = VDrive()
        self.vd.attach(0, self.disk)
        self.vd.attach(1, None, present=False)

    def tearDown(self): shutil.rmtree(self.tmp)

    def data(self, n): return bytes(self.rnd.randrange(256) for _ in range(n))

    def img(self):
        p = self.tmp / "out.d64"; p.write_bytes(self.disk.dump()); return p

    def valid(self):
        p = self.img(); before = bam(p.read_bytes()); c1541(p, "-validate")
        self.assertEqual(bam(p.read_bytes()), before)

    # ---- reading ----
    def test_load_file_and_eoi(self):
        self.assertEqual(self.vd.load(8, b"SEED"), self.seed[b"SEED"])
        data, last = self.vd.talk(8, 0x60)          # channel closed: nothing, timeout
        self.assertEqual(data, b"")

    def test_directory_load(self):
        d = self.vd.load(8, b"$")
        self.assertEqual(d[:2], b"\x01\x04")
        self.assertIn(b'"SEED"', d); self.assertIn(b'"OTHER"', d)
        d = self.vd.load(8, b"$:S*")
        self.assertIn(b'"SEED"', d); self.assertNotIn(b'"OTHER"', d)
        d = self.vd.load(8, b"$0:O*")
        self.assertIn(b'"OTHER"', d); self.assertNotIn(b'"SEED"', d)

    def test_slash_in_file_name(self):
        data = self.data(300)
        self.vd.save(8, b"A/B", data)
        self.assertEqual(self.vd.load(8, b"A/B"), data)
        self.assertIn(b'"A/B"', self.vd.load(8, b"$"))
        self.assertIn(b'"A/B"', self.vd.load(8, b"$A/*"))
        self.valid()

    def test_file_not_found_status(self):
        self.vd.open(8, 2, b"NOPE")
        self.assertEqual(self.vd.status(), "62,FILE NOT FOUND,00,00\r")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")          # cleared by reading

    def test_two_channels_interleaved(self):
        self.vd.open(8, 2, b"SEED"); self.vd.open(8, 3, b"OTHER")
        a = self.vd.talk(8, 0x62, 100)[0] + self.vd.talk(8, 0x62)[0]
        b = self.vd.talk(8, 0x63)[0]
        self.assertEqual(a, self.seed[b"SEED"]); self.assertEqual(b, self.seed[b"OTHER"])

    # ---- saving ----
    def test_save_and_c1541_reads_it(self):
        prog = b"\x01\x08" + self.data(1500)
        self.assertEqual(self.vd.save(8, b"PROG", prog), 0)
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.vd.load(8, b"PROG"), prog)
        out = self.tmp / "o.bin"
        c1541(self.img(), "-read", "prog", str(out))
        self.assertEqual(out.read_bytes(), prog)
        self.valid()
        self.assertIn(b'"PROG"', self.vd.load(8, b"$"))

    def test_save_existing_is_error_and_replace_works(self):
        self.vd.save(8, b"PROG", b"\x01\x08aaa")
        self.vd.status()
        self.vd.save(8, b"PROG", b"\x01\x08bbb")
        self.assertEqual(self.vd.status(), "63,FILE EXISTS,00,00\r")
        self.assertEqual(self.vd.load(8, b"PROG"), b"\x01\x08aaa")      # untouched
        self.vd.save(8, b"@0:PROG", b"\x01\x08ccccc")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.vd.load(8, b"PROG"), b"\x01\x08ccccc")
        self.valid()

    def test_disk_full_status(self):
        free = self.disk.free()
        self.vd.save(8, b"BIG", self.data(free * 254 + 1000))
        self.assertEqual(self.vd.status(), "72,DISK FULL,00,00\r")
        self.assertEqual(self.disk.free(), free)
        self.assertIsNone(self.disk.read_file(b"BIG"))
        self.valid()

    def test_sequential_write_then_read(self):
        self.vd.open(8, 2, b"DATA,S,W")
        self.vd.listen(8, 0x62, b"LINE1\rLINE2\r")
        self.vd.listen(8, 0x62, b"LINE3\r")
        self.vd.close(8, 2)
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.vd.open(8, 3, b"DATA,S,R")
        got, last = self.vd.talk(8, 0x63)
        self.assertEqual(got, b"LINE1\rLINE2\rLINE3\r"); self.assertEqual(last & 0x40, 0x40)
        t = {e["name"]: e["type"] for e in self.disk.entries()}
        self.assertEqual(t[b"DATA"], 0x81)                              # closed SEQ
        self.valid()

    def test_append_to_a_seq_file(self):
        first = self.data(300)
        self.vd.open(8, 2, b"LOG,S,W"); self.vd.listen(8, 0x62, first); self.vd.close(8, 2)
        more = self.data(1000)                                          # crosses into new sectors
        self.vd.open(8, 2, b"LOG,S,A")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        t = {e["name"]: e["type"] for e in self.disk.entries()}
        self.assertEqual(t[b"LOG"], 0x01)                               # unclosed while appending, like a real drive
        self.vd.listen(8, 0x62, more[:500]); self.vd.listen(8, 0x62, more[500:])
        self.vd.close(8, 2)
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.vd.open(8, 3, b"LOG")
        self.assertEqual(self.vd.talk(8, 0x63)[0], first + more)
        e = {e["name"]: e for e in self.disk.entries()}[b"LOG"]
        self.assertEqual((e["type"], e["blocks"]), (0x81, (1300 + 253) // 254))
        out = self.tmp / "log.bin"
        c1541(self.img(), "-read", "log,s", str(out))
        self.assertEqual(out.read_bytes(), first + more)
        self.valid()

    def test_append_at_a_sector_boundary_and_with_nothing_written(self):
        self.vd.open(8, 2, b"EXACT,S,W"); self.vd.listen(8, 0x62, b"x" * 254); self.vd.close(8, 2)
        self.vd.open(8, 2, b"0:EXACT,A"); self.vd.close(8, 2)           # append nothing: unchanged, closed again
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.vd.open(8, 2, b"EXACT,A"); self.vd.listen(8, 0x62, b"y"); self.vd.close(8, 2)
        self.vd.open(8, 3, b"EXACT")
        self.assertEqual(self.vd.talk(8, 0x63)[0], b"x" * 254 + b"y")
        self.valid()

    def test_append_errors(self):
        self.vd.open(8, 2, b"NOPE,A")
        self.assertEqual(self.vd.status(), "62,FILE NOT FOUND,00,00\r")
        self.vd.open(8, 2, b"REL,L," + bytes([10])); self.vd.close(8, 2)
        self.vd.open(8, 2, b"REL,A")
        self.assertEqual(self.vd.status(), "64,FILE TYPE MISMATCH,00,00\r")

    def test_append_disk_full_puts_the_file_back(self):
        self.vd.save(8, b"FILL", self.data((self.disk.free() - 3) * 254))      # three sectors left
        before = self.disk.dump()
        self.vd.open(8, 2, b"SEED,A")
        self.vd.listen(8, 0x62, self.data(254 * 5))
        self.assertEqual(self.vd.status(), "72,DISK FULL,00,00\r")
        self.vd.close(8, 2)
        track18 = lambda img: img[357 * 256: 376 * 256]
        self.assertEqual(track18(self.disk.dump()), track18(before))   # BAM and directory as before the append
        self.assertEqual(self.vd.load(8, b"SEED"), self.seed[b"SEED"])
        self.valid()

    def test_append_on_a_full_disk_puts_the_file_back(self):
        self.vd.save(8, b"FILL", self.data(self.disk.free() * 254))           # nothing left at all
        self.assertEqual(self.disk.free(), 0)
        before = self.disk.dump()[357 * 256: 376 * 256]
        self.vd.open(8, 2, b"SEED,A"); self.vd.listen(8, 0x62, self.data(300)); self.vd.close(8, 2)
        self.assertEqual(self.vd.status(), "72,DISK FULL,00,00\r")
        self.assertEqual(self.disk.dump()[357 * 256: 376 * 256], before)
        self.assertEqual(self.vd.load(8, b"SEED"), self.seed[b"SEED"])
        self.valid()

    def test_unclosed_file_is_splat(self):
        self.vd.open(8, 2, b"HALF,S,W")
        self.vd.listen(8, 0x62, b"abc")
        t = {e["name"]: e["type"] for e in self.disk.entries()}
        self.assertEqual(t[b"HALF"], 0x01)                              # not closed yet
        self.vd.close(8, 2)
        t = {e["name"]: e["type"] for e in self.disk.entries()}
        self.assertEqual(t[b"HALF"], 0x81)

    # ---- command channel ----
    def test_scratch_command(self):
        self.vd.listen(8, 0x6F, b"S:OTHER\r")
        self.assertEqual(self.vd.status(), "01,FILES SCRATCHED,01,00\r")
        self.assertIsNone(self.disk.read_file(b"OTHER"))
        self.vd.open(8, 15, b"S0:SEED,NOTTHERE")                       # command via OPEN 15,8,15,"..."
        self.assertEqual(self.vd.status(), "01,FILES SCRATCHED,01,00\r")
        self.valid()

    def test_scratch_wildcard(self):
        self.vd.listen(8, 0x6F, b"S:*")
        self.assertEqual(self.vd.status(), "01,FILES SCRATCHED,02,00\r")
        self.assertEqual(self.disk.entries(), [])

    def test_new_and_rename(self):
        self.vd.listen(8, 0x6F, b"R:RENAMED=SEED")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.vd.load(8, b"RENAMED"), self.seed[b"SEED"])
        self.vd.listen(8, 0x6F, b"R:OTHER=RENAMED")
        self.assertEqual(self.vd.status(), "63,FILE EXISTS,00,00\r")
        self.vd.listen(8, 0x6F, b"N:FRESH,ZZ")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.disk.header(), (b"FRESH", b"ZZ", b"2A"))
        self.assertEqual(self.disk.entries(), [])
        self.valid()

    def test_copy_command(self):
        self.vd.listen(8, 0x6F, b"C:DUP=SEED")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.vd.load(8, b"DUP"), self.seed[b"SEED"])
        self.vd.listen(8, 0x6F, b"C0:BOTH=0:SEED,OTH*\r")                 # join two files; drive prefixes, a pattern, PRINT#'s CR
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.vd.load(8, b"BOTH"), self.seed[b"SEED"] + self.seed[b"OTHER"])
        t = {e["name"]: e for e in self.disk.entries()}
        self.assertEqual(t[b"BOTH"]["type"], t[b"SEED"]["type"])
        self.assertEqual(t[b"BOTH"]["blocks"], (740 + 253) // 254)
        out = self.tmp / "both.bin"
        c1541(self.img(), "-read", "both", str(out))
        self.assertEqual(out.read_bytes(), self.seed[b"SEED"] + self.seed[b"OTHER"])
        self.valid()

    def test_copy_of_a_seq_file_keeps_its_type(self):
        self.vd.open(8, 2, b"DATA,S,W"); self.vd.listen(8, 0x62, b"HELLO\r"); self.vd.close(8, 2)
        self.vd.listen(8, 0x6F, b"C:DATA2=DATA")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        t = {e["name"]: e["type"] for e in self.disk.entries()}
        self.assertEqual(t[b"DATA2"], 0x81)
        self.vd.open(8, 3, b"DATA2,S,R")
        self.assertEqual(self.vd.talk(8, 0x63)[0], b"HELLO\r")

    def test_copy_errors_leave_nothing_behind(self):
        free = self.disk.free()
        names = lambda: sorted(e["name"] for e in self.disk.entries())
        before = names()
        for cmd, want in ((b"C:NEW=NOPE", "62,FILE NOT FOUND"), (b"C:NEW=SEED,NOPE", "62,FILE NOT FOUND"),
                          (b"C:OTHER=SEED", "63,FILE EXISTS"), (b"C:NEW", "34,SYNTAX ERROR"), (b"C:=SEED", "34,SYNTAX ERROR"),
                          (b"C:NEW=SEED,", "34,SYNTAX ERROR"), (b"C:N*W=SEED", "33,SYNTAX ERROR"), (b"C1=0", "31,SYNTAX ERROR")):
            self.vd.listen(8, 0x6F, cmd)
            self.assertTrue(self.vd.status().startswith(want + ","), cmd)
        self.assertEqual(names(), before); self.assertEqual(self.disk.free(), free)
        self.valid()

    def test_copy_disk_full_removes_the_new_file(self):
        big = self.data((self.disk.free() - 2) * 254)                    # the copy needs more than is left
        self.assertEqual(self.vd.save(8, b"BIG", big), 0)
        free = self.disk.free()
        self.vd.listen(8, 0x6F, b"C:BIG2=BIG")
        self.assertEqual(self.vd.status(), "72,DISK FULL,00,00\r")
        self.assertEqual(self.disk.free(), free)
        self.assertIsNone(self.disk.read_file(b"BIG2"))
        self.valid()

    def test_copy_source_pattern_does_not_pick_the_new_file(self):
        # S:SEED frees directory slot 0, so the new file OX takes it, ahead of OTHER: "O*" must still mean OTHER
        self.vd.listen(8, 0x6F, b"S:SEED"); self.vd.status()
        self.vd.listen(8, 0x6F, b"C:OX=O*")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.disk.entries()[0]["name"], b"OX")
        self.assertEqual(self.vd.load(8, b"OX"), self.seed[b"OTHER"])
        self.valid()

    def test_lock_toggle(self):
        self.assertEqual(self.cmd(b"L:SEED"), "00, OK,00,00\r")
        self.assertRegex(self.vd.load(8, b"$"), rb'"SEED" +PRG<')
        self.assertEqual(self.cmd(b"S:SEED"), "01,FILES SCRATCHED,00,00\r")      # locked: not scratched
        self.assertEqual(self.vd.load(8, b"SEED"), self.seed[b"SEED"])
        self.assertEqual(self.cmd(b"L0:S*"), "00, OK,00,00\r")                   # toggles back
        self.assertNotRegex(self.vd.load(8, b"$"), rb'"SEED" +PRG<')
        self.assertEqual(self.cmd(b"S:SEED"), "01,FILES SCRATCHED,01,00\r")
        self.assertEqual(self.cmd(b"L:NOPE"), "62,FILE NOT FOUND,00,00\r")
        self.assertEqual(self.cmd(b"L:"), "34,SYNTAX ERROR,00,00\r")
        self.valid()

    def test_write_protect_from_the_c64(self):
        self.assertEqual(self.cmd(b"W-1"), "00, OK,00,00\r")
        self.vd.save(8, b"NEW", b"\x01\x08abc")
        self.assertEqual(self.vd.status(), "26,WRITE PROTECT ON,00,00\r")
        for c in (b"S:SEED", b"R:X=SEED", b"C:X=SEED", b"N:X", b"V", b"L:SEED", b"B-A:0,1,0"):
            self.assertEqual(self.cmd(c), "26,WRITE PROTECT ON,00,00\r", c)
        self.assertEqual(self.vd.load(8, b"SEED"), self.seed[b"SEED"])          # reading still works
        self.assertEqual(self.cmd(b"W-0"), "00, OK,00,00\r")
        self.assertEqual(self.vd.save(8, b"NEW", b"\x01\x08abc"), 0)
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.cmd(b"W-1")
        self.disk = Disk(self.disk.dump()); self.vd.attach(0, self.disk)         # another image (a remount) is not protected
        self.assertEqual(self.cmd(b"S:NEW"), "01,FILES SCRATCHED,01,00\r")

    def test_unknown_command_is_syntax_error(self):
        self.vd.listen(8, 0x6F, b"X:FOO")
        self.assertTrue(self.vd.status().startswith("31,SYNTAX ERROR"))

    def test_ui_uj_reset_answers_73_and_closes_channels(self):
        for cmd in (b"UI", b"UJ", b"U9"):
            self.vd.open(8, 2, b"SEED")
            self.vd.listen(8, 0x6F, cmd)
            self.assertEqual(self.vd.status(), "73,TRVDRIVE V1.0,00,00\r", cmd)
            self.assertEqual(self.vd.talk(8, 0x62)[0], b"", cmd)             # the channel was closed by the reset
            self.assertEqual(self.vd.load(8, b"SEED"), self.seed[b"SEED"])    # the disk is still mounted

    def test_ui_works_with_nothing_mounted_and_bus_speed_variants_are_ignored(self):
        vd = VDrive(); vd.attach(0, None, present=True)
        vd.listen(8, 0x6F, b"UI")
        self.assertEqual(vd.status(), "73,TRVDRIVE V1.0,00,00\r")
        for cmd in (b"UI+", b"UI-"):
            vd.listen(8, 0x6F, cmd)
            self.assertEqual(vd.status(), "00, OK,00,00\r", cmd)
        vd.listen(8, 0x6F, b"S:X")                                          # other commands still need a disk
        self.assertTrue(vd.status().startswith("74,DRIVE NOT READY"))

    # ---- direct access: OPEN n,8,n,"#" + U1/U2/B-P/B-A/B-F ----
    def sector(self, t, s):
        off = LIB.d64_sector_offset(self.disk.d, t, s)
        return self.disk.dump()[off: off + 256]

    def cmd(self, text):
        self.vd.listen(8, 0x6F, text); return self.vd.status()

    def test_block_read_gets_the_whole_sector_with_eoi_at_255(self):
        self.vd.open(8, 5, b"#")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.cmd(b"U1:5 0 18 0"), "00, OK,00,00\r")
        data, last = self.vd.talk(8, 0x65)
        self.assertEqual(data, self.sector(18, 0)); self.assertTrue(last & 0x40)
        self.assertEqual(self.cmd(b"UA:5,0,18,1"), "00, OK,00,00\r")             # UA = U1, separators vary
        self.assertEqual(self.vd.talk(8, 0x65)[0], self.sector(18, 1))
        self.assertEqual(self.cmd(b"B-R:5,0,18,0\r"), "00, OK,00,00\r")            # B-R behaves like U1 here
        self.assertEqual(self.vd.talk(8, 0x65)[0], self.sector(18, 0))

    def test_buffer_pointer_and_partial_read(self):
        self.vd.open(8, 5, b"#"); self.cmd(b"U1:5 0 18 0")
        self.assertEqual(self.cmd(b"B-P:5,144"), "00, OK,00,00\r")
        data, last = self.vd.talk(8, 0x65)                              # disk name at $90, to the end of the sector
        self.assertEqual(data, self.sector(18, 0)[144:]); self.assertTrue(last & 0x40)
        self.cmd(b"B-P:5,144")
        self.assertEqual(self.vd.talk(8, 0x65, 5)[0], self.sector(18, 0)[144:149])

    def test_block_write_roundtrip_and_c1541_sees_it(self):
        payload = self.data(256)
        self.vd.open(8, 5, b"#")
        self.cmd(b"B-P:5,0")
        self.vd.listen(8, 0x65, payload)
        self.assertEqual(self.cmd(b"U2:5 0 1 0"), "00, OK,00,00\r")
        self.assertEqual(self.sector(1, 0), payload)
        self.cmd(b"U1:5 0 1 0")
        self.assertEqual(self.vd.talk(8, 0x65)[0], payload)
        c1541(self.img(), "-block", "1", "0")                                # c1541 reads the same sector

    def test_buffer_ignores_writes_past_the_end(self):
        self.vd.open(8, 5, b"#"); self.cmd(b"B-P:5,250")
        self.vd.listen(8, 0x65, b"ABCDEFGHIJ")
        self.assertEqual(self.cmd(b"U2:5 0 1 1"), "00, OK,00,00\r")
        self.assertEqual(self.sector(1, 1)[250:], b"ABCDEF")

    def test_block_errors(self):
        self.assertEqual(self.cmd(b"U1:5 0 18 0"), "70,NO CHANNEL,00,00\r")          # no buffer open
        self.vd.open(8, 5, b"#")
        self.assertEqual(self.cmd(b"U1:5 0 36 0"), "66,ILLEGAL TRACK OR SECTOR,36,00\r")
        self.assertEqual(self.cmd(b"U1:5 0 18 21"), "66,ILLEGAL TRACK OR SECTOR,18,21\r")
        self.assertEqual(self.cmd(b"U1:5 0 18"), "30,SYNTAX ERROR,00,00\r")
        self.vd.open(8, 2, b"SEED")                                          # a file channel is not a buffer
        self.assertEqual(self.cmd(b"U1:2 0 18 0"), "70,NO CHANNEL,00,00\r")
        self.assertEqual(self.cmd(b"B-P:7,0"), "70,NO CHANNEL,00,00\r")

    def test_buffer_channel_exhausts_the_pool_and_close_releases_it(self):
        for ch in range(2, 9): self.vd.open(8, ch, b"#")           # VD_NSLOT = 6, so the 7th fails
        self.assertEqual(self.vd.status(), "70,NO CHANNEL,00,00\r")
        self.vd.close(8, 2)
        self.vd.open(8, 2, b"#"); self.assertEqual(self.vd.status(), "00, OK,00,00\r")

    def test_b_a_and_b_f_update_the_bam(self):
        free = self.disk.free()
        self.assertEqual(self.cmd(b"B-A:0,1,0"), "00, OK,00,00\r")
        self.assertEqual(self.disk.free(), free - 1)
        self.assertEqual(self.cmd(b"B-A:0,1,0"), "65,NO BLOCK,01,01\r")            # taken: next free is 1/1
        self.assertEqual(self.disk.free(), free - 1)
        self.assertEqual(self.cmd(b"B-F:0,1,0"), "00, OK,00,00\r")
        self.assertEqual(self.disk.free(), free)
        self.assertEqual(self.cmd(b"B-A:0,99,0"), "66,ILLEGAL TRACK OR SECTOR,99,00\r")
        self.valid()

    def test_b_a_on_a_used_system_sector_reports_the_next_free_one(self):
        r = self.cmd(b"B-A:0,18,0")
        self.assertTrue(r.startswith("65,NO BLOCK,"), r)

    def test_unknown_b_command_is_syntax_error(self):
        self.assertTrue(self.cmd(b"B-X:1").startswith("31,SYNTAX ERROR"))

    def test_unread_status_is_dropped_by_the_next_operation(self):
        # the Q4K pattern: scratch through channel 15, read only the first 2 bytes, then save; the next status read must be the
        # save's own "00", not the unread tail of the scratch message
        self.vd.open(8, 15, b"S0:Q4K.SAV")
        self.assertEqual(self.vd.talk(8, 0x6F, 2)[0], b"01")
        self.vd.save(8, b"Q4K.SAV", b"\x01\x08abc")
        self.assertEqual(self.vd.talk(8, 0x6F, 2)[0], b"00")
        self.assertEqual(self.vd.status(), ", OK,00,00\r")                 # the rest of the SAVE's own message

    def test_status_read_in_pieces_without_operations_continues(self):
        self.vd.listen(8, 0x6F, b"S:NOSUCH")
        got = b"".join(self.vd.talk(8, 0x6F, 1)[0] for _ in range(6))      # one byte per TALK session, like GET#15
        self.assertEqual(got, b"01,FIL")
        rest = self.vd.status()
        self.assertEqual(rest, "ES SCRATCHED,00,00\r")

    def test_status_read_twice_in_a_row(self):
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")
        self.assertEqual(self.vd.status(), "00, OK,00,00\r")

    def test_close_15_closes_everything(self):
        self.vd.open(8, 2, b"X,S,W")
        self.vd.listen(8, 0x62, b"hi")
        self.vd.close(8, 15)
        t = {e["name"]: e["type"] for e in self.disk.entries()}
        self.assertEqual(t[b"X"], 0x81)

    # ---- bus ownership ----
    def test_other_devices_not_mine(self):
        nm, _, _ = self.vd.cmd(VD_LISTEN, 4)
        self.assertEqual(nm, 1)
        nm, _, _ = self.vd.cmd(VD_CIOUT, 65)                            # real device mid-transaction
        self.assertEqual(nm, 1)

    def test_absent_device_9_sets_nodev(self):
        nm, _, st = self.vd.cmd(VD_LISTEN, 9)
        self.assertEqual((nm, st), (0, 0x80))
        self.assertEqual(self.vd.cmd(VD_CIOUT, 1)[0], 0)                # swallowed, not forwarded to real bus

    def test_no_disk_mounted_gives_not_ready(self):
        vd = VDrive(); vd.attach(0, None, present=True)
        vd.open(8, 2, b"X")
        self.assertTrue(vd.status().startswith("74,DRIVE NOT READY"))


if __name__ == "__main__":
    unittest.main()
