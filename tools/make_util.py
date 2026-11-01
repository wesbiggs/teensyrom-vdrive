#!/usr/bin/env python3
"""Build build/util/vdcopy.prg from util/vdcopy.s and "build/util/VDrive Config.prg" from util/vdconfig.s (both all machine code, loading at
$0801 with a BASIC SYS stub; their tables are BSS, not in the PRG: from $2800 for VDCOPY, $2000 for VDrive Config).

usage: make_util.py [OUTDIR]       (needs ca65/ld65)
build(outdir) returns the path of the VDCOPY PRG (also used by make_testdisk.py and vdrive_mock.py); build_config(outdir) the other one.
"""
import pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent.parent

def _asm(out, name, bss_size, prg_name=None, bss=0x2000, defs=()):
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    obj, cfg, prg = out / (name + ".o"), out / (name + ".cfg"), out / (prg_name or name + ".prg")
    cfg.write_text("MEMORY { LA: start=$07FF, size=2, file=%%O; M: start=$0801, size=$%04X, file=%%O;\n"
                   "         B: start=$%04X, size=$%04X, file=\"\"; }\n"
                   "SEGMENTS { LOADADDR: load=LA, type=ro; BASIC: load=M, type=ro; CODE: load=M, type=ro; BSS: load=B, type=bss; }\n" % (bss - 0x0801, bss, bss_size))
    subprocess.run(["ca65", "-g", *[a for d in defs for a in ("-D", d)], "-o", str(obj), str(root / "util" / (name + ".s"))], check=True)
    subprocess.run(["ld65", "-C", str(cfg), "-Ln", str(out / (name + ".lbl")), "-o", str(prg), str(obj)], check=True)
    return prg

def label(out, name, sym):
    """the address of label sym in the last build of util/<name>.s (from ld65's label file)"""
    for line in (pathlib.Path(out) / (name + ".lbl")).read_text().splitlines():
        f = line.split()
        if f[2] == "." + sym: return int(f[1], 16)
    raise KeyError(sym)

def build(out, mock=False):
    """mock: the device registers where the VICE mock's KERNAL keeps its mailbox (RAM at $C800, Makefile) instead of $DE16"""
    return _asm(out, "vdcopy", 0x1800, bss=0x2800, defs=["DEV0REG=$C806"] if mock else ())

def build_config(out):
    return _asm(out, "vdconfig", 0x3000, "VDrive Config.prg")

if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "util"
    prg = build(out)
    print("wrote", prg, "(%d bytes)" % prg.stat().st_size)
    cfg = build_config(out)
    print("wrote", cfg, "(%d bytes)" % cfg.stat().st_size)
