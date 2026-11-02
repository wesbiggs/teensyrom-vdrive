#!/usr/bin/env python3
"""The KERNAL patch on every ROM the firmware's whitelist accepts (VD_KnownKernals in firmware/IOH_VDrive.c), in VICE.

usage: variants_vice.py STOCK.bin   the other ROMs are looked up next to it: VICE ships them in its C64 and C128 data folders.

Each ROM must have the CRC-32 the whitelist names, gets patched with tools/build.py (VICE mock image: mailbox in RAM) and runs the
mock's `variant` scenario (banner, directory, SAVE / LOAD, SYS64738). A ROM that is not installed is skipped with a note."""
import pathlib, re, subprocess, sys, tempfile, zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
stock = pathlib.Path(sys.argv[1]).resolve()
vice = stock.parent.parent                       # .../share/vice (C64/, C128/)
roms = {"901227-03": "C64/kernal-901227-03.bin", "901227-02": "C64/kernal-901227-02.bin", "251104-04 (SX-64)": "C64/kernal-251104-04.bin",
        "325179-01 (C128 C64 mode)": "C128/kernal64-325179-01.bin", "325182-01 (C128DCR C64 mode)": "C128/kernal64-325182-01.bin"}

glue = (ROOT / "firmware/IOH_VDrive.c").read_text()
known = {name: int(crc, 16) for crc, name in re.findall(r'\{ 0x([0-9a-f]{8}), "([^"]+)" \}', glue)}
assert set(known) == set(roms), "VD_KnownKernals and this list must name the same ROMs: %r" % sorted(set(known) ^ set(roms))

ok = True
with tempfile.TemporaryDirectory() as t:
    for name, rel in roms.items():
        rom = vice / rel
        if not rom.exists():
            print("SKIP %s: %s not installed" % (name, rom)); continue
        data = rom.read_bytes()
        if zlib.crc32(data) != known[name]:
            print("FAIL %s: CRC-32 %08x, the whitelist says %08x" % (name, zlib.crc32(data), known[name])); ok = False; continue
        img = pathlib.Path(t) / "k.bin"
        subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), str(rom), str(img), "-D", "MBX=$C800", "-D", "WIN=$CA00"],
                       check=True, capture_output=True)
        r = subprocess.run([sys.executable, "-I", str(ROOT / "tools/vdrive_mock.py"), str(img), "variant"], capture_output=True, text=True)
        bad = [l for l in r.stdout.splitlines() if l.startswith("FAIL")]
        print("%s %s%s" % ("PASS" if r.returncode == 0 else "FAIL", name, "".join("\n     " + l for l in bad)))
        if r.returncode: ok = False; print(r.stdout[-3000:], r.stderr[-2000:])
sys.exit(0 if ok else 1)
