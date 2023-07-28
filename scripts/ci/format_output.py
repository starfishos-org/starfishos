#!/usr/bin/env python3

# XXX: This script is temporarily used to wipe off "\n$ " pattern in output of tests in expect scripts.
# "\n$ " is the prefix printed in init.bin, which can interfere with app's output.

# Why not use existing tools (sed, tr, etc.)? Because 1. some tools process text at
# line granularity thus is improper to handle '\n' and 2. some tools buffer texts
# internally so that inconvenient to handle streaming outputs.

import sys

str = "\n$ "
idx = 0

for line in sys.stdin:
    for char in line:
        if char == str[idx]:
            if idx == len(str) - 1:
                idx = 0
            else:
                idx += 1
        else:
            for i in range(0, idx):
                print(str[i], end='')
            idx = 0
            print(char, end='')

