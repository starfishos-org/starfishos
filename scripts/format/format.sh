#!/bin/bash

set -e

script_dir=$(dirname "$0")

RED='\033[0;31m'
BLUE='\033[0;34m'
GREEN='\033[0;32m'
ORANGE='\033[0;33m'
BOLD='\033[1m'
NONE='\033[0m'

main() {
    for mod in "$@"; do
        for file in $(find $mod -type f); do
            echo "Formatting \"$file\"..."
            format $file
        done
    done
    echo "============"
    echo "Done"
}

format() {
    type=$(file $1)
    if (echo $type | grep -q "C source") || (echo $type | grep -q "C++ source"); then
        clang-format -i -style=file $1
        clang-format -i -style=file $1 # run clang-format twice in case of a bug
    elif (echo $type | grep -q "Bourne-Again shell script"); then
        shfmt -i 4 -w $1
    elif (echo $type | grep -q "Python script"); then
        black -q $1
    elif (echo $1 | grep -q "CMakeLists.txt") || (echo $1 | grep -q "*.cmake"); then
        cmake-format -c $script_dir/cmake_format_config.py -i $1
    fi
}

print_usage() {
    echo -e "\
${BOLD}Usage:${NONE} ./scripts/format/format.sh [path1 [path2 ...]]

${BOLD}Supported Languages:${NONE}
    C/C++
    Bash
    Python
    CMake

${BOLD}Examples:${NONE}
    ./scripts/format/format.sh kernel/ipc
    ./scripts/format/format.sh user/system-servers/fsm user/system-servers/procmgr user/init/main.c
"
}

if [ $# -eq 0 ]; then
    print_usage
    exit
fi

if [ -f /.dockerenv ]; then
    # we are in docker container
    main $@
else
    echo "Starting docker container to do formatting"
    docker run -it --rm \
        -u $(id -u ${USER}):$(id -g ${USER}) \
        -v $(pwd):/chos -w /chos \
        ipads/chcore_formatter:v2.0 \
        ./scripts/format/format.sh $@
fi
