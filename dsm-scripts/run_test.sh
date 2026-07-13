cpu_num=$1
test_name=$2
mode=$3

./dsm-scripts/change_cpu_num.sh 32

if [ "$mode" = "build" ]; then
    ./chbuild build
elif [ "$mode" = "full-build" ]; then
    ./scripts/quick-build.sh
else
    echo "direct run without build"
fi

./dsm-scripts/config.sh

if [ "$test_name" = "pca" ]; then
    echo "Running PCA function..."
    ./dsm-scripts/pca.exp $cpu_num
elif [ "$test_name" = "kmeans" ]; then
    echo "Running KMeans function..."
    ./dsm-scripts/kmeans.exp $cpu_num
elif [ "$test_name" = "kmeans" ]; then
    echo "Running Wordcount function..."
    ./dsm-scripts/word_count.exp $cpu_num
else
    echo "Unknown test_name: $test_name"
fi