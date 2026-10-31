"""Card files for VDSUITE (tests/basic/vdsuite.bas): a plain PRG and a .d64 with an error table, plus the placeholders the suite needs.

Used by tools/make_testdisk.py (files to copy to the card root) and tools/vdrive_mock.py (the mock's card in VICE).
"""
import pathlib, subprocess, tempfile

BAD_FREE = (30, 5)                      # a free sector the error table marks 21 (READ ERROR, byte 3); 30/4 stays good


def plain_prg():
    """VDPLAIN.PRG: load address $C000, then 600 bytes (j*7+3) & 255."""
    return b"\x00\xc0" + bytes((j * 7 + 3) & 255 for j in range(600))


def bad_image(path):
    """VDBAD.D64: BADFILE (1000 bytes, load address $C000, data 1 + j % 250) whose second sector the error table marks 23
    (READ ERROR, byte 5), and the free sector BAD_FREE marked 21. Returns the bad file sector (track, sector)."""
    path = pathlib.Path(path)
    with tempfile.TemporaryDirectory() as tmp:
        prg = pathlib.Path(tmp) / "badfile.prg"
        prg.write_bytes(b"\x00\xc0" + bytes(1 + j % 250 for j in range(998)))
        if path.exists(): path.unlink()
        subprocess.run(["c1541", "-format", "vdbad,bd", "d64", str(path)], check=True, capture_output=True)
        subprocess.run(["c1541", "-attach", str(path), "-write", str(prg), "badfile"], check=True, capture_output=True)
    img = bytearray(path.read_bytes())
    def off(t, s): return (sum(21 if x <= 17 else 19 if x <= 24 else 18 if x <= 30 else 17 for x in range(1, t)) + s) * 256
    e = off(18, 1) + 2                                             # first directory entry: BADFILE
    t, s = img[e + 1], img[e + 2]
    t, s = img[off(t, s)], img[off(t, s) + 1]                      # its second sector
    errs = bytearray([1] * 683)
    errs[off(t, s) // 256] = 5
    errs[off(*BAD_FREE) // 256] = 3
    path.write_bytes(bytes(img) + bytes(errs))
    return t, s


def subst(text, bad):
    """the suite's placeholders: @BT@/@BS@ = the bad file sector"""
    return text.replace("@BT@", str(bad[0])).replace("@BS@", str(bad[1]))
