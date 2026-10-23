"""ctypes wrapper over build/libd64 (host side), shared by tests and the VICE mock."""
import ctypes, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIB = ctypes.CDLL(str(ROOT / "build" / "libd64.dylib"))
for fn in ("d64_sizeof_d64", "d64_sizeof_mem", "d64_sizeof_dir", "d64_sizeof_file", "d64_sizeof_entry"):
    getattr(LIB, fn).restype = ctypes.c_size_t
LIB.d64_blocks_free.restype = ctypes.c_int
LIB.d64_sizeof_wfile.restype = ctypes.c_size_t
LIB.d64_build_dir.restype = ctypes.c_size_t
LIB.d64_build_dir.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]


class Disk:
    """ctypes wrapper over libd64 (memory image)."""
    def __init__(self, image: bytes):
        self.image = ctypes.create_string_buffer(image, len(image))
        self.mem = ctypes.create_string_buffer(LIB.d64_sizeof_mem())
        self.d = ctypes.create_string_buffer(LIB.d64_sizeof_d64())
        LIB.d64_mem_init(self.mem, self.image, ctypes.c_size_t(len(image)))
        assert LIB.d64_open_mem(self.d, self.mem) == 0

    def header(self):
        n, i, o = (ctypes.create_string_buffer(17), ctypes.create_string_buffer(3), ctypes.create_string_buffer(3))
        assert LIB.d64_header(self.d, n, i, o) == 0
        return n.value, i.value, o.value

    def entries(self):
        it = ctypes.create_string_buffer(LIB.d64_sizeof_dir())
        e = ctypes.create_string_buffer(LIB.d64_sizeof_entry())
        assert LIB.d64_dir_first(self.d, it) == 0
        out = []
        while True:
            r = LIB.d64_dir_next(self.d, it, e)
            assert r >= 0
            if r == 0: return out
            raw = e.raw
            out.append(dict(type=raw[0], track=raw[1], sector=raw[2],
                            blocks=raw[4] | raw[5] << 8, name=raw[6:].split(b"\0")[0]))

    def find(self, pat):
        e = ctypes.create_string_buffer(LIB.d64_sizeof_entry())
        r = LIB.d64_find(self.d, pat, e)
        return (r, e)

    def read_file(self, pat, chunk=100):
        r, e = self.find(pat)
        if r != 1: return None
        f = ctypes.create_string_buffer(LIB.d64_sizeof_file())
        LIB.d64_file_open(self.d, e, f)
        data = b""
        buf = ctypes.create_string_buffer(chunk)
        while True:
            n = LIB.d64_file_read(self.d, f, buf, ctypes.c_size_t(chunk))
            assert n >= 0
            if n == 0: break
            data += buf.raw[:n]
        return data

    # ---- write side ----
    def write_file(self, name, data, type=2, replace=False, chunk=1000):
        """returns 0 on success, else a D64_E_* code"""
        w = ctypes.create_string_buffer(LIB.d64_sizeof_wfile())
        r = LIB.d64_create(self.d, name, type, int(replace), w)
        if r: return r
        for i in range(0, max(len(data), 1), chunk):
            part = data[i:i + chunk]
            if not part: break
            r = LIB.d64_write(self.d, w, part, ctypes.c_size_t(len(part)))
            if r < 0: return r
        return LIB.d64_close_write(self.d, w)

    def scratch(self, pat): return LIB.d64_scratch(self.d, pat)
    def rename(self, old, new): return LIB.d64_rename(self.d, old, new)
    def format(self, name, id=None): return LIB.d64_format(self.d, name, id)
    def free(self): return LIB.d64_blocks_free(self.d)
    def dump(self): return bytes(self.image.raw)

    def listing(self, pat=None):
        buf = ctypes.create_string_buffer(8192)
        n = LIB.d64_build_dir(self.d, pat, buf, 8192)
        return buf.raw[:n]


LIB.vdrive_sizeof.restype = ctypes.c_size_t
LIB.vdrive_mailbox.restype = ctypes.c_int
(VD_LISTEN, VD_TALK, VD_SECOND, VD_TKSA, VD_CIOUT, VD_ACPTR, VD_UNLSN, VD_UNTLK) = range(1, 9)


class VDrive:
    """Python view of vdrive_t that speaks the KERNAL mailbox protocol."""
    def __init__(self):
        self.v = ctypes.create_string_buffer(LIB.vdrive_sizeof())
        LIB.vdrive_init(self.v)
        self.disks = {}

    def attach(self, unit, disk, present=True):
        self.disks[unit] = disk
        LIB.vdrive_attach(self.v, unit, disk.d if disk else None, int(present))

    def cmd(self, c, arg=0):
        data, st = ctypes.c_uint8(), ctypes.c_uint8()
        nm = LIB.vdrive_mailbox(self.v, c, arg, ctypes.byref(data), ctypes.byref(st))
        return nm, data.value, st.value

    # --- KERNAL-shaped helpers (what OPEN/SAVE/LOAD/INPUT# do on the bus) ---
    def listen(self, dev, sa, payload=b""):
        st = self.cmd(VD_LISTEN, dev)[2]
        st |= self.cmd(VD_SECOND, sa)[2]
        for b in payload: st |= self.cmd(VD_CIOUT, b)[2]
        st |= self.cmd(VD_UNLSN)[2]
        return st

    def talk(self, dev, sa, n=None):
        """read until EOI (or n bytes); returns (bytes, st)"""
        st = self.cmd(VD_TALK, dev)[2]
        st |= self.cmd(VD_TKSA, sa)[2]
        out, last = b"", 0
        while n is None or len(out) < n:
            nm, d, s = self.cmd(VD_ACPTR)
            last = s
            if s & 0x02: break
            out += bytes([d])
            if s & 0x40: break
        self.cmd(VD_UNTLK)
        return out, last

    def open(self, dev, ch, name): return self.listen(dev, 0xF0 | ch, name)
    def close(self, dev, ch): return self.listen(dev, 0xE0 | ch)
    def status(self, dev=8): return self.talk(dev, 0x6F)[0].decode("latin1")
    def load(self, dev, name):
        st = self.listen(dev, 0xF0, name)
        data, _ = self.talk(dev, 0x60)
        self.listen(dev, 0xE0)
        return data
    def save(self, dev, name, data):
        self.listen(dev, 0xF1, name)
        st = self.listen(dev, 0x61, data)
        self.listen(dev, 0xE1)
        return st


# ---------------------------------------------------------------------------
# register-level view (vio_t): exactly what the 6502 / IO1+IO2 handlers see
LIB.vio_sizeof.restype = ctypes.c_size_t
LIB.vio_hot_sizeof.restype = ctypes.c_size_t
LIB.vio_io1_read.restype = ctypes.c_uint8
LIB.vio_io2_read.restype = ctypes.c_uint8
LIB.vio_poll.restype = ctypes.c_int
LIB.vdrive_active.restype = ctypes.c_int
(R_CMD, R_ARG, R_DATA, R_STAT, R_ST, R_LEN, R_DEV0, R_DEV1) = range(8)
(F_LOAD_OPEN, F_LOAD_BLK, F_SAVE_OPEN, F_SAVE_BLK, F_SAVE_CLOSE, F_SCAN, F_FREE) = range(9, 16)
BUSY, OK, NOTMINE, SCANREQ = 0x80, 0x5A, 0xA5, 0x3C
WIN_SIZE = 128                                    # VIO_WIN: the IO2 window at $DF80 (d64/vdrive_io.h)


class VIO:
    def __init__(self):
        self.v = ctypes.create_string_buffer(LIB.vio_sizeof())
        self.hot = ctypes.create_string_buffer(LIB.vio_hot_sizeof())     # what the bus ISR touches
        LIB.vio_init(self.v, self.hot)
        self.keep = []

    def r(self, off): return LIB.vio_io1_read(self.hot, ctypes.c_uint8(off))
    def w(self, off, val): LIB.vio_io1_write(self.hot, ctypes.c_uint8(off), ctypes.c_uint8(val))
    def win_r(self, off): return LIB.vio_io2_read(self.hot, ctypes.c_uint8(off))
    def win_w(self, off, val): LIB.vio_io2_write(self.hot, ctypes.c_uint8(off), ctypes.c_uint8(val))
    def win_write(self, data):
        for i, b in enumerate(data): self.win_w(i, b)
    def win_read(self, n): return bytes(self.win_r(i) for i in range(n))

    def attach(self, unit, disk, present=True):
        # vio_t starts with vdrive_t, so the same pointer works
        LIB.vdrive_attach(self.v, unit, disk.d if disk else None, int(present))
        self.keep.append(disk)

    def arm_scan(self): return LIB.vio_arm_scan(self.v)
    def vd_act(self): return LIB.vdrive_active(self.v)

    def set_devices(self, d0, d1):
        LIB.vdrive_set_devices(self.v, ctypes.c_uint8(d0), ctypes.c_uint8(d1))
        LIB.vio_publish_devices(self.v)

    def call(self, cmd, arg=0, length=0, allow_scan=False):
        """write regs like the 6502, poll like the main loop; returns (notmine, data, st)"""
        self.w(R_ARG, arg); self.w(R_LEN, length); self.w(R_CMD, cmd)
        assert self.r(R_STAT) == BUSY, "BUSY must be visible immediately after the CMD write"
        assert LIB.vio_poll(self.v) == 1
        stat = self.r(R_STAT)
        self.stat = stat
        assert stat in (OK, NOTMINE) or (stat == SCANREQ and allow_scan), hex(stat)
        return stat == NOTMINE, self.r(R_DATA), self.r(R_ST)

    def fast_load(self, dev, name):
        """what the patched LOAD does. returns (rc, load_addr, data)"""
        self.win_write(name)
        nm, rc, _ = self.call(F_LOAD_OPEN, dev, len(name))
        if nm: return "notmine", None, None
        if rc: return rc, None, None
        addr = self.win_r(0) | self.win_r(1) << 8
        data = b""
        while True:
            _, n, st = self.call(F_LOAD_BLK)
            data += self.win_read(n)
            if st & 0x40: break
        return 0, addr, data

    def fast_save(self, dev, name, addr, data, block=WIN_SIZE):
        self.win_write(name)
        nm, rc, _ = self.call(F_SAVE_OPEN, dev, len(name))
        if nm: return "notmine"
        if rc: return rc
        self.win_write(bytes([addr & 255, addr >> 8]))
        self.call(F_SAVE_BLK, 0, 2)
        for i in range(0, len(data), block):
            part = data[i:i + block]
            self.win_write(part)
            self.call(F_SAVE_BLK, 0, len(part))
        return self.call(F_SAVE_CLOSE)[1]
