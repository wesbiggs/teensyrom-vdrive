# make STOCK=/path/to/kernal-901227-03.bin   (your own dump; not stored here)
STOCK ?= /opt/homebrew/Cellar/vice/3.10/share/vice/C64/kernal-901227-03.bin

# util/ is also a directory: without .PHONY `make util` is always "up to date"
.PHONY: test test-agent test-d64 test-devcfg test-fw test-vdconfig test-vdcopy test-dmatest test-reset test-gate test-banner test-menuprobe test-variants test-hash shot testdisk util

# Hardware image: mailbox at IO1 ($DE10, above the SwiftLink ACIA at $DE00-$DE07)
build/kernal-tr.bin: $(wildcard patches/*.s) tools/build.py
	python3 -I tools/build.py $(STOCK) $@

# the README publishes kernal-tr.bin's SHA-256 for the 901227-03 stock ROM (skipped with any other ROM)
test-hash: build/kernal-tr.bin README.md
	python3 -I tools/check_kernal_hash.py $(STOCK) build/kernal-tr.bin

# VICE mock image: mailbox in RAM at $C800 (VICE has no IO1 device to hook)
build/kernal-vd.bin: $(wildcard patches/*.s) tools/build.py
	python3 -I tools/build.py $(STOCK) $@ -D 'MBX=$$C800' -D 'WIN=$$CA00'

# without the DMA LOAD request (VD_DMA_LOAD=0: window blocks only, the upstream TeensyROM build; docs/DESIGN.md 49). The VICE scan run uses it.
build/kernal-tr-nodma.bin: $(wildcard patches/*.s) tools/build.py
	python3 -I tools/build.py $(STOCK) $@ -D VD_DMA_LOAD=0
build/kernal-vd-nodma.bin: $(wildcard patches/*.s) tools/build.py
	python3 -I tools/build.py $(STOCK) $@ -D 'MBX=$$C800' -D 'WIN=$$CA00' -D VD_DMA_LOAD=0
# The agent (docs/DESIGN.md 53), VICE flavour: served from GeoRAM's page at $DE08, mailbox as above
build/vdagent-vice.prg: vector/vdagent.s vector/agent.cfg patches/mailbox.inc
	mkdir -p build
	ca65 -I patches -D 'MBX=$$C800' -D 'WIN=$$CA00' -o build/vdagent-vice.o vector/vdagent.s
	ld65 -C vector/agent.cfg -S 0xDE08 -o $@ build/vdagent-vice.o

# ... and for x128 in 128 mode (docs/DESIGN.md 56): the same agent bytes, the mailbox below $4000 (RAM in the system bank)
build/vdagent128-vice.prg: vector/vdagent.s vector/agent.cfg patches/mailbox.inc
	mkdir -p build
	ca65 -I patches -D 'MBX=$$3F00' -D 'WIN=$$3F80' -o build/vdagent128-vice.o vector/vdagent.s
	ld65 -C vector/agent.cfg -S 0xDE08 -o $@ build/vdagent128-vice.o

test-agent: build/vdagent-vice.prg build/vdagent128-vice.prg build/libd64.dylib   # the agent + d64/vdrive_vec.c in VICE: the stock C64 KERNAL (mock `agent`, `agentscan`) and a C128 in 128 mode (`agent128`, `agent128scan`; skipped without x128)
	python3 -I tools/vdrive_mock.py $(STOCK) agent build/vdagent-vice.prg
	python3 -I tools/vdrive_mock.py $(STOCK) agentscan build/vdagent-vice.prg
	if command -v x128 >/dev/null; then python3 -I tools/vdrive_mock.py - agent128 build/vdagent128-vice.prg && \
	   python3 -I tools/vdrive_mock.py - agent128scan build/vdagent128-vice.prg; else echo "x128 not found: agent128 skipped"; fi

D64SRC = d64/d64.c d64/vdrive.c d64/vdrive_io.c d64/vdrive_vec.c
build/libd64.dylib: $(D64SRC) $(wildcard d64/*.h)
	cc -std=c99 -Wall -Wextra -Werror -fPIC -shared -o $@ $(D64SRC)
	c++ -x c++ -std=gnu++17 -Wall -Wextra -Werror -fsyntax-only $(D64SRC)   # firmware compiles these as C++

test-d64: build/libd64.dylib
	python3 -I tests/test_d64.py
	python3 -I tests/test_d64_write.py
	python3 -I tests/test_d71_d81.py
	python3 -I tests/test_vdrive.py
	python3 -I tests/test_rel.py
	python3 -I tests/test_vio.py
	python3 -I tests/test_vvec.py

test-devcfg:
	cc -std=c99 -Wall -Wextra -Werror -DVD_DEV0=10 -DVD_DEV1=11 -o build/devcfg tests/devcfg.c d64/d64.c d64/vdrive.c && ./build/devcfg

test-fw:
	python3 -I tests/fw_host/run.py $(STOCK)   # with the stock ROM: the firmware's in-memory patch == kernal-tr.bin

test-vdconfig:   # VDrive Config.prg (util/vdconfig.s) in py65 with its KERNAL calls answered by a drive model (skipped without py65)
	python3 -I tests/test_vdconfig.py

test-vdcopy: build/libd64.dylib   # VDCOPY (util/vdcopy.s) in py65 against the real drive code on both units: whole-disk and file copies (skipped without py65)
	python3 -I tests/test_vdcopy.py

test-dmatest:   # DMATEST (tests/asm/dmatest.s, the TR+ DMA-write check) in py65, the host's writes played by the test (skipped without py65)
	python3 -I tests/test_dmatest.py

test-reset:   # the cold-start hook (SYS64738 back to the TR menu) in a 6502 simulator (skipped without py65)
	python3 -I tests/test_reset_hook.py

test-gate:   # the mailbox gate (no VDrive handler: no mailbox writes, the real bus at once) in a 6502 simulator (skipped without py65)
	python3 -I tests/test_vcall_gate.py

test-banner:   # the power-on banner's drive numbers in a 6502 simulator with a fake serial bus (skipped without py65)
	python3 -I tests/test_banner.py

test-menuprobe: build/libd64.dylib   # the TR menu's launch-time bus probe (firmware/vdrive_probe.asm) in VICE, 1541s at 8 and 10 (skipped without acme)
	python3 -I tests/menuprobe_vice.py

test-variants: build/libd64.dylib   # the patch on each ROM the firmware patches in memory (VD_KnownKernals), in VICE; ROMs found next to STOCK
	python3 -I tests/variants_vice.py $(STOCK)

test: build/kernal-vd.bin build/kernal-vd-nodma.bin build/kernal-tr-nodma.bin build/libd64.dylib test-d64 test-devcfg test-fw test-reset test-gate test-banner test-vdconfig test-vdcopy test-dmatest test-hash test-menuprobe test-variants test-agent
	python3 -I tools/vdrive_mock.py build/kernal-vd.bin
	python3 -I tools/vdrive_mock.py build/kernal-vd-nodma.bin scan

shot: build/kernal-tr.bin
	x64sc -warp -kernal $(CURDIR)/build/kernal-tr.bin -limitcycles 5000000 -exitscreenshot $(CURDIR)/build/banner.png

# Bring-up images: stock = control, banner = proves the TR+ serves the image
build/kernal-stock.bin:
	cp $(STOCK) $@

build/banner-only/banner.s: bringup/banner.s
	mkdir -p build/banner-only && cp $< $@

build/kernal-banner.bin: build/banner-only/banner.s tools/build.py
	python3 -I tools/build.py $(STOCK) $@ --patches build/banner-only

# Test disk for the real hardware: copy build/testdisk/vdtest.d64 to the SD card, then OPEN15,8,15,"CD:VDTEST.D64"
testdisk:
	python3 -I tools/make_testdisk.py

# VDCOPY, the file and whole-disk copy utility (machine code) between any two units (real drive, SD, USB): build/util/vdcopy.prg (copy it to the card; see the README)
util:
	python3 -I tools/make_util.py
