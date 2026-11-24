# =====================================================================================================
# gowin_bz.tcl — сборка ядра «бомж-3D» в Gowin (gw_sh), из папки стадии (её готовит build_bz.ps1).
# =====================================================================================================
# gw_sh — консольная оболочка Gowin: выполняет этот сценарий, как IDE по кнопке «Run All».
# Шаги: синтез (Verilog → вентили и триггеры), размещение и трассировка (place & route — куда в
# кристалле встанет каждый элемент и какими проводами соединится), проверка времени (STA по .sdc),
# выпуск битстрима (.fs / .bin — то, что загружается в ПЛИС).
#
# По умолчанию рисует этап B (bz_gpu: bz_core2 + bz_raster + bz_pix). Ключ BZ_STAGEA (build_bz.ps1
# -StageA дописывает `define BZ_STAGEA в копию bz_top.sv) — ядро этапа A (bz_core + bz_memif), с ключом
# BZ_NO3D (-No3D) — оно же без POLY и POINT: пробная сборка оболочки (такты, SDRAM, LINK, вывод).

set_device GW2AR-LV18QN88C8/I7 -device_version C

foreach f {bz_top bz_fifo bz_core bz_div bz_memif bz_evo_in bz_scan bz_ram bz_div4 bz_raster bz_pix bz_core2
           bz_gpu bz_rdwin} {
    add_file rtl/$f.sv
}
foreach f {fb_clocking ts_clocking sdram_burst sd_bridge hdmi_tx tmds_encoder tmds_serializer_gowin fb_link cp_spi ft_spi} {
    add_file rtl/vdac2/$f.sv
}
add_file constraints/bz_top.cst
add_file constraints/bz_top.sdc

set_option -output_base_name bz_top
set_option -top_module bz_top
set_option -verilog_std sysv2017
set_option -include_path {rtl}
set_option -synthesis_tool gowinsynthesis
# выводы служебных интерфейсов загрузки — обычные выводы после загрузки (как у FT-ядра)
set_option -use_sspi_as_gpio 1
set_option -use_mspi_as_gpio 1
set_option -use_ready_as_gpio 1
set_option -use_done_as_gpio 1
set_option -use_reconfign_as_gpio 1
# метка ядра для BL616: «VD4» и личность 0x51 ('Q')
set_option -user_code 0x56443451
set_option -gen_text_timing_rpt 1
set_option -gen_sdf 1
set_option -ioreg_in_iob 1
set_option -rw_check_on_ram 1
set_option -place_option 2
set_option -route_option 2
run all
