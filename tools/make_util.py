#!/usr/bin/env python3
"""Build build/util/vdcopy.prg: util/vdcopy.bas with the machine-code helper (util/vdcopy.s) as DATA lines.

usage: make_util.py [OUTDIR]       (needs ca65/ld65, petcat)
build(outdir) returns the path of the PRG (also used by make_testdisk.py and vdrive_mock.py).
"""
import pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent.parent
LOAD, STUB = 0x033C, 0x03ED               # cassette buffer; the STOP stub sits at $03ED (low byte of the STOP vector), results at $03F0

def build(out):
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    obj, cfg, binf = out / "vdcopy.o", out / "vdcopy.cfg", out / "vdcopy.bin"
    cfg.write_text(("MEMORY { M: start=$%04X, size=$%X, file=%%O, fill=yes; S: start=$%04X, size=3, file=%%O; }\n"
                   "SEGMENTS { CODE: load=M, type=ro; STUB: load=S, type=ro; }\n") % (LOAD, STUB - LOAD, STUB))
    subprocess.run(["ca65", "-o", str(obj), str(root / "util" / "vdcopy.s")], check=True)
    subprocess.run(["ld65", "-C", str(cfg), "-o", str(binf), str(obj)], check=True)
    code = binf.read_bytes()
    data = "\n".join("%d data %s" % (10010 + 10 * (i // 16), ",".join(map(str, code[i:i + 16]))) for i in range(0, len(code), 16))
    text = (root / "util" / "vdcopy.bas").read_text().replace("@MCLEN@", str(len(code))).replace("@MCDATA@", data)
    (out / "vdcopy.bas").write_text(text)
    prg = out / "vdcopy.prg"
    subprocess.run(["petcat", "-w2", "-o", str(prg), "--", str(out / "vdcopy.bas")], check=True, capture_output=True)
    return prg

if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "util"
    prg = build(out)
    print("wrote", prg, "(%d bytes)" % prg.stat().st_size)
