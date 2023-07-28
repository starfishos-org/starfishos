#!/usr/bin/bash

source ../config.sh

sed -i "/SLS_RESTORE/c\set(SLS_RESTORE OFF)" $kconfig
sed -i "/SLS_EXT_SYNC/c\set(SLS_EXT_SYNC OFF)" $kconfig
sed -i "/SLS_HYBRID_MEM/c\set(SLS_HYBRID_MEM OFF)" $kconfig
sed -i "/SLS_REPORT_CKPT/c\set(SLS_REPORT_CKPT OFF)" $kconfig
sed -i "/SLS_REPORT_RESTORE/c\set(SLS_REPORT_RESTORE OFF)" $kconfig
sed -i "/SLS_REPORT_HYBRID/c\set(SLS_REPORT_HYBRID OFF)" $kconfig
sed -i "/SLS_SPECIAL_OMIT_PF/c\set(SLS_SPECIAL_OMIT_PF ON)" $kconfig
sed -i "/SLS_SPECIAL_OMIT_MEMCPY/c\set(SLS_SPECIAL_OMIT_MEMCPY ON)" $kconfig
sed -i "/SLS_SPECIAL_OMIT_BENCHMARK/c\set(SLS_SPECIAL_OMIT_BENCHMARK ON)" $kconfig

cd $basedir
./chbuild clean
./chbuild build
