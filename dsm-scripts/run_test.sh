cpu_num=$1
test_name=$2

./dsm-scripts/change_cpu_num.sh 32 $cpu_num

./quick-build.sh

./dsm-scripts/config.sh

if [ "$test_name" = "pca" ]; then
    echo "Running PCA function..."
    ./dsm-scripts/pca.exp
elif [ "$test_name" = "kmeans" ]; then
    echo "Running KMeans function..."
    ./dsm-scripts/kmeans.exp
else
    echo "Unknown test_name: $test_name"
fi