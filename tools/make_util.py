#!/usr/bin/env python3
"""Build build/util/vdcopy.prg from util/vdcopy.s (all machine code, loads at $0801 with a BASIC SYS stub; its tables are BSS at $2000).

usage: make_util.py [OUTDIR]       (needs ca65/ld65)
build(outdir) returns the path of the PRG (also used by make_testdisk.py and vdrive_mock.py).
"""
import pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent.parent

def build(out):
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    obj, cfg, prg = out / "vdcopy.o", out / "vdcopy.cfg", out / "vdcopy.prg"
    cfg.write_text("MEMORY { LA: start=$07FF, size=2, file=%O; M: start=$0801, size=$17FF, file=%O;\n"
                   "         B: start=$2000, size=$1800, file=\"\"; }\n"
                   "SEGMENTS { LOADADDR: load=LA, type=ro; BASIC: load=M, type=ro; CODE: load=M, type=ro; BSS: load=B, type=bss; }\n")
    subprocess.run(["ca65", "-o", str(obj), str(root / "util" / "vdcopy.s")], check=True)
    subprocess.run(["ld65", "-C", str(cfg), "-o", str(prg), str(obj)], check=True)
    return prg

if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "util"
    prg = build(out)
    print("wrote", prg, "(%d bytes)" % prg.stat().st_size)
