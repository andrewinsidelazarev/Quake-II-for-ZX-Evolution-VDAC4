# =====================================================================================================
# bz_top.sdc — временные ограничения ядра «бомж-3D»: какие такты есть и как их проверять.
# =====================================================================================================
# Зачем: программа размещения (Gowin) проверяет, успевает ли сигнал дойти от триггера до триггера за
# период такта (статический анализ времени, STA). Для этого ей надо знать все такты. Такт, которого
# нет в этом файле, Gowin не проверяет вовсе — схема может работать «как повезёт».
# Gowin не принимает перенос строки через обратную косую черту: каждая команда — одной строкой.

# Первичные такты — приходят на выводы:
#   clk27    — кварц Tang 27 МГц (период 37,037 нс);
#   link_sck — SCK LINK от ESP, до 40 МГц;
#   ft_sck   — SCK Z-контроллера Evo, 14 или 28 МГц;
#   cp_sck   — SCK BL616, 26,67 МГц (с запасом — до 30).
create_clock -name clk27    -period 37.037 -waveform {0 18.518} [get_ports {clk27}]
create_clock -name link_sck -period 25.000 -waveform {0 12.500} [get_ports {link_sck}]
create_clock -name ft_sck   -period 35.714 -waveform {0 17.857} [get_ports {ft_sck}]
create_clock -name cp_sck   -period 33.333 -waveform {0 16.667} [get_ports {cp_sck}]

# Производные такты — их делают PLL внутри ПЛИС (имена выводов — по иерархии bz_top):
#   PLL 1 (pll_hdmi): 27 × 55 / 4 = 371,25 МГц для TMDS; делитель на 5 — точка 74,25 МГц;
#   PLL 2 (pll_sd):   27 × 9 / 2 = 121,5 МГц для SDRAM; CLKOUTD ÷ 2 = 60,75 МГц — ядро.
create_generated_clock -name clk_serial -source [get_ports {clk27}] -multiply_by 55 -divide_by 4 [get_pins {pll_hdmi/pll/CLKOUT}]
create_generated_clock -name pixel_clk -source [get_pins {pll_hdmi/pll/CLKOUT}] -divide_by 5 [get_pins {clkdiv/divider/CLKOUT}]
create_generated_clock -name clk_sd -source [get_ports {clk27}] -multiply_by 9 -divide_by 2 [get_pins {pll_sd/pll/CLKOUT}]
create_generated_clock -name clk_mem -source [get_pins {pll_sd/pll/CLKOUT}] -divide_by 2 [get_pins {pll_sd/pll/CLKOUTD}]

# Группы, между которыми пути не проверяются: сигналы между ними идут только через синхронизаторы
# (переключатель + 2–3 триггера) и память на два такта (буфер строки, палитра) — для них «успеть за
# такт» не имеет смысла. clk_sd и clk_mem — одна группа: они из одной PLL, мост sd_bridge передаёт
# слова между ними за такт, и эти пути Gowin проверять обязан.
set_clock_groups -asynchronous -group [get_clocks {clk_sd clk_mem}] -group [get_clocks {pixel_clk clk_serial}] -group [get_clocks {link_sck}] -group [get_clocks {ft_sck}] -group [get_clocks {cp_sck}]

# Входы без своего такта (их ловят синхронизаторы или их тактирует сам SPI-такт)
set_false_path -from [get_ports {m_sel link_cs_n link_io[*] ft_cs_n ft_mosi cp_cs_n cp_mosi}]

# Сброс сериализаторов TMDS снимается один раз после захвата PLL — статический сигнал
set_false_path -to [get_pins {serializer/ser0/RESET serializer/ser1/RESET serializer/ser2/RESET serializer/serclk/RESET}]

# Отчёты: худшие пути внутри основных тактов и между clk_sd и clk_mem
# clk_mem — 200 путей: после сборки этапа B 23.11 (58 концов с минусом) виден весь хвост, а не 20 худших
report_timing -setup -max_paths 200 -max_common_paths 1 -from_clock [get_clocks {clk_mem}] -to_clock [get_clocks {clk_mem}]
report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock [get_clocks {clk_sd}] -to_clock [get_clocks {clk_sd}]
report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock [get_clocks {pixel_clk}] -to_clock [get_clocks {pixel_clk}]
report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock [get_clocks {clk_sd}] -to_clock [get_clocks {clk_mem}]
report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock [get_clocks {clk_mem}] -to_clock [get_clocks {clk_sd}]
# link_sck (24.11.2026, ядро 0.02): окно чтения SDRAM (bz_rdwin) читается по фронту, fb_link отдаёт по спаду — полутакт 12,5 нс
report_timing -setup -max_paths 20 -max_common_paths 1 -from_clock [get_clocks {link_sck}] -to_clock [get_clocks {link_sck}]
