"""VDSUITE (tests/basic/vdsuite.bas) and its card files: a plain PRG and a .d64 with an error table, plus the placeholders the suite needs.

Used by tools/make_testdisk.py (the test disk and the files to copy to the card root) and tools/vdrive_mock.py (the mock's card in VICE).
"""
import pathlib, subprocess, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SUITE_TOP = 0x5800                      # the PRG's end: VDSUITE lowers the top of BASIC to $6000, its variables and strings live in between

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


def strip_rems(text):
    """drop the text after each REM (outside quotes), keeping the REM itself (a line may be a GOTO target): room for the machine code"""
    out = []
    for line in text.splitlines():
        q = False
        for i in range(len(line) - 2):
            if line[i] == '"': q = not q
            elif not q and line[i:i + 3] == "rem":
                line = line[:i + 3]; break
        out.append(line)
    return "\n".join(out) + "\n"


def build_vdsuite(out, bad, mock=False):
    """VDSUITE.PRG: the BASIC program (petcat) with the machine-code checks of tests/asm/suite.s appended at the next page,
    linked for that address. mock: BUSTEST on the VICE mock's mailbox (RAM at $C800, window $CA00) instead of $DE10/$DF80.
    Returns the PRG's path in out."""
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    text, prg = out / "vdsuite.bas", out / "vdsuite.prg"
    text.write_text(strip_rems(subst((ROOT / "tests" / "basic" / "vdsuite.bas").read_text(), bad)))
    subprocess.run(["petcat", "-w2", "-o", str(prg), "--", str(text)], check=True, capture_output=True)
    basic = prg.read_bytes()
    assert basic[:2] == b"\x01\x08", "petcat: load address"
    at = (0x0801 + len(basic) - 2 + 0xFF) & 0xFF00
    obj, cfg, mc = out / "suite.o", out / "suite.cfg", out / "suite.bin"
    cfg.write_text("MEMORY { M: start=$%04X, size=$%04X, file=%%O; }\n"
                   "SEGMENTS { CODE: load=M, type=ro; RODATA: load=M, type=ro; JUMPS: load=M, type=ro; }\n" % (at, SUITE_TOP - at))
    defs = ["-D", "MBX=$C800", "-D", "WIN=$CA00"] if mock else []
    subprocess.run(["ca65", *defs, "-I", str(ROOT / "tests" / "asm"), "-o", str(obj), str(ROOT / "tests" / "asm" / "suite.s")], check=True)
    subprocess.run(["ld65", "-C", str(cfg), "-o", str(mc), str(obj)], check=True)   # fails when the code passes SUITE_TOP
    code = mc.read_bytes()
    assert code[-14:-12] == b"VD", "the jump table must end the code"
    prg.write_bytes(basic + bytes(at - 0x0801 - (len(basic) - 2)) + code)
    return prg
