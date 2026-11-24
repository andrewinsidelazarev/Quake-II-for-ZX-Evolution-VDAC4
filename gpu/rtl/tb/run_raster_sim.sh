#!/bin/sh
# Стенд Verilator растеризатора этапа B: bz_raster против C-модели на трассе Quake 2 (tb_raster.cpp).
#
#   sh gpu/rtl/tb/run_raster_sim.sh [КАТАЛОГ]   — собрать КАТАЛОГ/tb_raster.exe (по умолчанию E:/zx/cache/tmp/raster_sim)
#   КАТАЛОГ/tb_raster.exe results/bz/t169.bzcmd [кадров] [задержки выхода 0/1]
#
# Инструменты — как у run_bz_sim.sh: Verilator 5 (E:/zx/tools/rtl/ucrt64), clang (E:/zx/llvm); всё временное — на E:.
set -e
cd "$(dirname "$0")/../.."
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
CC=${CC:-/e/zx/llvm/bin/clang}
export VERILATOR_ROOT="$VR"
export TMP=E:/zx/cache/tmp TEMP=E:/zx/cache/tmp

out=${1:-E:/zx/cache/tmp/raster_sim}
mkdir -p "$out"
rm -rf "$out/obj"
"$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
    --top-module bz_raster --Mdir "$out/obj" rtl/bz_div4.sv rtl/bz_raster.sv
"$CC" -O2 -c -w -D_CRT_SECURE_NO_WARNINGS model/bomzh.c -o "$out/bomzh.o"
"$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" -Imodel \
    "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
    rtl/tb/tb_raster.cpp "$out/bomzh.o" -o "$out/tb_raster.exe"
echo "собран $out/tb_raster.exe"
