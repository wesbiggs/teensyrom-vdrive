# make STOCK=/path/to/kernal-901227-03.bin   (your own dump; not stored here)
STOCK ?= /opt/homebrew/Cellar/vice/3.10/share/vice/C64/kernal-901227-03.bin

# util/ is also a directory: without .PHONY `make util` is always "up to date"
.PHONY: test test-d64 test-devcfg test-fw test-vdconfig test-border test-banner test-menuprobe test-hash shot testdisk util

# Hardware image: mailbox at IO1 ($DE10, above the SwiftLink ACIA at $DE00-$DE07)
build/kernal-tr.bin: $(wildcard patches/*.s) tools/build.py
	python3 -I tools/build.py $(STOCK) $@

# the README publishes kernal-tr.bin's SHA-256 for the 901227-03 stock ROM (skipped with any other ROM)
test-hash: build/kernal-tr.bin README.md
	python3 -I tools/check_kernal_hash.py $(STOCK) build/kernal-tr.bin

# VICE mock image: mailbox in RAM at $C800 (VICE has no IO1 device to hook)
build/kernal-vd.bin: $(wildcard patches/*.s) tools/build.py
	python3 -I tools/build.py $(STOCK) $@ -D 'MBX=$$C800' -D 'WIN=$$CA00'

D64SRC = d64/d64.c d64/vdrive.c d64/vdrive_io.c
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

test-devcfg:
	cc -std=c99 -Wall -Wextra -Werror -DVD_DEV0=10 -DVD_DEV1=11 -o build/devcfg tests/devcfg.c d64/d64.c d64/vdrive.c && ./build/devcfg

test-fw:
	python3 -I tests/fw_host/run.py

test-vdconfig:   # VDrive Config.prg (util/vdconfig.s) in py65 with its KERNAL calls answered by a drive model (skipped without py65)
	python3 -I tests/test_vdconfig.py

test-border:   # the border-flash routine in a 6502 simulator (skipped without py65)
	python3 -I tests/test_irq_border.py

test-banner:   # the power-on banner's drive numbers in a 6502 simulator with a fake serial bus (skipped without py65)
	python3 -I tests/test_banner.py

test-menuprobe: build/libd64.dylib   # the TR menu's launch-time bus probe (firmware/vdrive_probe.asm) in VICE, 1541s at 8 and 10 (skipped without acme)
	python3 -I tests/menuprobe_vice.py

test: build/kernal-vd.bin build/libd64.dylib test-d64 test-devcfg test-fw test-border test-banner test-vdconfig test-hash test-menuprobe
	python3 -I tools/vdrive_mock.py build/kernal-vd.bin
	python3 -I tools/vdrive_mock.py build/kernal-vd.bin scan

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

# VDCOPY, the file copy utility (machine code) between any two units (real drive, SD, USB): build/util/vdcopy.prg (copy it to the card; see the README)
util:
	python3 -I tools/make_util.py
