#!/bin/sh
# Стенд Verilator «бомж-3D»: RTL bz_core против C-модели bomzh.c на трассе Quake 2 (tb_bz.cpp).
#
#   sh gpu/rtl/tb/run_bz_sim.sh [КАТАЛОГ]   — собрать КАТАЛОГ/tb_bz.exe (по умолчанию E:/zx/cache/tmp/bz_sim)
#   КАТАЛОГ/tb_bz.exe results/bz/rec169.bzcmd [кадров] [задержка_памяти]
#
# Инструменты те же, что у стендов VDAC4: Verilator 5 из E:/zx/tools/rtl/ucrt64, clang++ из E:/zx/llvm.
# Временные файлы — только на E: (каталог сборки в E:/zx/cache/tmp).
set -e
cd "$(dirname "$0")/../.."
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
CC=${CC:-/e/zx/llvm/bin/clang}
export VERILATOR_ROOT="$VR"
export TMP=E:/zx/cache/tmp TEMP=E:/zx/cache/tmp

out=${1:-E:/zx/cache/tmp/bz_sim}
mkdir -p "$out"
rm -rf "$out/obj"
"$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
    --top-module bz_core --Mdir "$out/obj" rtl/bz_div.sv rtl/bz_core.sv
"$CC" -O2 -c -w -D_CRT_SECURE_NO_WARNINGS model/bomzh.c -o "$out/bomzh.o"
"$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" -Imodel \
    "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
    rtl/tb/tb_bz.cpp "$out/bomzh.o" -o "$out/tb_bz.exe"
echo "собран $out/tb_bz.exe"
