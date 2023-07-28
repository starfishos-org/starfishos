#!/bin/bash

RED='\033[0;31m'
BLUE='\033[0;34m'
GREEN='\033[0;32m'
BOLD='\033[1m'
NONE='\033[0m'

function print_usage() {
    echo -e "Usage:
    ${GREEN}./scripts/ci/local_ci.sh${NONE} [OPTION]
    "

    echo -e "${RED}OPTIONS${NONE}:

    ${GREEN}fbinfer${NONE}: infer static analysis

    ${GREEN}unit_test${NONE}:         all the following unit tests
    ${BLUE}common_test${NONE}:        common tests
    ${BLUE}object_test${NONE}:        object tests
    ${BLUE}mm_page_table_test${NONE}: mm page table tests
    ${BLUE}mm_allocators_test${NONE}: mm allocators tests

    ${GREEN}basic_test${NONE}:          all the following basic tests
    ${BLUE}build_test${NONE}:           build with each default configs
    ${BLUE}basic_x86_qemu${NONE}:       basic x86 tests on qemu
    ${BLUE}basic_riscv64_qemu${NONE}:   basic riscv64 tests on qemu
    ${BLUE}basic_raspi3_qemu${NONE}:    basic arm tests on qemu

    ${BLUE}basic_raspi3_board_local${NONE}:   basic arm tests on raspi3 board (local)
    ${BLUE}basic_hikey970_board_local${NONE}: basic arm tests on hikey (local)

    ${GREEN}full_test${NONE}:          all the following full tests
    ${BLUE}full_x86_qemu${NONE}:       full x86 tests on qemu
    ${BLUE}full_riscv64_qemu${NONE}:   full riscv64 tests on qemu
    ${BLUE}full_raspi3_qemu${NONE}:    full arm tests on qemu

    ${BLUE}full_raspi3_board_local${NONE}:   full arm tests on raspi3 board (local)
    ${BLUE}full_hikey970_board_local${NONE}: full arm tests on hikey (local)
    "
}

function check_if_running_as_root() {
    if [[ "$UID" -ne "0" ]]; then
        echo -e "${RED}Error:${NONE} You must run this script as root!"
        exit -1
    fi
}

function check_if_running_in_root_dir() {
    script_path=`pwd`
    name="${script_path##*/}"
    if [[ "$name" != "chos" ]]; then
        echo -e "${RED}Error:${NONE} You must run this script in the root directory (chos)!"
        exit -1
    fi
}

function detect_error() {
    # $1: exit code
    # $2: test name
    if [ $1 -ne 0 ]; then
        echo -e "${RED}Error:${NONE}" $2 "failed, exit code: $1"
        exit $1
    else
        echo -e "${GREEN}$2 passed${NONE}"
    fi
}

function reset_raspi3() {
    while :; do
        echo -e "${GREEN} 1. Please copy the new image to sd card. ${NONE}"
        echo -e "${RED} 2. DO NOT POWER ON until the screen is cleared! ${NONE}"
        echo -e "${GREEN} 3. Please POWER on the board when the screen is cleared. ${NONE}"
        read -p "Have you prepared the board and made sure it turned off? (y/n): " ANS
        if [[ $ANS == "Y" || $ANS == "y" ]]; then
            break
        fi
    done
}

function reset_hikey() {
    while :; do
        echo -e "${GREEN}Please reset the board!!!${NONE}"
        read -p "Have you reset the hikey board? (y/n): " ANS
        if [[ $ANS == "Y" || $ANS == "y" ]]; then
            break
        fi
    done
}

function docker_helper() {
    test -t 1 && use_tty="-t"
    docker run -i $use_tty --rm -u $(id -u ${USER}):$(id -g ${USER}) -v $(pwd):/chos -w /chos promisivia/treesls_chcore_builder:v2.0 "$@"
}

function docker_test() {
    test -t 1 && use_tty="-t"
    docker run -i $use_tty --user root --rm --privileged --network host --device /dev/ttyXRUSB0:/dev/ttyXRUSB0 --device /dev/ttyUSB0:/dev/ttyUSB0 -v $(pwd):/chos -w /chos ipads/chcore_tester:v1.2 "$@"
}

function fbinfer() {
    docker_helper ./scripts/ci/local_ci_scripts/prepare_infer.sh
    docker_test ./scripts/ci/local_ci_scripts/infer.sh
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function common_test() {
    docker_helper ./scripts/ci/local_ci_scripts/common_test.sh
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function object_test() {
    docker_helper ./scripts/ci/local_ci_scripts/object_test.sh
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function mm_page_table_test() {
    docker_helper ./scripts/ci/local_ci_scripts/mm_page_table_test.sh
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function mm_allocators_test() {
    docker_helper ./scripts/ci/local_ci_scripts/mm_allocators_test.sh
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function build_test() {
    cfgs=$(find ./scripts/build/defconfigs -name "*.config" | grep -v '/ci_')
    for cfg in $cfgs; do
        cfg=$(basename $cfg .config)
        ./chbuild distclean
        ./chbuild defconfig $cfg
        ./chbuild clean
        ./chbuild build
        ret=$?
        detect_error $ret "build_test for $cfg"
    done
}

function basic_x86_qemu() {
    ./chbuild distclean
    ./chbuild defconfig ci_x86_64
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_basic_qemu.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function basic_riscv64_qemu() {
    ./chbuild distclean
    ./chbuild defconfig ci_riscv64
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_basic_qemu.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function basic_raspi3_qemu() {
    ./chbuild distclean
    ./chbuild defconfig ci_raspi3_qemu
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_basic_qemu.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function basic_raspi3_board_local() {
    ./chbuild distclean
    ./chbuild defconfig ci_raspi3_board
    ./chbuild clean
    ./chbuild build
    reset_raspi3
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_basic_raspi3_local.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function basic_hikey970_board_local() {
    reset_hikey
    ./chbuild distclean
    ./chbuild defconfig ci_hikey970
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_basic_hikey970.exp 0
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function full_x86_qemu() {
    ./chbuild distclean
    ./chbuild defconfig ci_x86_64
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_full_qemu.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function full_riscv64_qemu() {
    ./chbuild distclean
    ./chbuild defconfig ci_riscv64
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_full_qemu.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function full_raspi3_qemu() {
    ./chbuild distclean
    ./chbuild defconfig ci_raspi3_qemu
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_full_qemu.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function full_raspi3_board_local() {
    ./chbuild distclean
    ./chbuild defconfig ci_raspi3_board
    ./chbuild clean
    ./chbuild build
    reset_raspi3
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_full_raspi3_local.exp
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function full_hikey970_board_local() {
    reset_hikey
    ./chbuild distclean
    ./chbuild defconfig ci_hikey970
    ./chbuild clean
    ./chbuild build
    docker_test ./scripts/ci/expect_wrapper.sh ./scripts/ci/test_full_hikey970.exp 0
    ret=$?
    detect_error $ret ${FUNCNAME[0]}
}

function unit_test() {
    common_test
    object_test
    mm_page_table_test
    mm_allocators_test
}

function basic_test() {
    build_test
    basic_x86_qemu
    basic_riscv64_qemu
    basic_raspi3_qemu
    # basic_raspi3_board_local
    # basic_hikey970_board_local
}

function full_test() {
    full_x86_qemu
    full_riscv64_qemu
    full_raspi3_qemu
    # full_raspi3_board_local
    # full_hikey970_board_local
}

# check_if_running_in_root_dir
case $# in
    0)
        fbinfer
        unit_test
        basic_test
        full_test
        git rev-parse HEAD
        echo -e "${GREEN}ALL PASSED${NONE}"
        ;;
    1)
        case $1 in
            "fbinfer") fbinfer;;
            "unit_test") unit_test;;
            "common_test") common_test;;
            "object_test") object_test;;
            "mm_page_table_test") mm_page_table_test;;
            "mm_allocators_test") mm_allocators_test;;
            "basic_test") basic_test;;
            "build_test") build_test;;
            "basic_x86_qemu") basic_x86_qemu;;
            "basic_riscv64_qemu") basic_riscv64_qemu;;
            "basic_raspi3_qemu") basic_raspi3_qemu;;
            "basic_raspi3_board_local") basic_raspi3_board_local;;
            # "basic_hikey970_board_local") basic_hikey970_board_local;;
            "full_test") full_test;;
            "full_x86_qemu") full_x86_qemu;;
            "full_raspi3_qemu") full_raspi3_qemu;;
            "full_riscv64_qemu") full_riscv64_qemu;;
            "full_raspi3_board_local") full_raspi3_board_local;;
            # "full_hikey970_board_local") full_hikey970_board_local;;
            "-h"|"--help"|"help") print_usage && exit 0;;
            *) print_usage && exit -1;;
        esac
        git rev-parse HEAD
        echo -e "${GREEN}PASS $1${NONE}"
        ;;
    *)
        print_usage
        exit -1
        ;;
esac

exit 0
