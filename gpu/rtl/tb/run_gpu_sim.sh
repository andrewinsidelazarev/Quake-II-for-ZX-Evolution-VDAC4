#!/bin/sh
# Стенд Verilator этапа B: bz_gpu (ядро + растеризатор + конвейер точки) с мостом sd_bridge против
# C-модели bomzh.c на трассе Quake 2 (tb_gpu.cpp).
#
#   sh gpu/rtl/tb/run_gpu_sim.sh [КАТАЛОГ]   — собрать КАТАЛОГ/tb_gpu.exe (по умолчанию E:/zx/cache/tmp/gpu_sim)
#   КАТАЛОГ/tb_gpu.exe results/bz/t169.bzcmd [кадров] [сравнивать_с] [зерно]
#
# Инструменты — Verilator 5 из E:/zx/tools/rtl/ucrt64, clang++ из E:/zx/llvm. Временные файлы — только на E:.
set -e
cd "$(dirname "$0")/../.."
VR=${VERILATOR_ROOT:-E:/zx/tools/rtl/ucrt64/share/verilator}
VB=${VERILATOR_BIN:-/e/zx/tools/rtl/ucrt64/bin/verilator_bin.exe}
CXX=${CXX:-/e/zx/llvm/bin/clang++}
CC=${CC:-/e/zx/llvm/bin/clang}
export VERILATOR_ROOT="$VR"
export TMP=E:/zx/cache/tmp TEMP=E:/zx/cache/tmp

out=${1:-E:/zx/cache/tmp/gpu_sim}
mkdir -p "$out"
rm -rf "$out/obj"
"$VB" --cc -O3 --x-assign fast --x-initial fast --noassert -Wno-fatal -Wno-lint -Wno-style \
    --top-module bz_gpu_sim --Mdir "$out/obj" \
    rtl/bz_ram.sv rtl/bz_div4.sv rtl/bz_raster.sv rtl/bz_pix.sv rtl/bz_memif.sv rtl/bz_core2.sv rtl/bz_gpu.sv \
    rtl/bz_rdwin.sv rtl/vdac2/sd_bridge.sv rtl/tb/bz_gpu_sim.sv
"$CC" -O2 -c -w -D_CRT_SECURE_NO_WARNINGS model/bomzh.c -o "$out/bomzh.o"
"$CXX" -O2 -std=c++20 -w -D_CRT_SECURE_NO_WARNINGS -I"$out/obj" -I"$VR/include" -I"$VR/include/vltstd" -Imodel \
    "$out"/obj/*.cpp "$VR/include/verilated.cpp" "$VR/include/verilated_threads.cpp" \
    rtl/tb/tb_gpu.cpp "$out/bomzh.o" -o "$out/tb_gpu.exe"
echo "собран $out/tb_gpu.exe"
