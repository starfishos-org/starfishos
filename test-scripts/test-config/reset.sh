source ./test-scripts/config.sh

sed -i \
    -e 's/DSM_HEAP_MODE = DRAM/DSM_HEAP_MODE = CXL/' \
    $basedir/user/musl-1.1.24/Makefile

sed -i \
    -e 's/set(DSM_MALLOC_MODE "MIXED")/set(DSM_MALLOC_MODE "CXL")/' \
    -e 's/set(DSM_PGTABLE_MODE "DRAM")/set(DSM_PGTABLE_MODE "CXL")/' \
    -e 's/set(DSM_HEAP_MODE "DRAM")/set(DSM_HEAP_MODE "CXL")/' \
    $basedir/kernel/dsm_config.cmake

echo "config to reset completed"