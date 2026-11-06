#!/usr/bin/env python3
"""Compile firmware/IOH_VDrive.c against host stubs and run it; then check the images it wrote with c1541.

usage: run.py [STOCK.bin]   with a stock KERNAL the patched image the glue builds from it (VD_KernalImage) is compared with
                            tools/build.py's; without one (or with a ROM the glue does not know) only the other KERNAL checks run"""
import pathlib, shutil, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
tmp = pathlib.Path(tempfile.mkdtemp())
try:
    # build tree mirrors the firmware layout: IO_Handlers/IOH_VDrive.c + vdrive/*.c
    (tmp / "IO_Handlers").mkdir(); (tmp / "vdrive").mkdir()
    shutil.copy(ROOT / "firmware/IOH_VDrive.c", tmp / "IO_Handlers/glue.c")
    shutil.copy(ROOT / "firmware/IOH_VDriveSwift.c", tmp / "IO_Handlers/swift.c")
    shutil.copy(ROOT / "firmware/IOH_VDriveREU.c", tmp / "IO_Handlers/vdreu.c")
    shutil.copy(ROOT / "firmware/IOH_VDriveSwiftREU.c", tmp / "IO_Handlers/swiftreu.c")
    shutil.copy(ROOT / "firmware/IOH_VDriveAgent.c", tmp / "IO_Handlers/agent.c")
    for f in (ROOT / "d64").iterdir():
        if f.suffix in (".c", ".h"): shutil.copy(f, tmp / "vdrive" / f.name)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), "--table", str(tmp / "vdrive/vd_kpatch.h")], check=True)
    subprocess.run([sys.executable, "-I", str(ROOT / "tools/build_agent.py"), str(tmp / "vdrive/vd_agent.h")], check=True)
    stock = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else None
    kstock, kpatched = tmp / "kstock.bin", tmp / "kpatched.bin"
    if stock and stock.exists():
        shutil.copy(stock, kstock)
        subprocess.run([sys.executable, "-I", str(ROOT / "tools/build.py"), str(stock), str(kpatched)], check=True, capture_output=True)
    shutil.copy(ROOT / "tests/fw_host/stub.h", tmp / "stub.h")
    main = (ROOT / "tests/fw_host/main.cpp").read_text().replace('#include "glue.c"', '#include "IO_Handlers/glue.c"')
    # the glue expects vdrive_io.h's VIO_/VD_ symbols; vdrive.h/d64.h arrive through the .c includes' headers
    (tmp / "main.cpp").write_text(main)
    exe = tmp / "fwhost"
    r = subprocess.run(["c++", "-std=gnu++17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror", "-I", str(tmp / "vdrive"),
                        "-o", str(exe), str(tmp / "main.cpp")], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr); sys.exit(1)
    print("PASS firmware glue compiles as C++ (-Wall -Wextra -Werror) against host stubs")
    for flag in ("-DVD_TRACE", "-DVD_NO_TRACE", "-DVD_ALLOC_HEAP"):
        r = subprocess.run(["c++", "-std=gnu++17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror", flag, "-I", str(tmp / "vdrive"),
                            "-fsyntax-only", str(tmp / "main.cpp")], capture_output=True, text=True)
        if r.returncode:
            print(r.stdout + r.stderr); sys.exit(1)
        print("PASS glue also compiles with", flag)
    # the plain TeensyROM: no Fab04_*, only the agent handler (tests/fw_host/plain.cpp)
    shutil.copy(ROOT / "tests/fw_host/plain.cpp", tmp / "plain.cpp")
    plain = tmp / "fwplain"
    r = subprocess.run(["c++", "-std=gnu++17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror", "-DPLAIN_TR", "-I", str(tmp / "vdrive"),
                        "-o", str(plain), str(tmp / "plain.cpp")], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr); sys.exit(1)
    print("PASS glue + agent compile for the plain TeensyROM (-DPLAIN_TR, no Fab04_*)")

    sd, usb = tmp / "sdroot", tmp / "usbroot"
    (sd / "SUB").mkdir(parents=True); usb.mkdir()
    def mk(path, name, ident, files, fmt="d64"):
        subprocess.run(["c1541", "-format", f"{name},{ident}", fmt, str(path)], capture_output=True, check=True)
        for fname, data in files.items():
            p = tmp / ("src_" + fname); p.write_bytes(data)
            subprocess.run(["c1541", "-attach", str(path), "-write", str(p), fname.lower()], capture_output=True, check=True)
    hello = b"\x01\x08" + bytes(range(256)) * 5
    mk(sd / "GAME.D64", "gamedisk", "gd", {"HELLO": hello})
    mk(sd / "SUB" / "INNER.D64", "inner", "in", {})
    mk(usb / "USB.D64", "usbdisk", "us", {})
    mk(sd / "BIG.D81", "bigdisk", "bd", {"H81": b"\x01\x08d81!"}, "d81")
    mk(sd / "TWO.D71", "twodisk", "td", {"H71": b"\x01\x08d71!"}, "d71")
    # error tables (683 bytes after a 35-track image): BAD.D64 marks 17/0, the first sector of BADF, as 23 (byte 5); OKTAB.D64 all OK
    mk(sd / "BAD.D64", "baddisk", "bd", {"BADF": b"\x01\x08" + b"B" * 600, "GOODF": b"\x01\x08good"})
    tab = bytearray([1] * 683); tab[16 * 21] = 5
    (sd / "BAD.D64").write_bytes((sd / "BAD.D64").read_bytes() + bytes(tab))
    mk(sd / "OKTAB.D64", "oktab", "ok", {})
    (sd / "OKTAB.D64").write_bytes((sd / "OKTAB.D64").read_bytes() + bytes([1] * 683))
    (sd / ".HIDDEN").write_bytes(b"x")
    (sd / "AVERYLONGFILENAME.PRG").write_bytes(b"x")
    lf = sd / "Long Folder With A Long Name"; lf.mkdir()                 # aliases (docs/DESIGN.md 44)
    mk(lf / "The-Pumpkins-Breadbin-of-Horror-side1.D64", "pumpkins1", "p1", {})
    mk(lf / "The-Pumpkins-Breadbin-of-Horror-side2.D64", "pumpkins2", "p2", {"SIDE2": b"\x01\x08side two"})
    (lf / "a,b.prg").write_bytes(b"\x01\x08comma")
    (lf / "Delete-Me-Long-Image-Name-x.d64").write_bytes(b"x")          # DELETE-ME-^X.D64, for S:
    (lf / "Collide-AAAAAA-x-side1.prg").write_bytes(b"\x01\x08A")      # both COLLID^SIDE1.PRG: dropped
    (lf / "Collide-BBBBBB-x-side1.prg").write_bytes(b"\x01\x08B")
    (lf / "Shortname-xyz-ONE.prg").write_bytes(b"\x01\x08alias")      # SHORTNAM^ONE.PRG, but a real file has that name
    (lf / "shortnam^one.prg").write_bytes(b"\x01\x08real")
    expect = tmp / "hello.bin"; expect.write_bytes(hello)

    r = subprocess.run([str(exe), str(sd), str(usb), str(expect), str(kstock), str(kpatched)], capture_output=True, text=True)
    print(r.stdout, end="")
    ok = r.returncode == 0
    r = subprocess.run([str(plain), str(sd)], capture_output=True, text=True)
    print(r.stdout, end="")
    ok = ok and r.returncode == 0

    out = subprocess.run(["c1541", "-attach", str(sd / "GAME.D64"), "-list"], capture_output=True, text=True, encoding="latin1").stdout.lower()
    good = '"newfile"' in out
    print(("PASS" if good else "FAIL"), "c1541 sees NEWFILE written to the SD image by the glue")
    v = subprocess.run(["c1541", "-attach", str(sd / "GAME.D64"), "-read", "newfile", str(tmp / "nf")], capture_output=True)
    nf = (tmp / "nf").read_bytes() if (tmp / "nf").exists() else b""
    good2 = nf == b"\x01\x08" + b"Q" * 3000
    print(("PASS" if good2 else "FAIL"), "NEWFILE contents intact on the image file")
    made = sd / "made.d81"
    out = subprocess.run(["c1541", "-attach", str(made), "-list"], capture_output=True, text=True, encoding="latin1").stdout.lower() if made.exists() else ""
    good3 = '"made' in out and "3160 blocks free" in out
    print(("PASS" if good3 else "FAIL"), "c1541 reads the D81 that N:MADE.D81 created")
    sys.exit(0 if ok and good and good2 and good3 else 1)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
