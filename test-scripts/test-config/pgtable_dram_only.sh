source ./test-scripts/config.sh

sed -i \
    -e 's/set(DSM_MALLOC_MODE "CXL")/set(DSM_MALLOC_MODE "MIXED")/' \
    -e 's/set(DSM_PGTABLE_MODE "CXL")/set(DSM_PGTABLE_MODE "DRAM")/' \
    $basedir/kernel/dsm_config.cmake

echo "config to pgtable_dram_only completed"
