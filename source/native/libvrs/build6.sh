#!/bin/sh
# lib6: release libvrs.a, debug libvrs_dbg.a (-DVRS_DBG_HOOKS), and the drivers; CPUs 0-11
set -e
cd ~/vrsint7
CXX="taskset -c 0-11 g++ -O2 -std=c++20 -mcx16 -pthread -fPIC -Wall -Wno-unused-function -Wno-unused-variable -Wno-misleading-indentation -I swegca_cpp"
rm -f vrs_lib.o vrs_lib_dbg.o libvrs.a libvrs_dbg.a
$CXX -c vrs_lib.cpp -o vrs_lib.o 2>&1 | grep -E "error|warning" -A4 | head -40 || true
$CXX -DVRS_DBG_HOOKS -c vrs_lib.cpp -o vrs_lib_dbg.o 2>&1 | grep -E "error" -A4 | head -20 || true
ar rcs libvrs.a vrs_lib.o obj/evidence_rules.o obj/sha256.o obj/strong_types.o
ar rcs libvrs_dbg.a vrs_lib_dbg.o obj/evidence_rules.o obj/sha256.o obj/strong_types.o
for t in vrs_cli vrs_t_insert5 vrs_t_shut vrs_t_live5 t6drv; do $CXX -I. -o $t $t.cpp libvrs.a -pthread 2>&1 | grep error -A3 | head -5 || true; done
$CXX -I. -o t6drv_dbg t6drv.cpp libvrs_dbg.a -pthread 2>&1 | grep error -A3 | head -5 || true
sha256sum libvrs.a libvrs_dbg.a | cut -c1-16
