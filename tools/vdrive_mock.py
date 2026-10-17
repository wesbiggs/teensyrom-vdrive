#!/usr/bin/env python3
"""VICE-side mock of the TR+ virtual-drive mailbox (RAM at MBX, see Makefile).

The drive itself is the real C code (d64/vdrive.c via ctypes); only the mailbox
transport is faked here.

Drives x64sc through its binary monitor: a store-checkpoint on MBX+0 (CMD)
stops the CPU, this script plays the Teensy (state machine for IEC devices
8/9 backed by a tiny in-memory 'disk'), writes DATA/STAT/ST and resumes.
usage: vdrive_mock.py KERNAL.bin
"""
import socket, struct, subprocess, sys, time, threading, queue

MBX = 0xC800
WIN = 0xCA00                      # IO2 window (RAM here)
CMD, ARG, DATA, STAT, ST, LEN = (MBX + i for i in range(6))
(C_LISTEN, C_TALK, C_SECOND, C_TKSA, C_CIOUT, C_ACPTR, C_UNLSN, C_UNTLK) = range(1, 9)
NOT_MINE = 0xA5                   # STAT answer tokens (see d64/vdrive_io.h)
ANSWERED = 0x5A

# ---- drive backed by libd64 ------------------------------------------------
import ctypes, os, random, shutil, tempfile, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from d64py import LIB, Disk, VDrive, VIO
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

def main(kernal):
    vice = subprocess.Popen(["x64sc", "-warp", "-drive8type", "0", "-kernal", kernal,
                             "-binarymonitor", "-binarymonitoraddress", "ip4://127.0.0.1:6502"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    workdir = pathlib.Path(tempfile.mkdtemp())
    try:
        m = Mon()
        image, big = make_image(workdir)
        disk = Disk(image)
        io = VIO()
        # a fake SD card for CD:/mount/renumber tests (cdtest.bas, renum.bas)
        sdroot = workdir / "sdroot"; sdroot.mkdir()
        (sdroot / "mock.d64").write_bytes(image)
        subprocess.run(["c1541", "-format", "vdtest,01", "d64", str(sdroot / "vdtest.d64")], check=True, capture_output=True)
        hs = {"cwd": sdroot, "disks": [], "entries": [], "i": 0, "saved": []}
        def h_mount(ctx, unit, name, out):
            p = hs["cwd"] / name.decode().lower()
            if not p.exists() or p.is_dir(): return 62
            d = Disk(p.read_bytes()); hs["disks"].append(d); out[0] = ctypes.addressof(d.d); return 0
        def h_unmount(ctx, unit): pass
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
        LIB.vdrive_set_host(io.v, ctypes.byref(cbs))
        io.attach(0, disk)
        io.attach(1, None, present=False)          # device 9 ("USB") not present in the mock
        log = []
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
                    if 1 <= c <= 13:
                        if c in (9, 11, 12):                    # commands that consume the window
                            io.win_write(bytes(m.read(WIN, 256)))
                        nm, d, st = io.call(c, a, ln)
                        if c == 10 and fault["at"]:
                            fault["n"] += 1
                            if fault["n"] == fault["at"]: nm = True
                        log.append((c, a, d, st, nm))
                        if c in (9, 10):                        # commands that produce window data
                            m.write(WIN, io.win_read(256))
                        m.write(DATA, [d]); m.write(ST, [st])
                        m.write(STAT, [NOT_MINE if nm else ANSWERED])
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

        # --- read side ---
        run('LOAD"$",8\r', "SEARCHING FOR $")
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
        ok = prog("dostest", [""], ["0 FAILED"]) and ok
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
        check("... and the default again afterwards", hs["saved"][-1] == (0, 0))

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
    sys.exit(main(sys.argv[1]))
