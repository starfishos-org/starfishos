source ./test-scripts/config.sh

./dsm-scripts/change_cpu_num.sh 64
cd $basedir
./quick-build.sh