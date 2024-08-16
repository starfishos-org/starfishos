source ./test-scripts/config.sh

sed -i \
    -e 's/DSM_HEAP_MODE = CXL/DSM_HEAP_MODE = DRAM/' \
    $basedir/user/musl-1.1.24/Makefile

echo "config to shared_data_cxl_only completed"
