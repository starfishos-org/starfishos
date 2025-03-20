IP = '0x0'
P = 'libc.so'

.PHONY: build build-all

b: build
build:
	./chbuild build

ba: build-all
build-all:
	./quick-build.sh

r: run
run:
	./dsm-scripts/config_memdev.sh cxl
	./build/simulate.sh

ra: run-all
run-all:
	./dsm-scripts/simulate_4clusters.sh

c: ip2c
ip2c:
	addr2line -e user/build/ramdisk/$(P) -fCi $(IP)

ck: ip2c-kernel
ip2c-kernel:
	addr2line -e build/kernel.img -fCi $(IP)

test:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/tests/lkl.exp

cxl-new:
	./dsm-scripts/config_memdev.sh cxl-new
