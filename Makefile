IP = '0x0'
P = 'libc.so'
T = 12

.PHONY: build build-all

help:
	@echo "make b: build the system without cleaning"
	@echo "make build-all / ba: build the whole system"
	@echo "make run / r: start the qemu"
	@echo "make clean / c: clean the system"
	@echo "make test: run tests under the directory ./dsm-scripts/tests"
	@echo "make run-mm-test / run-graph-test / run-dbx1000-test: run automated benchmarks"
	@echo "make prepare: prepare the system (only need to run once after the first clone)"

b: build
build:
	./chbuild build

ba: build-all
build-all:
	./scripts/quick-build.sh

clean-dsm:
	./dsm-scripts/config_memdev.sh cxl

start-ivshmem-server:
	./dsm-scripts/start_ivshmem_server.sh

kill-ivshmem-server:
	./dsm-scripts/kill_ivshmem_server.sh

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
	./scripts/quick-build.sh

c: clean
clean:
	./dsm-scripts/clean_memdev.sh

run-mm-test:
	./dsm-scripts/simulate_ncluster.sh 2 mm "source run_matrix_multiply.sh" "matrix multiply finished"

run-graph-test:
	./dsm-scripts/simulate_ncluster.sh 2 graph "pagerank /host/twitter-2010.bin 41652230 50 2" "exec_time=" --timeout=1800

run-medium-graph-test:
	./dsm-scripts/simulate_ncluster.sh 2 graph "pagerank /host/uk-2014-host.bin 4769354 10 2" "exec_time=" --timeout=600

run-dbx1000-test:
	./dsm-scripts/simulate_ncluster.sh 2 dbx1000 "source run_dbx1000.sh" "DBX1000 finished"

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
	./dsm-scripts/tests/leveldb.exp

db1000: clean-dsm
	./dsm-scripts/tests/db1000.exp

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

cross-pca: clean-dsm
	./dsm-scripts/tests/pca-cross-machine.exp $(T)

pca: clean-dsm
	./dsm-scripts/tests/phoenix/pca.exp $(T)

matrix_multiply: clean-dsm
	./dsm-scripts/tests/phoenix/matrix_multiply.exp $(T)

kmeans: clean-dsm
	./dsm-scripts/tests/phoenix/kmeans.exp $(T)

string_match: clean-dsm
	./dsm-scripts/tests/phoenix/string_match.exp $(T)

linear_regression: clean-dsm
	./dsm-scripts/tests/phoenix/linear_regression.exp $(T)

word_count: clean-dsm
	./dsm-scripts/tests/phoenix/word_count.exp $(T)

histogram: clean-dsm
	./dsm-scripts/tests/phoenix/histogram.exp $(T)

kmalloc: clean-dsm
	./dsm-scripts/tests/kmalloc.exp

clean-ramdisk:
	rm -rf ./user/build/ramdisk/*

clean-libc:
	rm -rf ./user/musl-1.1.24/build/*

clean-dsm-meta:
	./dsm-scripts/config_memdev.sh cxl

tmux:
	./dsm-scripts/simulate_tmux.sh

gemini: clean-dsm
	./dsm-scripts/tests/gemini.exp
