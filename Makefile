IP = '0x0'
P = 'libc.so'

.PHONY: build build-all

help:
	@echo "make b: build the system without cleaning"
	@echo "make build-all / ba: build the whole system"
	@echo "make run / r: start the qemu"
	@echo "make clean / c: clean the system"
	@echo "make test: run tests under the directory ./dsm-scripts/tests"
	@echo "make prepare: prepare the system (only need to run once after the first clone)"

b: build
build:
	./chbuild build

ba: build-all
build-all:
	./quick-build.sh

clean-dsm:
	./dsm-scripts/config_memdev.sh cxl

perf-config:
	./dsm-scripts/config.sh

r: run
run: clean-dsm
	./build/simulate.sh

r2: run-2clusters
run-2clusters: clean-dsm
	./dsm-scripts/simulate_2clusters.sh

r4: run-4clusters
run-4clusters: clean-dsm
	./dsm-scripts/simulate_4clusters.sh

r2-perf: perf-config r2

r4-perf: perf-config r4

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
cfork: clean-dsm
	./dsm-scripts/simulate_cfork.sh $(APP)

cfork-prepare:
	./dsm-scripts/tests/cfork_prepare.exp $(APP)

cfork-restore:
	./dsm-scripts/tests/cfork_restore.exp $(APP)

llama-bench: clean-dsm
	./dsm-scripts/tests/llama-bench.exp

llama-cli: clean-dsm
	./dsm-scripts/tests/llama-cli.exp

cxl-new:
	./dsm-scripts/config_memdev.sh cxl-new

cluster: clean-dsm
	./dsm-scripts/simulate_cluster.sh

4cluster: clean-dsm
	./dsm-scripts/simulate_4clusters.sh

leveldb: clean-dsm
	./dsm-scripts/leveldb.sh

json-test-py: clean-dsm
	./dsm-scripts/tests/python.exp json_test.py json/english.json

float-test-py: clean-dsm
	./dsm-scripts/tests/python.exp float_operation.py 1000000

function-bench: json-test float-test matmul-test linpack-test pyaes-test
	@echo "All function benchmarks completed successfully."

json-test: clean-dsm
	./dsm-scripts/tests/function-bench/json.exp english.json

float-test: clean-dsm
	./dsm-scripts/tests/function-bench/float.exp 10000000

matmul-test: clean-dsm
	./dsm-scripts/tests/function-bench/matmul.exp 2000

linpack-test: clean-dsm
	./dsm-scripts/tests/function-bench/linpack.exp 2000

pyaes-test: clean-dsm
	./dsm-scripts/tests/function-bench/pyaes.exp 1000000 100

cfork-matmul-prepare: clean-dsm
	./dsm-scripts/tests/cfork_prepare.exp matmul

cfork-matmul-restore:
	./dsm-scripts/tests/cfork_restore.exp matmul

pca: clean-dsm
	./dsm-scripts/tests/phoenix/pca.exp 96

matrix_multiply: clean-dsm
	./dsm-scripts/tests/phoenix/matrix_multiply.exp 16

kmeans: clean-dsm
	./dsm-scripts/tests/phoenix/kmeans.exp 16

string_match: clean-dsm
	./dsm-scripts/tests/phoenix/string_match.exp 16

linear_regression: clean-dsm
	./dsm-scripts/tests/phoenix/linear_regression.exp 16

word_count: clean-dsm
	./dsm-scripts/tests/phoenix/word_count.exp 16

histogram: clean-dsm
	./dsm-scripts/tests/phoenix/histogram.exp 16

clean-ramdisk:
	rm -rf ./user/build/ramdisk/*

clean-libc:
	rm -rf ./user/musl-1.1.24/build/*