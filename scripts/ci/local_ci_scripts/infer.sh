#!/bin/bash

infer capture --compilation-database kernel/build/compile_commands.json
infer analyze >kernel_fbinfer.res 2>&1
cat kernel_fbinfer.res
grep "No issues found" kernel_fbinfer.res
ret_kern=$?
cp infer-out/report.txt infer_kernel_report.txt

rm -rf kernel/build
rm -rf infer-out kernel_fbinfer.res

exit $ret_kern
