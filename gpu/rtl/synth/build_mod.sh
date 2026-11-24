#!/bin/sh
# Пробная сборка Gowin одного узла этапа B через обёртку synth/<узел>_wrap.sv (логика и частота clk_mem).
#   sh gpu/rtl/synth/build_mod.sh bz_pix|bz_core2   — стадия E:/zx/cache/tmp/q2gpu_<узел>_syn, журнал build.log
# Выводы, .cst и .sdc — общие с обёрткой растеризатора (clk27, sdi, go, sdo). Не для платы.
set -e
M=$1
cd "$(dirname "$0")/.."
S=/e/zx/cache/tmp/q2gpu_${M}_syn
rm -rf "$S"
mkdir -p "$S/source/rtl"
cp synth/${M}_wrap.sv bz_ram.sv bz_div4.sv bz_pix.sv bz_core2.sv vdac2/fb_clocking.sv "$S/source/rtl/"
sed "s/bz_raster_wrap/${M}_wrap/g" synth/raster_wrap.cst > "$S/source/wrap.cst"
cp synth/raster_wrap.sdc "$S/source/wrap.sdc"
cat > "$S/source/wrap.tcl" <<EOF
set_device GW2AR-LV18QN88C8/I7 -device_version C
add_file rtl/${M}_wrap.sv
add_file rtl/bz_ram.sv
add_file rtl/bz_div4.sv
add_file rtl/bz_pix.sv
add_file rtl/bz_core2.sv
add_file rtl/fb_clocking.sv
add_file wrap.cst
add_file wrap.sdc
set_option -output_base_name wrap
set_option -top_module ${M}_wrap
set_option -verilog_std sysv2017
set_option -synthesis_tool gowinsynthesis
set_option -gen_text_timing_rpt 1
set_option -place_option 2
set_option -route_option 2
run all
EOF
cd "$S/source"
cmd //c "set TMP=E:\zx\cache\tmp&& set TEMP=E:\zx\cache\tmp&& C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\bin\gw_sh.exe wrap.tcl > ..\build.log 2>&1" || true
echo "стадия $S"
