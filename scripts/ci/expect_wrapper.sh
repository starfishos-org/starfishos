#!/bin/sh

# Remove log
sudo rm -f ./exec_log

# Killall screen
# sudo killall screen

# Execute expect script
# Note: $2 is only used as device num for HiKey970
echo $1 $2
$1 $2

ret=$?
echo "The exit value of CI script: $ret"

# Check return value
if [ $ret -eq 0 ]; then
        cat ./exec_log
        echo ""
        echo "Succeeded to run expect script $1"
elif [ $ret -eq 1 ]; then
        cat ./exec_log
        echo ""
        echo "[CI] Run expect script $1 timeout, exit value: $ret"
        exit $ret
else
        cat ./exec_log
        echo ""
        echo "[CI] Failed to run expect script $1, exit value: $ret"
        exit $ret
fi
