#!/usr/bin/env python3
"""The power-on banner's drive numbers (vd_banner, patches/vdbanner.s) run in a 6502 simulator (py65; skipped when it is not installed).

The KERNAL is assembled onto a synthetic 8 KB image that only has the bytes tools/build.py checks, so no Commodore ROM is needed.
The stock routines the banner calls are trapped by address: BASIC's STROUT ($AB1E) and CHROUT ($FFD2) collect the text, and the
serial bus is faked where the stock IEC code would drive it ($ED11 sends the LISTEN / UNLISTEN byte under ATN, $ED36 the secondary
address; a device that is not on the bus sets ST bit 7 there, and an empty bus already fails the LISTEN, as on a real C64)."""
import pathlib, subprocess, sys, tempfile

try:
    from py65.devices.mpu6502 import MPU
except ImportError:
    print("SKIP py65 not installed (pip install py65): vd_banner not simulated"); sys.exit(0)

ROOT = pathlib.Path(__file__).resolve().parent.parent
stock = bytearray(8192)
stock[0xA31:0xA34] = bytes([0x20, 0xEA, 0xFF]); stock[0x1D9:0x1DB] = bytes([0xA2, 0x01])
stock[0x429:0x447] = bytes.fromhex("a973a0e4201eaba53738e52baaa538e52c20cdbda960a0e4201eab4c44a6")   # INITMS from $E429 (code only)
with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t); (t / "stock.bin").write_bytes(stock)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), str(t / "stock.bin"), str(t / "k.bin")], check=True, capture_output=True)
    img = bytearray((t / "k.bin").read_bytes())

TAG = b", VDRIVE \x00"                      # what the firmware searches for (VD_BannerUnits, firmware/IOH_VDrive.c)
tag = img.find(TAG)
assert tag > 0 and img.find(TAG, tag + 1) < 0, "exactly one tag in the image"
assert img[tag + 10:tag + 12] == b"\x00\x00", "built with no units: no VDrive text"
assert img[0x444] == 0x4C, "INITMS ends in a JMP to the banner code"
assert img[0x460:0x466] == b" FREE\x00" and img[0x49A:0x4A5] == b" 64K RAM, \x00"
ST = 0x90


def run(units, bus=()):
    """units: the two bytes the firmware writes; bus: device numbers a real drive answers at. -> (text, probes, bus log)"""
    m = MPU()
    m.memory[0xE000:0xE000 + 8192] = list(img)
    m.memory[0xE000 + tag + 10] = units[0]; m.memory[0xE000 + tag + 11] = units[1]
    m.pc, m.sp = m.memory[0xE445] | m.memory[0xE446] << 8, 0xF0
    out, log, cur = [], [], [None]

    def rts():
        lo = m.memory[0x101 + m.sp]; hi = m.memory[0x102 + m.sp]; m.sp += 2
        m.pc = (lo | hi << 8) + 1

    for _ in range(200000):
        pc = m.pc
        if pc == 0xA644: break                          # NEW: the stock code goes on from here
        if pc == 0xAB1E:                                # STROUT A/Y
            a = m.a | m.y << 8
            while m.memory[a]: out.append(chr(m.memory[a])); a += 1
            m.x = m.y = 0x55; rts()                     # it clobbers X and Y
        elif pc == 0xFFD2: out.append(chr(m.a)); rts()
        elif pc == 0xF0A4: rts()                        # RS-232 NMI check before LISTEN
        elif pc == 0xED11:                              # byte under ATN: LISTEN n, or UNLISTEN $3F
            if m.a == 0x3F: log.append("UNLSN")
            else:
                assert m.a & 0xE0 == 0x20, hex(m.a)
                cur[0] = m.a & 0x1F; log.append("LISTEN %d" % cur[0])
                if not bus: m.memory[ST] |= 0x80
            rts()
        elif pc == 0xED36:                              # SECOND ($95)
            log.append("SECOND %02X" % m.memory[0x95])
            if cur[0] not in bus: m.memory[ST] |= 0x80
            rts()
        elif pc in (0xEDBE, 0xEE03): rts()              # release ATN, end of UNLSN
        else:
            assert pc >= 0xE000 + 0x1000 or pc in range(0xE444, 0xE447), "ran into stock code at $%04X" % pc
            m.step()
    else:
        raise AssertionError("no end")
    assert m.memory[ST] == 0, "ST left at %02X" % m.memory[ST]
    text = "".join(out)
    assert text.endswith("\r"), repr(text)
    return text[:-1], [int(e.split()[1]) for e in log if e.startswith("LISTEN")], log


def check(units, bus, text, probes=None):
    got, pr, log = run(units, bus)
    assert got == text, (units, bus, got)
    if probes is not None: assert pr == probes, (units, bus, pr)
    for i, e in enumerate(log):                         # every probe is LISTEN n, SECOND $6F (unless LISTEN failed), and UNLSN when found
        if e.startswith("LISTEN") and bus:
            assert log[i + 1] == "SECOND 6F", log
            n = int(e.split()[1])
            assert (log[i + 2:i + 3] == ["UNLSN"]) == (n in bus), log
    return got


check((0, 0), (), "", [])                                          # image as built / no VDrive firmware: stock-like line, no bus traffic
check((0xFF, 0xFF), (), ", VDRIVE 8+9", [8, 9])                    # empty bus
check((0xFF, 0xFF), (8, 10), ", VDRIVE 9+11", [8, 9, 10, 11])      # real drives at 8 and 10 (the VICE `scan` set-up)
check((0xFF, 0), (8,), ", VDRIVE 9", [8, 9])                       # no USB stick
check((0, 0xFF), (), ", VDRIVE 8", [8])                            # no SD card
check((9, 0xFF), (), ", VDRIVE 9+8", [8])                          # SD fixed at 9: USB takes 8; 9 is never probed
check((8, 0xFF), (9,), ", VDRIVE 8+10", [9, 10])                   # 8 fixed (skipped, not probed), 9 a real drive
check((0xFF, 8), (), ", VDRIVE 9+8", [9])                          # USB fixed at 8: SD skips it
check((12, 13), (8,), ", VDRIVE 12+13", [])                        # both fixed: no probe at all
check((30, 0), (), ", VDRIVE 30", [])
check((0xFF, 0xFF), tuple(range(8, 31)), "", list(range(8, 31)))   # every number taken: no text, stops at 30
check((0xFF, 0xFF), tuple(range(8, 30)), ", VDRIVE 30", list(range(8, 31)))
check((0xFF, 0xFF), tuple(range(8, 19)) + (20,), ", VDRIVE 19+21", list(range(8, 22)))
print("PASS vd_banner: none / automatic / fixed / mixed / full bus / two digits; probes are LISTEN + SECOND $6F (+ UNLSN)")
