#!/usr/bin/env python3
"""Compile firmware/IOH_VDrive.c against host stubs and run it; then check the images it wrote with c1541."""
import pathlib, shutil, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
tmp = pathlib.Path(tempfile.mkdtemp())
try:
    # build tree mirrors the firmware layout: IO_Handlers/IOH_VDrive.c + vdrive/*.c
    (tmp / "IO_Handlers").mkdir(); (tmp / "vdrive").mkdir()
    shutil.copy(ROOT / "firmware/IOH_VDrive.c", tmp / "IO_Handlers/glue.c")
    shutil.copy(ROOT / "firmware/IOH_VDriveSwift.c", tmp / "IO_Handlers/swift.c")
    for f in (ROOT / "d64").iterdir():
        if f.suffix in (".c", ".h"): shutil.copy(f, tmp / "vdrive" / f.name)
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

    sd, usb = tmp / "sdroot", tmp / "usbroot"
    (sd / "SUB").mkdir(parents=True); usb.mkdir()
    def mk(path, name, ident, files):
        subprocess.run(["c1541", "-format", f"{name},{ident}", "d64", str(path)], capture_output=True, check=True)
        for fname, data in files.items():
            p = tmp / ("src_" + fname); p.write_bytes(data)
            subprocess.run(["c1541", "-attach", str(path), "-write", str(p), fname.lower()], capture_output=True, check=True)
    hello = b"\x01\x08" + bytes(range(256)) * 5
    mk(sd / "GAME.D64", "gamedisk", "gd", {"HELLO": hello})
    mk(sd / "SUB" / "INNER.D64", "inner", "in", {})
    mk(usb / "USB.D64", "usbdisk", "us", {})
    (sd / ".HIDDEN").write_bytes(b"x")
    (sd / "AVERYLONGFILENAME.PRG").write_bytes(b"x")
    expect = tmp / "hello.bin"; expect.write_bytes(hello)

    r = subprocess.run([str(exe), str(sd), str(usb), str(expect)], capture_output=True, text=True)
    print(r.stdout, end="")
    ok = r.returncode == 0

    out = subprocess.run(["c1541", "-attach", str(sd / "GAME.D64"), "-list"], capture_output=True, text=True, encoding="latin1").stdout.lower()
    good = '"newfile"' in out
    print(("PASS" if good else "FAIL"), "c1541 sees NEWFILE written to the SD image by the glue")
    v = subprocess.run(["c1541", "-attach", str(sd / "GAME.D64"), "-read", "newfile", str(tmp / "nf")], capture_output=True)
    nf = (tmp / "nf").read_bytes() if (tmp / "nf").exists() else b""
    good2 = nf == b"\x01\x08" + b"Q" * 3000
    print(("PASS" if good2 else "FAIL"), "NEWFILE contents intact on the image file")
    sys.exit(0 if ok and good and good2 else 1)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
