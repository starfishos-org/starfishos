source ./test-scripts/config.sh

threads=(1 2 4 8 16 32 64)
cases=("all_cxl" "pgtable_dram_only" "heap_cxl_only" "shared_data_cxl_only")

for thread in ${threads[@]}
do
    mkdir -p $logdir/$thread
done

for case in ${cases[@]}
do
    bash $testdir/test-config/$case.sh
    for thread in ${threads[@]}
    do
        ./dsm-scripts/config_memdev.sh cxl
        $testdir/matrix_test.exp $thread 2>&1 | tee $logdir/$thread/$case.out
        sleep 30
    done
done

bash $testdir/test-config/reset.sh
