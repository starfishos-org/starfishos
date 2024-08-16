source ./test-scripts/config.sh

sed -i \
    -e 's/set(DSM_HEAP_MODE "CXL")/set(DSM_HEAP_MODE "DRAM")/' \
    $basedir/kernel/dsm_config.cmake

echo "config to heap_cxl_only completed"
