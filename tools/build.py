#!/usr/bin/env python3
"""Overlay ca65 patches onto a user-supplied stock KERNAL.

usage: build.py STOCK.bin OUT.bin [-D NAME=VALUE ...] [--patches DIR]

Patch sources use segments named P_XXXX (XXXX = hex load address). Each
segment is linked at that address and copied over the stock image.
Stock ROM bytes are never stored in this repo.
"""
import pathlib, re, subprocess, sys, tempfile

root = pathlib.Path(__file__).resolve().parent.parent
stock = pathlib.Path(sys.argv[1]).read_bytes()
assert len(stock) == 8192, "stock KERNAL must be 8192 bytes"
args = sys.argv[3:]
patch_dir = root / "patches"
if "--patches" in args:
    i = args.index("--patches"); patch_dir = pathlib.Path(args[i + 1]).resolve(); del args[i:i + 2]
defs = [a for a in args if a != "-D"]
img = bytearray(stock)

srcs = sorted(patch_dir.glob("*.s"))
segs = sorted({m for s in srcs for m in re.findall(r'\.segment\s+"P_([0-9A-Fa-f]{4})"', s.read_text())})

if "EA32" in segs:   # the border flash retargets the IRQ routine's `JSR $FFEA` (UDTIM): refuse to patch a ROM that does not have it
    assert stock[0xEA31 - 0xE000:0xEA34 - 0xE000] == bytes([0x20, 0xEA, 0xFF]), "no JSR $FFEA at $EA31: not a 901227-03 KERNAL?"

with tempfile.TemporaryDirectory() as t:
    t = pathlib.Path(t)
    objs = []
    for s in srcs:
        o = t / (s.stem + ".o")
        cmd = ["ca65", "-o", str(o)]
        for d in defs: cmd += ["-D", d]
        subprocess.run(cmd + [str(s)], check=True)
        objs.append(str(o))
    mem, sg = [], []
    for a in segs:
        mem.append(f'M{a}: start = ${a}, size = $0800, file = "{t}/{a}.bin";')
        sg.append(f"P_{a}: load = M{a}, type = ro;")
    cfg = t / "k.cfg"
    cfg.write_text("MEMORY {\n" + "\n".join(mem) + "\n}\nSEGMENTS {\n" + "\n".join(sg) + "\n}\n")
    subprocess.run(["ld65", "-C", str(cfg), "-o", str(t / "x.bin"), *objs], check=True)
    for a in segs:
        addr = int(a, 16)
        data = (t / f"{a}.bin").read_bytes()
        assert 0xE000 <= addr and addr + len(data) <= 0x10000, f"P_{a} out of range"
        img[addr - 0xE000 : addr - 0xE000 + len(data)] = data
        print(f"P_{a}: {len(data)} bytes")

pathlib.Path(sys.argv[2]).write_bytes(img)
