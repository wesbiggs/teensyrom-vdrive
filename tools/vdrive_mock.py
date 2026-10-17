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
NOT_MINE = 0x40

# ---- drive backed by libd64 ------------------------------------------------
import ctypes, os, random, shutil, tempfile, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from d64py import LIB, Disk, VDrive, VIO

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
        io.attach(0, disk)
        io.attach(1, None, present=False)          # device 9 ("USB") not present in the mock
        log = []

        def service(timeout):
            """handle mailbox stops until quiet for `timeout` s"""
            while True:
                try: typ, body = m.ev.get(timeout=timeout)
                except queue.Empty: return
                if typ == 0x62:  # stopped
                    regs = m.read(MBX, 6)
                    c, a, ln = regs[0], regs[1], regs[5]
                    if 1 <= c <= 13:
                        if c in (9, 11, 12):                    # commands that consume the window
                            io.win_write(bytes(m.read(WIN, 256)))
                        nm, d, st = io.call(c, a, ln)
                        log.append((c, a, d, st, nm))
                        if c in (9, 10):                        # commands that produce window data
                            m.write(WIN, io.win_read(256))
                        m.write(DATA, [d]); m.write(ST, [st])
                        m.write(STAT, [NOT_MINE if nm else 0])
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
