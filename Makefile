IP = '0x0'
P = 'libc.so'

help:
	@echo "make b: build the system without cleaning"
	@echo "make build-all / ba: build the whole system"
	@echo "make run / r: start the qemu"
	@echo "make clean / c: clean the system"
	@echo "make test: run tests under the directory ./dsm-scripts/tests"
	@echo "make prepare: prepare the system (only need to run once after the first clone)"

b: build-wo-clean
build-wo-clean:
	./chbuild build

ba: build-all
build-all:
	./quick-build.sh

r: run
run:
	./dsm-scripts/config_memdev.sh cxl
	./build/simulate.sh

r2: run-2clusters
run-2clusters:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/simulate_2clusters.sh

r4: run-4clusters
run-4clusters:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/simulate_4clusters.sh

r2-perf:
	./dsm-scripts/config.sh
	make r2

r4-perf:
	./dsm-scripts/config.sh
	make r4

prepare:
	./dsm-scripts/config_memdev.sh cxl-new
	python3 ./dsm-scripts/prepare_hostfs.py
	./quick-build.sh

c: clean
clean:
	./dsm-scripts/clean_memdev.sh

ip2c:
	addr2line -e user/build/ramdisk/$(P) -fCi $(IP)

ip2c-kernel:
	addr2line -e build/kernel.img -fCi $(IP)

test: cfork
cfork:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/simulate_cfork.sh leveldb

cfork-prepare:
	./dsm-scripts/tests/cfork_prepare.exp

cfork-restore:
	./dsm-scripts/tests/cfork_restore.exp

llama-bench:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/tests/llama-bench.exp

llama-cli:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/tests/llama-cli.exp

cxl-new:
	./dsm-scripts/config_memdev.sh cxl-new

cluster:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/simulate_cluster.sh

4cluster:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/simulate_4clusters.sh

leveldb:
	./dsm-scripts/config_memdev.sh cxl
	./dsm-scripts/leveldb.sh
