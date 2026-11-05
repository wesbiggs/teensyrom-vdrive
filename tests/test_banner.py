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
stock[0xA31:0xA34] = bytes([0x20, 0xEA, 0xFF]); stock[0x1D9:0x1DB] = bytes([0xA2, 0x01]); stock[0x5EE:0x5F3] = bytes([0xA2, 0x09, 0x78, 0x86, 0xC6])
stock[0x429:0x447] = bytes.fromhex("a973a0e4201eaba53738e52baaa538e52c20cdbda960a0e4201eab4c44a6")   # INITMS from $E429 (code only)
stock[0x49A:0x4AC] = b" 64K RAM SYSTEM  \0"                                       # the stock line's text (the banner reuses it)
with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t); (t / "stock.bin").write_bytes(stock)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), str(t / "stock.bin"), str(t / "k.bin")], check=True, capture_output=True)
    img = bytearray((t / "k.bin").read_bytes())

TAG = b"VDUNITS:"                          # what the firmware searches for (VD_BannerUnits, firmware/IOH_VDrive.c)
tag = img.find(TAG)
assert tag > 0 and img.find(TAG, tag + 1) < 0, "exactly one tag in the image"
assert img[tag + 8:tag + 10] == b"\x00\x00", "built with no units: the stock line"
assert img[0x42D] == 0x20 and img[0x43D] == 0x4C, "INITMS: JSR vd_head, JMP vd_banner"
assert img[0x49A] == 0, "the banner string at $E473 ends after the top line"
img[0x460:0x473] = b" BASIC BYTES FREE\r\x00"   # the stock string at $E460 the no-firmware case prints (not in the synthetic image)
ST = 0x90


def run(units, bus=(), regs=(0xDE,) * 4):
    """units: the two bytes the firmware writes; bus: device numbers a real drive answers at; regs: what $DE3C-$DE3F read (the TR menu's
    IO1 offsets 60-63, where the firmware puts the same bytes for a VDrive KERNAL the C64 holds itself; floating $DE by default)
    -> (text, probes, bus log)"""
    m = MPU()
    m.memory[0xE000:0xE000 + 8192] = list(img)
    m.memory[0xE000 + tag + 8] = units[0]; m.memory[0xE000 + tag + 9] = units[1]
    m.memory[0xDE3C:0xDE40] = list(regs)
    m.memory[0x2B:0x2D] = [0x01, 0x08]; m.memory[0x37:0x39] = [0x00, 0xA0]   # BASIC from $0801 to $A000: 38911 bytes free
    m.pc, m.sp = 0xE429, 0xF0                       # INITMS after its memory check: banner string, free bytes, the rest
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
        elif pc == 0xBDCD: out.append(str(m.a << 8 | m.x)); rts()   # LINPRT: no leading space
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
            assert pc >= 0xE000 + 0x1000 or 0xE429 <= pc < 0xE440, "ran into stock code at $%04X" % pc
            m.step()
    else:
        raise AssertionError("no end")
    assert m.memory[ST] == 0, "ST left at %02X" % m.memory[ST]
    text = "".join(out)
    assert text.endswith("\r"), repr(text)
    return text[:-1], [int(e.split()[1]) for e in log if e.startswith("LISTEN")], log


def check(units, bus, field, probes=None, regs=(0xDE,) * 4):
    """field: the 10-character drive field, or None for the stock line"""
    got, pr, log = run(units, bus, regs)
    want = " 64K RAM SYSTEM  38911 BASIC BYTES FREE" if field is None else " 64K RAM  38911 FREE  VDRIVE " + field
    assert got == want, (units, bus, got)
    assert len(got) == 39, got                          # like the stock line: one leading space, 38 characters
    if probes is not None: assert pr == probes, (units, bus, pr)
    for i, e in enumerate(log):                         # every probe is LISTEN n, SECOND $6F (unless LISTEN failed), and UNLSN when found
        if e.startswith("LISTEN") and bus:
            assert log[i + 1] == "SECOND 6F", log
            n = int(e.split()[1])
            assert (log[i + 2:i + 3] == ["UNLSN"]) == (n in bus), log


check((0, 0), (), None, [])                                        # image as built / no VDrive firmware: the stock line, no bus traffic
check((0xFF, 0xFF), (), "SD 8 USB 9", [8, 9])                      # empty bus
check((0xFF, 0xFF), (8, 10), "SD 9 USB11", [8, 9, 10, 11])         # real drives at 8 and 10 (the VICE `scan` set-up)
check((0xFF, 0xFE), (8,), "SD 9 NOUSB", [8, 9])                    # no USB stick
check((0xFE, 0xFF), (), "NOSD USB 8", [8])                         # no SD card
check((0xFE, 0xFE), (8,), "NOSD NOUSB", [])                        # neither: no probe
check((0xFE, 12), (), "NOSD USB12", [])
check((9, 0xFF), (), "SD 9 USB 8", [8])                            # SD fixed at 9: USB takes 8; 9 is never probed
check((8, 0xFF), (9,), "SD 8 USB10", [9, 10])                      # 8 fixed (skipped, not probed), 9 a real drive
check((0xFF, 8), (), "SD 9 USB 8", [9])                            # USB fixed at 8: SD skips it
check((10, 11), (8,), "SD10 USB11", [])                            # both fixed: no probe at all
check((30, 0xFE), (), "SD30 NOUSB", [])
check((0xFF, 0xFF), tuple(range(8, 31)), "NOSD NOUSB", list(range(8, 31)))   # every number taken: stops at 30
check((0xFF, 0xFF), tuple(range(8, 30)), "SD30 NOUSB", list(range(8, 31)))
check((0xFF, 0xFF), tuple(range(8, 19)) + (20,), "SD19 USB21", list(range(8, 22)))

# a VDrive KERNAL in the machine itself (image bytes 0 0): the copy in the TR menu's IO1 registers, when the tag $D6 $64 reads right
R = lambda sd, usb: (0xD6, 0x64, sd, usb)
check((0, 0), (), "SD 8 USB 9", [8, 9], regs=R(0xFF, 0xFF))           # automatic, not settled: probed here as before
check((0, 0), (8, 10), "SD 9 USB11", [8, 9, 10, 11], regs=R(0xFF, 0xFF))
check((0, 0), (8,), "SD 9 USB11", [], regs=R(9, 11))                  # settled by the menu's probe: printed, no bus traffic
check((0, 0), (), "NOSD USB12", [], regs=R(0xFE, 12))
check((0, 0), (), "NOSD NOUSB", [], regs=R(0xFE, 0xFE))
check((0, 0), (), None, [], regs=(0, 0, 0, 0))                        # cleared by a launch with another Special IO
check((0, 0), (), None, [], regs=(0xD6, 0x65, 8, 9))                  # a wrong tag byte: the stock line
check((0, 0), (), None, [], regs=(0xD5, 0x64, 8, 9))
check((10, 11), (8,), "SD10 USB11", [], regs=R(0xFE, 0xFE))           # the served image's bytes come first

print("PASS vd_banner: menu registers for a resident KERNAL (tag, settled / automatic / absent, cleared, wrong tag, image first)")
print("PASS vd_banner: stock line without firmware / automatic / fixed / absent / full bus / two digits; always 38 wide; probes are LISTEN + SECOND $6F (+ UNLSN)")
