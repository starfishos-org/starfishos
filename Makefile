ADDR = '0x0'
PROC = 'libc.so'

build:
	./chbuild build

build-all:
	./quick-build.sh

run:
	./dsm-scripts/config_memdev.sh cxl
	./build/simulate.sh

run-all:
	./dsm-scripts/simulate_4clusters.sh

ip2c:
	addr2line -e user/build/ramdisk/$(PROC) -fCi $(ADDR)

ip2c-kernel:
	addr2line -e build/kernel.img -fCi $(ADDR)

