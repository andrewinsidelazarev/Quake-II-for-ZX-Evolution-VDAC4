# Пробная сборка Gowin одного растеризатора (bz_raster_wrap): логика и частота clk_mem. Не для платы.
set_device GW2AR-LV18QN88C8/I7 -device_version C
add_file rtl/bz_raster_wrap.sv
add_file rtl/bz_raster.sv
add_file rtl/bz_div4.sv
add_file rtl/fb_clocking.sv
add_file raster_wrap.cst
add_file raster_wrap.sdc
set_option -output_base_name raster_wrap
set_option -top_module bz_raster_wrap
set_option -verilog_std sysv2017
set_option -synthesis_tool gowinsynthesis
set_option -gen_text_timing_rpt 1
set_option -place_option 2
set_option -route_option 2
run all
