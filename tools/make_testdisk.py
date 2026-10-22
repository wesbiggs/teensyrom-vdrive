#!/usr/bin/env python3
"""Build build/testdisk/vdtest.d64: hello + big + the BASIC test programs (tests/basic/*.bas).

usage: make_testdisk.py [OUTDIR]     (needs petcat and c1541 from VICE)
Copy the resulting vdtest.d64 to the root of the SD card (or USB stick) and mount it with CD:VDTEST.D64.
"""
import pathlib, random, subprocess, sys

root = pathlib.Path(__file__).resolve().parent.parent
out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / "build" / "testdisk"
out.mkdir(parents=True, exist_ok=True)

rnd = random.Random(2026)
big = bytes(rnd.randrange(256) for _ in range(6000))
xx = sum(big[i] for i in range(0, 6000, 8))
(out / "big.prg").write_bytes(b"\x00\x20" + big)                      # loads at $2000

hello_line = bytes([0x99, 0x22]) + b"HELLO FROM D64" + bytes([0x22, 0x00])
(out / "hello.prg").write_bytes(bytes([0x01, 0x08]) + (0x0801 + 4 + len(hello_line)).to_bytes(2, "little")
                                + (10).to_bytes(2, "little") + hello_line + bytes([0, 0]))

img = out / "vdtest.d64"
if img.exists(): img.unlink()
def c1541(*a): subprocess.run(["c1541", *a], check=True, capture_output=True)
c1541("-format", "vdtest,01", "d64", str(img))
c1541("-attach", str(img), "-write", str(out / "hello.prg"), "hello")
c1541("-attach", str(img), "-write", str(out / "big.prg"), "big")
for src in sorted((root / "tests" / "basic").glob("*.bas")):
    text = out / src.name
    text.write_text(src.read_text().replace("@XX@", str(xx)))
    prg = out / (src.stem + ".prg")
    subprocess.run(["petcat", "-w2", "-o", str(prg), "--", str(text)], check=True, capture_output=True)
    c1541("-attach", str(img), "-write", str(prg), src.stem)
sys.path.insert(0, str(root / "tools"))
from make_util import build as build_util, build_config
c1541("-attach", str(img), "-write", str(build_util(out / "util")), "vdcopy")
c1541("-attach", str(img), "-write", str(build_config(out / "util")), "vdconfig")
for src in sorted((root / "tests" / "asm").glob("*.s")):      # machine-code tests: PRG with a BASIC SYS stub at $0801
    obj, prg, cfg = out / (src.stem + ".o"), out / (src.stem + ".prg"), out / "asm.cfg"
    cfg.write_text("MEMORY { LA: start=$07FF, size=2; M: start=$0801, size=$3000, file=%O; }\n"
                   "SEGMENTS { LOADADDR: load=LA, type=ro; CODE: load=M, type=ro; RODATA: load=M, type=ro; }\n")
    subprocess.run(["ca65", "-o", str(obj), str(src)], check=True)
    subprocess.run(["ld65", "-C", str(cfg), "-o", str(prg), str(obj)], check=True)
    c1541("-attach", str(img), "-write", str(prg), src.stem)
print(subprocess.run(["c1541", "-attach", str(img), "-list"], capture_output=True, text=True, encoding="latin1").stdout.split("\n", 2)[2].rsplit("Unit", 1)[0])
print(f"BIG: 6000 bytes at $2000; full-sum {sum(big)}, stride-8 sum {xx} (the soak program checks the latter)")
print(f"wrote {img}")
