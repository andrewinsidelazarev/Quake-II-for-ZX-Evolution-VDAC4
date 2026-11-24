#!/bin/sh
# Пробная сборка Gowin одного растеризатора (обёртка bz_raster_wrap): стадия E:/zx/cache/tmp/q2gpu_raster_syn.
set -e
cd "$(dirname "$0")/.."
S=/e/zx/cache/tmp/q2gpu_raster_syn
rm -rf $S
mkdir -p $S/source/rtl
cp synth/bz_raster_wrap.sv bz_raster.sv bz_div4.sv vdac2/fb_clocking.sv $S/source/rtl/
cp synth/raster_wrap.tcl synth/raster_wrap.cst synth/raster_wrap.sdc $S/source/
cd $S/source
cmd //c "set TMP=E:\zx\cache\tmp&& set TEMP=E:\zx\cache\tmp&& C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\bin\gw_sh.exe raster_wrap.tcl > ..\build.log 2>&1"
