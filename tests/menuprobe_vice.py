#!/usr/bin/env python3
"""The TR menu's launch-time bus probe (firmware/vdrive_probe.asm, inserted into MainMenu.asm by tools/make_tr_patch.sh) in VICE,
with true-drive 1541s at 8 and 10 and the C64's own KERNAL, as in the menu.

The routine is assembled with ACME (the menu's assembler) into a small harness at $C000 that does what XferCopyRun does around it:
clear rwRegScratch, PreStart, wait, VDriveProbe. The TR's IO1 registers are RAM at $C900 here; a store checkpoint on the control
register stops the CPU and this script plays the Teensy: KERNALPreStart() with VDrive_ProbeStep() (firmware/IOH_VDrive.c) over
the real drive model's vio_probe_begin / vio_probe_free. The C side is tested in tests/fw_host and tests/test_vio.py.
usage: menuprobe_vice.py        (needs acme, x64sc with its ROMs)
"""
import pathlib, queue, shutil, struct, subprocess, sys, tempfile, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
if not shutil.which("acme"):
    print("SKIP acme not installed: the menu's bus probe not run in VICE"); sys.exit(0)
from vdrive_mock import Mon
from d64py import VIO, R_DEV0, R_DEV1

IO1 = 0xC900
SCRATCH, CONTROL, PRESTART = IO1 + 41, IO1 + 4, 24
DONE = 0xC9FF

HARNESS = """
   IO1Port = $c900
   rwRegScratch = 41
   wRegControl = 4
   rCtlKERNALPreStartWAIT = 24
* = $c000
   lda #0
   sta rwRegScratch+IO1Port
   lda #rCtlKERNALPreStartWAIT
   sta wRegControl+IO1Port
   jsr WaitForTRDots
   jsr VDriveProbe
   lda #$42
   sta $c9ff
   rts
WaitForTRDots:  ;the Teensy (this script) answers while the CPU is stopped
   rts
!src "%s"
"""


def main():
    work = pathlib.Path(tempfile.mkdtemp())
    (work / "h.asm").write_text(HARNESS % (ROOT / "firmware" / "vdrive_probe.asm"))
    subprocess.run(["acme", "-f", "plain", "-o", str(work / "h.bin"), str(work / "h.asm")], check=True)
    code = (work / "h.bin").read_bytes()
    (work / "vicerc").write_text("")
    vice = subprocess.Popen(["x64sc", "-config", str(work / "vicerc"), "-default", "-warp",
                             "-drive8type", "1541", "-drive8truedrive", "-drive9type", "0", "-drive10type", "1541", "-drive10truedrive",
                             "-drive11type", "0", "-binarymonitor", "-binarymonitoraddress", "ip4://127.0.0.1:6502"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    def check(label, cond):
        nonlocal ok
        print(("PASS" if cond else "FAIL"), label); ok = ok and bool(cond)
    try:
        m = Mon()
        m.req(0x12, struct.pack("<HHBBBB", CONTROL, CONTROL, 1, 1, 2, 0))       # store checkpoint on the control register
        t0 = time.time()
        while time.time() - t0 < 20 and not any("READY." in r for r in m.screen()):
            try:
                typ, _ = m.ev.get(timeout=0.5)
                if typ == 0x62: m.go()
            except queue.Empty: pass
        m.write(0xC000, code)

        def run(teensy):
            """the harness once; teensy(scratch) -> new scratch, at each PreStart. Returns the values the menu offered."""
            offered = []
            m.write(DONE, [0]); m.write(CONTROL, [0]); m.write(SCRATCH, [0x77])  # stale value: the harness must clear it
            while not m.ev.empty(): m.ev.get()
            m.go()                                                               # every stop needs a go, the monitor's own included
            m.keys("SYS49152\r")
            t = time.time()
            while time.time() - t < 30:
                try: typ, _ = m.ev.get(timeout=0.5)
                except queue.Empty:
                    if m.read(DONE, 1)[0] == 0x42: break
                    continue
                if typ != 0x62: continue
                if m.read(CONTROL, 1)[0] == PRESTART:
                    s = m.read(SCRATCH, 1)[0]
                    offered.append(s)
                    m.write(SCRATCH, [teensy(s)]); m.write(CONTROL, [0])        # (a memory read while running stops the CPU too)
                m.go()
            check("harness finished", m.read(DONE, 1)[0] == 0x42)
            return offered

        def vdrive(io):
            """KERNALPreStart() as patched: VDrive_ProbeStep(), else VDrive_PreStart() (which begins the probe)"""
            st = {"probing": False}
            def f(s):
                if st["probing"] and s:
                    left = 0 if s == 0xFF else io.probe_free(s)
                    st["probing"] = left != 0
                    return left
                io.arm_scan()
                need = io.probe_begin()
                st["probing"] = need != 0
                return need
            return f
        def devs(io): return io.r(R_DEV0), io.r(R_DEV1)
        def units(pins=(0, 0)):
            io = VIO(); io.attach(0, None); io.attach(1, None)                 # card and stick present
            io.set_devices(*pins); return io

        io = units()
        offered = run(vdrive(io))
        check("both automatic: the menu offered 9 and 11 (8 and 10 are real 1541s): %r" % offered, offered == [0, 9, 11])
        check("... SD unit 9, USB unit 11: %r" % (devs(io),), devs(io) == (9, 11))

        io = units((9, 0))
        offered = run(vdrive(io))
        check("SD fixed at 9: 9 is offered (nobody answers there yet), refused, USB gets 11: %r" % offered, offered == [0, 9, 11])
        check("... %r" % (devs(io),), devs(io) == (9, 11))

        io = units((12, 13))
        offered = run(vdrive(io))
        check("both fixed: PreStart only, no probe: %r" % offered, offered == [0] and devs(io) == (12, 13))

        offered = run(lambda s: 0)
        check("a TeensyROM without VDrive (scratch stays 0): no probe: %r" % offered, offered == [0])

        offered = run(lambda s: 0 if s == 0xFF else 1)                          # never satisfied: every number up to 30, then the end marker
        want = [0, 9] + list(range(11, 31)) + [0xFF]
        check("drives that never get a number: every free number to 30, then $ff", offered == want)
        return 0 if ok else 1
    finally:
        vice.terminate()
        shutil.rmtree(work, ignore_errors=True)

if __name__ == "__main__":
    sys.exit(main())
