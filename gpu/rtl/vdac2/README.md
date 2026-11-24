# Модули из FT-ядра VDAC4

Копии файлов `E:\zx\Tang Nano 20k VDAC\fpga\vdac2\rtl` (FT-ядро 1.45, слот 1, на плате с 31.10.2026),
снятые 23.11.2026 без изменений. Проект Quake 2 их не правит: если нужна правка — делать здесь,
с комментарием, что отличается от VDAC4.

| Файл | Что берётся |
|---|---|
| fb_clocking.sv | fb_pll_xtal (HDMI 720p: 371,25 / 74,25 МГц), fb_pll_sd (SDRAM 121,5 МГц, clk_mem 60,75, CLKOUTP со сдвигом) |
| ts_clocking.sv | ts_clkdiv5 (пиксельный клок из последовательного) |
| sdram_burst.sv | sdram_burst + sdram_train (пачки по 8 слов, подбор фазы после включения) |
| sd_bridge.sv | мост клиентов clk_mem → контроллер clk_sd |
| hdmi_tx.sv, tmds_encoder.sv, tmds_serializer_gowin.sv | HDMI с AVI InfoFrame, сериализатор OSER10 |
| fb_link.sv | LINK: ведомый QSPI для ESP (регистры и окна) |
| cp_spi.sv | SPI BL616: снимок STATUS (бит перезагрузки — возврат в слот 0 через BL616) |
| ft_spi.sv | приёмник SPI Z-контроллера Evo (14/28 МГц) — второй вход и признак сброса Evo |
