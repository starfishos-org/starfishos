#!/usr/bin/bash

# test mode "IPMI" or "QEMU"
test_mode="IPMI"
# test_mode="QEMU"

basedir="/home/<basedir>/treesls"
aedir="$basedir/artificial_evaluation"
logbasedir="$aedir/logs/$test_mode"
appdir="$aedir/applications"
kconfig="$basedir/kernel/sls_config.cmake"
