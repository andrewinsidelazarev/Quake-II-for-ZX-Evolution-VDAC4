create_clock -name clk27 -period 37.037 -waveform {0 18.518} [get_ports {clk27}]
create_generated_clock -name clk_sd -source [get_ports {clk27}] -multiply_by 9 -divide_by 2 [get_pins {pll_sd/pll/CLKOUT}]
create_generated_clock -name clk_mem -source [get_pins {pll_sd/pll/CLKOUT}] -divide_by 2 [get_pins {pll_sd/pll/CLKOUTD}]
set_false_path -from [get_ports {sdi go}]
