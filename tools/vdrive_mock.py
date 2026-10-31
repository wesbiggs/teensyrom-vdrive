#!/usr/bin/env python3
"""VICE-side mock of the TR+ virtual-drive mailbox (RAM at MBX, see Makefile).

The drive itself is the real C code (d64/vdrive.c via ctypes); only the mailbox
transport is faked here.

Drives x64sc through its binary monitor: a store-checkpoint on MBX+0 (CMD)
stops the CPU, this script plays the Teensy (state machine for IEC devices
8/9 backed by a tiny in-memory 'disk'), writes DATA/STAT/ST and resumes.
usage: vdrive_mock.py KERNAL.bin [basic|scan]
  basic  the whole feature test, no physical drives (the SD drive ends up as device 8 after the start-up scan)
  scan   physical 1541s at 8 and 10: the SD and USB drives must end up at 9 and 11
"""
import socket, struct, subprocess, sys, time, threading, queue

MBX = 0xC800
WIN = 0xCA00                      # IO2 window (RAM here)
CMD, ARG, DATA, STAT, ST, LEN, DEV0, DEV1 = (MBX + i for i in range(8))
(C_LISTEN, C_TALK, C_SECOND, C_TKSA, C_CIOUT, C_ACPTR, C_UNLSN, C_UNTLK) = range(1, 9)
NOT_MINE = 0xA5                   # STAT answer tokens (see d64/vdrive_io.h)
ANSWERED = 0x5A

# ---- drive backed by libd64 ------------------------------------------------
import ctypes, os, random, shutil, tempfile, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from d64py import LIB, Disk, VDrive, VIO, R_DEV0, R_DEV1, WIN_SIZE
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests"))
from test_vio import HostStruct               # ctypes layout of vd_host_t

OK = b"00, OK,00,00\r"

def make_image(workdir):
    """Build a test disk with c1541: TEST (BASIC), BIG (multi-sector, load $2000)."""
    rnd = random.Random(1)
    big = b"\x00\x20" + bytes(rnd.randrange(256) for _ in range(6000))
    prg = b"\x01\x08" + bytes.fromhex("0e08 0a00 99 22 48 49 22 00 00 00".replace(" ", ""))  # 10 PRINT"HI"
    (workdir / "test.prg").write_bytes(prg)
    (workdir / "big.prg").write_bytes(big)
    img = workdir / "t.d64"
    def c(*a): subprocess.run(["c1541", *a], check=True, capture_output=True)
    c("-format", "mockdisk,md", "d64", str(img))
    c("-attach", str(img), "-write", str(workdir / "test.prg"), "test")
    c("-attach", str(img), "-write", str(workdir / "big.prg"), "big")
    basic = pathlib.Path(__file__).resolve().parent.parent / "tests" / "basic"
    xx = sum(big[2 + i] for i in range(0, 6000, 8))                 # stride-8 checksum soak.bas compares against
    for src in sorted(basic.glob("*.bas")):
        out = workdir / (src.stem + ".prg")
        text = workdir / src.name
        text.write_text(src.read_text().replace("@XX@", str(xx)))
        subprocess.run(["petcat", "-w2", "-o", str(out), "--", str(text)], check=True, capture_output=True)
        c("-attach", str(img), "-write", str(out), src.stem)
    from make_util import build as build_util
    c("-attach", str(img), "-write", str(build_util(workdir / "util")), "vdcopy")
    return img.read_bytes(), big


# ---- VICE binary monitor ---------------------------------------------------
class Mon:
    def __init__(self, port=6502):
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port)); break
            except OSError: time.sleep(0.1)
        self.rid = 0
        self.ev = queue.Queue()
        self.resp = {}
        self.cv = threading.Condition()
        threading.Thread(target=self.rx, daemon=True).start()

    def rx(self):
        buf = b""
        while True:
            d = self.s.recv(65536)
            if not d: return
            buf += d
            while len(buf) >= 12:
                ln = struct.unpack_from("<I", buf, 2)[0]
                if len(buf) < 12 + ln: break
                typ, err, rid = buf[6], buf[7], struct.unpack_from("<I", buf, 8)[0]
                body = buf[12:12 + ln]; buf = buf[12 + ln:]
                if rid == 0xFFFFFFFF: self.ev.put((typ, body))
                else:
                    with self.cv: self.resp[rid] = (typ, err, body); self.cv.notify_all()

    def req(self, cmd, body=b""):
        self.rid += 1
        self.s.sendall(struct.pack("<BBIIB", 2, 2, len(body), self.rid, cmd) + body)
        with self.cv:
            self.cv.wait_for(lambda: self.rid in self.resp, 10)
            return self.resp.pop(self.rid)

    def read(self, a, n):
        _, _, b = self.req(0x01, struct.pack("<BHHBH", 0, a, a + n - 1, 0, 0))
        return b[2:2 + n]
    def write(self, a, data):
        self.req(0x02, struct.pack("<BHHBH", 0, a, a + len(data) - 1, 0, 0) + bytes(data))
    def go(self): self.req(0xAA)
    def keys(self, text): self.req(0x72, bytes([len(text)]) + text.encode("latin1"))
    def screen(self):
        raw = self.read(0x0400, 1000)
        out = []
        for r in range(25):
            row = ""
            for c in raw[r * 40:(r + 1) * 40]:
                c &= 0x7F
                row += chr(c + 64) if c < 32 else chr(c) if c < 64 else "?"
            out.append(row.rstrip())
        return out

def main(kernal, scenario="basic"):
    scan = scenario == "scan"
    workdir = pathlib.Path(tempfile.mkdtemp())
    drives = ["-drive8type", "0"]
    if scan:       # real 1541s at 8 and 10, each with a blank disk so that a LOAD gets a quick FILE NOT FOUND instead of a long "no disk" wait
        for n in (8, 10):
            subprocess.run(["c1541", "-format", "real%d,r%d" % (n, n), "d64", str(workdir / ("real%d.d64" % n))], check=True, capture_output=True)
        rnd = random.Random(5)
        real = {"rprg": b"\x00\x40" + bytes(rnd.randrange(256) for _ in range(5000)),         # a PRG of 20 sectors
                "rseq": bytes([13, 0, 10, 255, 34, 44, 58] * 400),                                 # SEQ with CR, NUL, quotes, commas
                "test": b"\x01\x08" + b"real drive version"}                                      # exists on the SD already
        for nm_, data in real.items():
            (workdir / (nm_ + ".bin")).write_bytes(data)
            subprocess.run(["c1541", "-attach", str(workdir / "real8.d64"), "-write", str(workdir / (nm_ + ".bin")), nm_ + (",s" if nm_ == "rseq" else "")],
                           check=True, capture_output=True)
        drives = ["-drive8type", "1541", "-drive8truedrive", "-drive9type", "0", "-drive10type", "1541", "-drive10truedrive", "-drive11type", "0",
                  "-8", str(workdir / "real8.d64"), "-10", str(workdir / "real10.d64")]
    # the banner bytes as VD_BannerUnits (firmware/IOH_VDrive.c) writes them into the served image: SD automatic, USB automatic
    # in `scan` (a stick is attached there) and absent in `basic`
    kimg = bytearray(pathlib.Path(kernal).read_bytes())
    tag = kimg.find(b", VDRIVE \x00")
    if tag >= 0: kimg[tag + 10:tag + 12] = bytes([0xFF, 0xFF if scan else 0])
    kernal = str(workdir / "kernal.bin"); pathlib.Path(kernal).write_bytes(kimg)
    vice = subprocess.Popen(["x64sc", "-warp", *drives, "-kernal", kernal,
                             "-binarymonitor", "-binarymonitoraddress", "ip4://127.0.0.1:6502"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = Mon()
        image, big = make_image(workdir)
        disk = Disk(image)
        io = VIO()
        # a fake SD card for CD:/mount/renumber tests (cdtest.bas, renum.bas)
        sdroot = workdir / "sdroot"; sdroot.mkdir()
        (sdroot / "mock.d64").write_bytes(image)
        for fmt, nm_, fn in (("d81", "bigdisk", "big.d81"), ("d71", "twodisk", "two.d71")):
            subprocess.run(["c1541", "-format", nm_ + ",xx", fmt, str(sdroot / fn)], check=True, capture_output=True)
            (workdir / "h.prg").write_bytes(b"\x01\x08" + bytes.fromhex("0b08 0a00 99 22 48 49 22 00 00 00".replace(" ", "")))   # 10 PRINT"HI"
            subprocess.run(["c1541", "-attach", str(sdroot / fn), "-write", str(workdir / "h.prg"), "hello"], check=True, capture_output=True)
        subprocess.run(["c1541", "-format", "vdtest,01", "d64", str(sdroot / "vdtest.d64")], check=True, capture_output=True)
        hs = {"cwd": sdroot, "disks": [], "entries": [], "i": 0, "saved": [], "cur": None, "snap": None}
        def h_mount(ctx, unit, name, out):
            p = hs["cwd"] / name.decode().lower()
            if not p.exists() or p.is_dir(): return 62
            d = Disk(p.read_bytes()); hs["disks"].append(d); hs["cur"] = d; out[0] = ctypes.addressof(d.d); return 0
        def h_unmount(ctx, unit): hs["cur"] = None
        def h_chdir(ctx, unit, name):
            n = name.decode().lower()
            if n == "/": hs["cwd"] = sdroot; return 0
            q = hs["cwd"].parent if n == ".." else hs["cwd"] / n
            if not q.is_dir(): return 62
            hs["cwd"] = q; return 0
        def h_dir_open(ctx, unit, title):
            ctypes.memmove(title, b"SD:/\0", 5)
            hs["entries"] = sorted(hs["cwd"].iterdir()); hs["i"] = 0; return 0
        def h_dir_next(ctx, unit, name, size, is_dir):
            if hs["i"] >= len(hs["entries"]): return 0
            p = hs["entries"][hs["i"]]; hs["i"] += 1
            ctypes.memmove(name, p.name.upper().encode()[:16] + b"\0", min(len(p.name), 16) + 1)
            size[0] = 0 if p.is_dir() else p.stat().st_size; is_dir[0] = int(p.is_dir()); return 1
        cbs = HostStruct()
        cbs.mount = HostStruct.MOUNT(h_mount); cbs.unmount = HostStruct.UNMOUNT(h_unmount); cbs.chdir = HostStruct.CHDIR(h_chdir)
        cbs.dir_open = HostStruct.DIROPEN(h_dir_open); cbs.dir_next = HostStruct.DIRNEXT(h_dir_next)
        cbs.save_devices = HostStruct.SAVEDEV(lambda c, a, b: hs["saved"].append((a, b)))
        def h_push(ctx, unit): hs["snap"] = (hs["cwd"], hs["cur"])
        def h_pop(ctx, unit, out):
            if not out: return 0
            hs["cwd"], hs["cur"] = hs["snap"]
            out[0] = ctypes.addressof(hs["cur"].d) if hs["cur"] else None; return 0
        cbs.push = HostStruct.PUSH(h_push); cbs.pop = HostStruct.POP(h_pop)
        LIB.vdrive_set_host(io.v, ctypes.byref(cbs))
        io.attach(0, disk)
        if scan:
            usb = workdir / "usb.d64"
            subprocess.run(["c1541", "-format", "usbdisk,us", "d64", str(usb)], check=True, capture_output=True)
            usbd = Disk(usb.read_bytes())
            io.attach(1, usbd, present=True)
        else:
            io.attach(1, None, present=False)      # the USB drive is not present in the mock
        io.arm_scan()                              # the first IEC transaction triggers the bus scan, like on the TR+
        log = []
        last_devs = [None]
        fault = {"at": 0, "n": 0}               # fault injection: answer NOT_MINE to the Nth FLOAD_BLK of a run

        def service(timeout, limit=None):
            """handle mailbox stops until quiet for `timeout` s (or `limit` s in total, for programs that never go quiet)"""
            deadline = time.time() + limit if limit else None
            while True:
                if deadline and time.time() > deadline: return
                try: typ, body = m.ev.get(timeout=timeout)
                except queue.Empty: return
                if typ == 0x62:  # stopped
                    regs = m.read(MBX, 6)
                    c, a, ln = regs[0], regs[1], regs[5]
                    if 1 <= c <= 15:
                        if c in (9, 11, 12):                    # commands that consume the window
                            io.win_write(bytes(m.read(WIN, WIN_SIZE)))
                        nm, d, st = io.call(c, a, ln, allow_scan=True)
                        if c == 10 and fault["at"]:
                            fault["n"] += 1
                            if fault["n"] == fault["at"]: nm = True
                        log.append((c, a, d, st, nm))
                        if c in (9, 10):                        # commands that produce window data
                            m.write(WIN, io.win_read(WIN_SIZE))
                        m.write(DATA, [d]); m.write(ST, [st])
                        devs = (io.r(R_DEV0), io.r(R_DEV1))
                        if devs != last_devs[0]: m.write(DEV0, list(devs)); last_devs[0] = devs
                        m.write(STAT, [NOT_MINE if nm else io.stat])      # (nm may also come from fault injection)
                        m.write(CMD, [0])
                    m.go()

        m.write(MBX, [0] * 5)
        cp = m.req(0x12, struct.pack("<HHBBBB", CMD, CMD, 1, 1, 2, 0))  # store checkpoint on CMD
        cp_num = struct.unpack_from("<I", cp[2], 0)[0]
        time.sleep(0.5); service(1.0)       # boot

        ok = True
        def run(keys, expect=None, absent=None):
            nonlocal ok
            m.keys(keys); service(1.5)
            scr = m.screen()
            hit = expect is None or any(expect in r for r in scr)
            if absent: hit = hit and not any(absent in r for r in scr)
            print(("PASS" if hit else "FAIL"), repr(keys), "| expect", expect)
            if not hit: ok = False; print("\n".join(scr))
        def check(label, cond):
            nonlocal ok
            print(("PASS" if cond else "FAIL"), label)
            ok = ok and cond

        banner = " 64K RAM, 38911 FREE, VDRIVE " + ("9+11" if scan else "8")
        check("banner: %r (the KERNAL probed the bus itself at power-on)" % banner, banner in m.screen())
        if scan:
            # the first transaction below is a LOAD: the Teensy answers "scan first", the KERNAL probes 8, 9, 10, 11 and reports the free ones
            run('LOAD"$",9\r', "SEARCHING FOR $")
            run('LIST\r', '0 "MOCKDISK')
            check("scan: SD drive is device 9 (8 is a real 1541), USB drive device 11 (10 is a real 1541): regs %r" % (bytes(m.read(DEV0, 2)),),
                  bytes(m.read(DEV0, 2)) == bytes([9, 11]))
            check("scan: the bus was probed (SCAN, then FREE for 9 and 11 only)",
                  [(e[0], e[1]) for e in log if e[0] in (14, 15)] == [(14, 0), (15, 9), (15, 11)])
            run('LOAD"$",11\r', "SEARCHING FOR $")
            run('LIST\r', '0 "USBDISK')
            n0 = len(log)
            run('LOAD"X",8\r', "FILE NOT FOUND", absent="DEVICE NOT PRESENT")     # the real drive 8 answers (with a DOS error), not us
            run('LOAD"X",10\r', "FILE NOT FOUND", absent="DEVICE NOT PRESENT")
            check("scan: nothing for 8 or 10 was handled by the mock", all(e[4] for e in log[n0:] if e[1] in (8, 10)))
            run('LOAD"X",12\r', "DEVICE NOT PRESENT")
            # how long does probing an empty number take with drives on the bus? (jiffies = 1/60 s; LISTEN + SECOND as the scan does it)
            run('T=TI:POKE780,12:SYS65457:POKE780,111:SYS65427:PRINT"P=";PEEK(144);"T=";TI-T\r', "P= 128")
            import re; ticks = [int(x) for x in re.findall(r"P= 128 T= (\d+)", "".join(m.screen()))]
            check("scan: probing an empty number takes %s jiffies (< 4 = 66 ms)" % ticks, ticks and ticks[-1] < 4)
            # --- util/vdcopy: copy from the real 1541 (8) to the SD drive (9), then the other directions ---
            def vdcopy(sel, replace, expect, src="", dst=""):
                m.keys('RUN\r'); service(1.5)
                m.keys(src + '\r'); service(0.5); m.keys(dst + '\r'); service(0.5)     # default units: 8 -> 9 (PEEK(56854) says the SD is 9)
                for _ in range(40):                       # the directory is read from the true-drive 1541
                    service(1.0, limit=3)
                    if any("COPY: A=ALL" in r for r in m.screen()): break
                for ch in sel: m.keys(ch); service(0.3)
                m.keys('\r'); service(0.5); m.keys('Y' if replace else 'N'); service(1.0)
                for _ in range(60):
                    service(1.0, limit=3)
                    if any("COPIED," in r for r in m.screen()): break
                scr = m.screen()
                check("vdcopy %r replace=%s: %s" % (sel, replace, expect), any(expect in " ".join(r.split()) for r in scr))
                if not ok: print("\n".join(scr))
                m.keys('\r'); service(1.0)                                  # RETURN at the prompt quits
            run('LOAD"VDCOPY",9\r', "READY.")
            vdcopy("A", False, "2 COPIED, 1 SKIPPED, 0 FAILED")           # TEST exists on the SD
            img = workdir / "after.d64"; img.write_bytes(disk.dump())
            for nm_ in ("rprg", "rseq"):
                f = workdir / (nm_ + ".out")
                subprocess.run(["c1541", "-attach", str(img), "-read", nm_ + (",s" if nm_ == "rseq" else ""), str(f)], capture_output=True)
                want = real[nm_]
                got = f.read_bytes() if f.exists() else b""
                check("vdcopy: %s copied byte for byte" % nm_, got == want)
                if got != want:
                    print("  got %d bytes, want %d, first difference at %s" % (len(got), len(want), next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), None)))
            vdcopy("3", True, "1 COPIED, 0 SKIPPED, 0 FAILED")            # replace the existing TEST
            vdcopy("1,2-3", False, "0 COPIED, 3 SKIPPED, 0 FAILED")       # a list and a range, all exist now
            img.write_bytes(disk.dump())
            subprocess.run(["c1541", "-attach", str(img), "-read", "test", str(workdir / "test.out")], capture_output=True)
            check("vdcopy: the replaced TEST is the real drive's", (workdir / "test.out").exists() and (workdir / "test.out").read_bytes() == real["test"])
            def usb_file(name):
                f = workdir / "usb.out"; f.unlink(missing_ok=True)
                img_ = workdir / "usb_after.d64"; img_.write_bytes(usbd.dump())
                subprocess.run(["c1541", "-attach", str(img_), "-read", name, str(f)], capture_output=True)
                return f.read_bytes() if f.exists() else b""
            # vdrive -> vdrive: SD (9) to USB (11); the SD listing is TEST BIG ... vdcopy RPRG RSEQ, so 1 = TEST and 2 = BIG
            vdcopy("1", False, "1 COPIED, 0 SKIPPED, 0 FAILED", "9", "11")
            check("vdcopy: SD -> USB, TEST byte for byte", usb_file("test") == real["test"])
            vdcopy("2", False, "1 COPIED, 0 SKIPPED, 0 FAILED", "9", "11")
            check("vdcopy: SD -> USB, BIG (6000 bytes) byte for byte", usb_file("big") == big)
            vdcopy("1-2", False, "0 COPIED, 2 SKIPPED, 0 FAILED", "9", "11")
            # vdrive -> real drive: SD (9) to the 1541 (8), BIG; the 1541 then lists RPRG RSEQ TEST BIG. Back to the SD with replace (8 -> 9)
            # proves the bytes the real drive stored, and 8 -> 11 puts RPRG on the USB image.
            vdcopy("2", False, "1 COPIED, 0 SKIPPED, 0 FAILED", "9", "8")
            vdcopy("2", False, "0 COPIED, 1 SKIPPED, 0 FAILED", "9", "8")        # the real drive answers 63
            vdcopy("4", True, "1 COPIED, 0 SKIPPED, 0 FAILED", "8", "9")
            sd_img = workdir / "sd_after.d64"; sd_img.write_bytes(disk.dump())
            (workdir / "big.out").unlink(missing_ok=True)
            subprocess.run(["c1541", "-attach", str(sd_img), "-read", "big", str(workdir / "big.out")], capture_output=True)
            check("vdcopy: BIG went SD -> 1541 -> SD byte for byte", (workdir / "big.out").exists() and (workdir / "big.out").read_bytes() == big)
            vdcopy("1", False, "1 COPIED, 0 SKIPPED, 0 FAILED", "8", "11")
            check("vdcopy: 1541 -> USB, RPRG byte for byte", usb_file("rprg") == real["rprg"])
            return 0 if ok else 1

        # --- read side ---
        run('LOAD"$",8\r', "SEARCHING FOR $")
        check("scan: the first transaction made the KERNAL probe the (empty) bus and the SD drive keeps 8",
              [(e[0], e[1]) for e in log if e[0] in (14, 15)] == [(14, 0), (15, 8)] and bytes(m.read(DEV0, 2)) == bytes([8, 0]))
        run('LIST\r', '0 "MOCKDISK')
        run('LIST\r', '"BIG"')
        run('LOAD"$:T*",8\r', "SEARCHING FOR $:T*")
        run('LIST\r', '"TEST"')
        run('NEW\r')
        run('LOAD"TEST",8\r', "READY.")
        run('LIST\r', '10 PRINT"HI"')
        run('LOAD"NOPE",8\r', "FILE NOT FOUND")
        run('LOAD"X",11\r', "DEVICE NOT PRESENT")
        run('LOAD"X",9\r', "DEVICE NOT PRESENT")
        run('LOAD"BIG",8,1\r', "READY.")
        check("BIG (6000 bytes, multi-sector) loaded to $2000 intact", bytes(m.read(0x2000, len(big) - 2)) == big[2:])
        n_blk = sum(1 for e in log if e[0] == 10)
        n_acptr = sum(1 for e in log if e[0] == 6)
        check("LOAD used the fast path (%d block calls, %d byte-level ACPTR calls in the whole run so far)" % (n_blk, n_acptr),
              n_blk >= 24 and n_acptr == 0)

        # --- names and the 128-byte window: 127 characters still go the fast way, 128 and more fall back to the byte-level open ---
        n0 = len(log)
        run('N$="":FORI=1TO127:N$=N$+"X":NEXT:LOAD N$,8\r', "FILE NOT FOUND")
        check("127-character name: fast open with the whole name in the window",
              any(e[0] == 9 for e in log[n0:]) and not any(e[0] == 5 for e in log[n0:]))
        n0 = len(log)
        run('N$="":FORI=1TO130:N$=N$+"X":NEXT:LOAD N$,8\r', "FILE NOT FOUND")
        check("130-character name: no fast open, the name goes byte by byte (CIOUT)",
              not any(e[0] == 9 for e in log[n0:]) and sum(1 for e in log[n0:] if e[0] == 5) >= 130)

        # --- regression: CIOUT must return the byte sent (CHROUT tail-jumps into it); numbers followed by more PRINT items broke BASIC ---
        run('NEW\r')
        run('OPEN2,8,2,"NUMTST,S,W"\r')
        run('PRINT#2,1;"A";2:PRINT#2,12;34\r', None, absent="SYNTAX")
        run('CLOSE2\r')
        run('OPEN15,8,15,"S:NUMTST":CLOSE15\r')

        # --- the shipped BASIC test programs, run for real inside VICE ---
        def prog(name, answers, expect, wait=3.0, dev=8):
            run('NEW\r')
            run('LOAD"%s",%d\r' % (name.upper(), dev), "READY.")
            m.keys('RUN\r'); service(1.0)
            for a in answers:
                m.keys(a + '\r'); service(wait)
            scr = m.screen()
            norm = [" ".join(r.split()) for r in scr]
            ok_ = all(any(" ".join(e.split()) in r for r in norm) for e in expect)
            print(("PASS" if ok_ else "FAIL"), "BASIC program", name, "| expect", expect)
            if not ok_:
                print("\n".join(scr))
            return ok_
        ok = prog("seqtest", [""], ["SEQ TEST PASS", "CHECKED 100 LINES, 0 ERRORS"]) and ok
        ok = prog("dostest", [""], ["10 PASSED, 0 FAILED"]) and ok
        ok = prog("blktest", [""], ["12 PASSED, 0 FAILED"]) and ok
        ok = prog("reltest", [""], ["14 PASSED, 0 FAILED"], wait=8.0) and ok
        ok = prog("dos2test", [""], ["12 PASSED, 0 FAILED"], wait=6.0) and ok
        run('NEW\r')
        run('LOAD"SOAK",8\r', "READY.")
        m.keys('RUN\r')
        for _ in range(12):                                           # let it make several passes (each reloads BIG and checksums it)
            service(0.5, limit=5)
            if any("PASS 3 " in r for r in m.screen()): break
        scr = m.screen()
        soak_ok = any("PASS 3 ERRORS 0" in r.replace("  ", " ") for r in scr) and not any("MISMATCH" in r for r in scr)
        print(("PASS" if soak_ok else "FAIL"), "BASIC soak program: 3 load+verify passes, 0 errors")
        if not soak_ok: print("\n".join(scr))
        ok = ok and soak_ok
        m.keys(' '); service(1.5, limit=15)                            # any key stops the soak loop

        # --- fault injection: a bad answer in the middle of a LOAD must give an error, never silent corruption ---
        m.write(0x2000, bytes(6000))                                  # known contents to see whether anything was copied after the fault
        fault["at"], fault["n"] = 3, 0
        run('LOAD"BIG",8,1\r', "DEVICE NOT PRESENT")
        fault["at"] = 0
        run('LOAD"BIG",8,1\r', "READY.")                             # and the machine/drive recover cleanly
        check("LOAD works again after the injected fault", bytes(m.read(0x2000, len(big) - 2)) == big[2:])

        # --- SAVE a BASIC program, replace, collision ---
        run('NEW\r')
        run('10 PRINT"SAVED"\r')
        run('SAVE"SAVEME",8\r', "SAVING SAVEME")
        run('NEW\r')
        run('LOAD"SAVEME",8\r', "READY.")
        run('RUN\r', "SAVED")
        run('VERIFY"SAVEME",8\r', "OK")                           # VERIFY takes the stock byte-level path
        run('POKE 2049,0:POKE 2050,0\r')                           # corrupt the program in RAM
        run('VERIFY"SAVEME",8\r', "VERIFY  ERROR")
        run('NEW\r')
        run('10 PRINT"V2"\r')
        run('SAVE"SAVEME",8\r')                                  # exists: drive refuses (63)
        run('20 OPEN15,8,15:INPUT#15,A,B$:PRINT"ER=";A;B$:CLOSE15\r')
        run('RUN\r', "ER= 63 FILE EXISTS")
        run('SAVE"@0:SAVEME",8\r', "SAVING @0:SAVEME")
        run('NEW\r')
        run('LOAD"SAVEME",8\r', "READY.")
        run('LIST\r', 'PRINT"V2"')
        # no device number: 8, as on the SX-64 (stock: 1, the tape); "\x93" clears the screen so an earlier listing can't pass for this one
        run('NEW\r')
        run('\x93LOAD"SAVEME"\r', "SEARCHING FOR SAVEME", absent="PRESS PLAY")
        run('\x93LIST\r', 'PRINT"V2"')
        run('\x93VERIFY"SAVEME"\r', "OK", absent="PRESS PLAY")
        run('10 PRINT"V3"\r')
        run('\x93SAVE"@0:SAVEME"\r', "SAVING @0:SAVEME", absent="PRESS RECORD")
        run('NEW\r')
        run('\x93LOAD"SAVEME",8\r', "READY.")
        run('\x93LIST\r', 'PRINT"V3"')

        # --- sequential file round trip ---
        run('NEW\r')
        run('10 OPEN2,8,2,"DATA,S,W":PRINT#2,"HELLO":PRINT#2,"WORLD":CLOSE2\r')
        run('20 OPEN3,8,3,"DATA,S,R":INPUT#3,A$,B$:CLOSE3:PRINT"GOT:";A$;B$\r')
        run('RUN\r', "GOT:HELLOWORLD")

        # --- big SAVE from RAM (multi-sector), then scratch ---
        rnd = random.Random(9)
        ram = bytes(rnd.randrange(256) for _ in range(6000))
        m.write(0x2000, ram)
        run('NEW\r')
        run('POKE43,0:POKE44,32:POKE45,112:POKE46,55:SAVE"BIG2",8\r', "SAVING BIG2")
        run('NEW\r')
        run('OPEN15,8,15,"S:SAVEME":CLOSE15\r')
        run('LOAD"SAVEME",8\r', "FILE NOT FOUND")

        # --- cdtest + renum: they unmount / renumber the drive, so they run after the disk checks and before the dead-mailbox test ---
        ok = prog("cdtest", [""], ["6 PASSED, 0 FAILED"]) and ok
        run('OPEN15,8,15,"CD:MOCK.D64":CLOSE15\r')                    # remount the mock disk
        ok = prog("renum", ["8", "10"], ["NEW DEVICE 10 ANSWERS: 0 OK"]) and ok
        check("U0> reported the new number for persistence", hs["saved"] and hs["saved"][-1] == (10, 0))
        ok = prog("renum", ["10", "8"], ["NEW DEVICE 8 ANSWERS: 0 OK"], dev=10) and ok
        check("... an explicit 8 is a fixed number now (saved as 8, not as 'automatic')", hs["saved"][-1] == (8, 0))
        run('OPEN15,8,15,"U0>"+CHR$(0):CLOSE15\r')                    # CHR$(0): back to automatic numbering
        check("U0>CHR$(0) saves 'automatic'", hs["saved"][-1] == (0, 0))
        run('LOAD"$",8\r', "SEARCHING FOR $")
        check("... and the drive still answers as 8", bytes(m.read(DEV0, 1)) == bytes([8]))

        # --- paths in LOAD names, UI, D71 / D81 images ---
        run('NEW\r')
        run('10 OPEN15,8,15,"UI":INPUT#15,A,B$:PRINT"UI=";A;B$:CLOSE15\r')
        run('RUN\r', "UI= 73 TRVDRIVE V1.0")
        run('NEW\r')
        run('OPEN15,8,15,"CD:/":CLOSE15\r')
        run('LOAD"$/MOCK.D64",8\r', "SEARCHING FOR $/MOCK.D64")      # lists the image (mounted only while it is listed)
        run('LIST\r', '0 "MOCKDISK')
        run('LOAD"$",8\r', "SEARCHING FOR $"); run('LIST\r', '"SD:/')     # nothing stayed selected
        run('LOAD"/MOCK.D64/TEST",8\r', "READY.")                       # a file through a path
        run('LIST\r', '10 PRINT"HI"')
        run('LOAD"BIG.D81",8\r', "FILE NOT FOUND")                      # with an image mounted a bare name is a file inside it
        run('OPEN15,8,15,"CD:_":CLOSE15\r')                                                # nothing mounted: now it is an image to mount,
        run('LOAD"BIG.D81",8\r', "READY.")                  # and its directory is the "file" (listed only, not mounted)
        run('LIST\r', '0 "BIGDISK')
        run('LOAD"$",8\r', "SEARCHING FOR $"); run('LIST\r', '"SD:/')
        run('LOAD"$==BIG.D81",8\r', "READY."); run('LIST\r', '0 "BIGDISK')     # $== selects it for good
        run('LOAD"HELLO",8\r', "READY."); run('LIST\r', '10 PRINT"HI"')  # load from the D81 (fast path through the 1581 layout)
        run('NEW\r'); run('10 PRINT"SAVED81"\r'); run('SAVE"S81",8\r', "SAVING S81")
        run('NEW\r'); run('LOAD"S81",8\r', "READY."); run('RUN\r', "SAVED81")
        run('LOAD"$/TWO.D71",8\r', "SEARCHING FOR $/TWO.D71"); run('LIST\r', '0 "TWODISK')
        run('LOAD"$",8\r', "SEARCHING FOR $"); run('LIST\r', '0 "BIGDISK')    # the D81 is still the selected one
        run('LOAD"$==TWO.D71",8\r', "READY."); run('LIST\r', '0 "TWODISK')
        run('LOAD"HELLO",8\r', "READY."); run('LIST\r', '10 PRINT"HI"')
        run('LOAD"$/",8\r', "SEARCHING FOR $/"); run('LIST\r', '"SD:/')               # a folder path: the host directory
        run('OPEN15,8,15,"CD:/MOCK.D64":CLOSE15\r')                                       # leave the card as the later tests expect
        run('NEW\r')

        # --- handler absent (BUSY never clears): KERNAL must time out and use the real IEC code ---
        m.req(0x13, struct.pack("<I", cp_num))                  # delete the checkpoint: nobody answers any more
        m.write(STAT, [0x80])
        run('LOAD"X",8\r', "DEVICE NOT PRESENT")
        run('SAVE"X",8\r')                                       # same for the SAVE hook
        run('OPEN1,8,15:PRINT"ST=";ST', None)
        run('LOAD"X",8\r', "DEVICE NOT PRESENT")

        # --- the resulting disk must be a valid D64 that c1541 can read ---
        img = workdir / "after.d64"; img.write_bytes(disk.dump())
        def c(*a):
            return subprocess.run(["c1541", "-attach", str(img), *a], capture_output=True, text=True, encoding="latin1").stdout
        listing = c("-list").lower()
        check("c1541 lists SEQ file 'data' and 'big2', not scratched 'saveme'",
              '"data"' in listing and '"big2"' in listing and '"saveme"' not in listing)
        out = workdir / "big2.out"; c("-read", "big2", str(out))
        check("BIG2 (saved from RAM through the bus) read back by c1541 == RAM", out.exists() and out.read_bytes() == b"\x00\x20" + ram)
        before = disk.dump()[357 * 256 + 4: 357 * 256 + 0x90]
        c("-validate")
        check("BAM written by the drive equals what c1541 validate computes",
              img.read_bytes()[357 * 256 + 4: 357 * 256 + 0x90] == before)
        n_save = sum(1 for e in log if e[0] == 12)
        check("SAVE used the fast path (%d block writes)" % n_save, n_save >= 24)
        print("mailbox calls:", len(log))
        return 0 if ok else 1
    finally:
        vice.terminate()
        shutil.rmtree(workdir, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "basic"))
