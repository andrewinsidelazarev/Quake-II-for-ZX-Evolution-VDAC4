// =====================================================================================================
// bz_raster_wrap.sv — обёртка для пробной сборки Gowin одного растеризатора (сколько логики и какая
// частота), не для платы.
// =====================================================================================================
//
// Зачем обёртка: у bz_raster больше тысячи входов и выходов, столько ножек у ПЛИС нет. Если же оставить
// входы постоянными, синтезатор «сократит» почти всю схему (зная входы, он заранее посчитает выходы).
// Поэтому входы грузятся из длинного сдвигового регистра по одному выводу (sdi), а все выходы сворачиваются
// исключающим ИЛИ в один вывод (sdo): синтезатор не может ничего выбросить, а ножек — четыре.
module bz_raster_wrap (
    input  wire clk27,
    input  wire sdi,
    input  wire go,
    output reg  sdo
);
    // такт ядра — как в оболочке: PLL 2 даёт 60,75 МГц (здесь он нужен только для отчёта о частоте)
    wire clk_mem, clk_sd, clkp, lock;
    fb_pll_sd pll_sd (.clkin(clk27), .phase(4'd0), .clk_sd(clk_sd), .clk_mem(clk_mem), .clkoutp(clkp), .lock(lock));

    localparam int NIN = 5 + 16 + 512 + 384 + 64;
    reg  [NIN-1:0] sh;
    always @(posedge clk_mem) sh <= {sh[NIN-2:0], sdi};

    wire        in_ready, out_valid, idle;
    wire [15:0] ox, oy;
    wire [31:0] ozi, os, ot, ol;
    wire [15:0] dbg;
    bz_raster r (
        .clk(clk_mem), .rst(!lock),
        .in_valid(go), .in_ready(in_ready),
        .in_n(sh[4:0]), .in_mode(sh[20:5]), .in_xy(sh[532:21]), .in_g(sh[916:533]),
        .in_sx0(sh[932:917]), .in_sy0(sh[948:933]), .in_sx1(sh[964:949]), .in_sy1(sh[980:965]),
        .out_valid(out_valid), .out_ready(sdi), .out_x(ox), .out_y(oy),
        .out_zi(ozi), .out_s(os), .out_t(ot), .out_l(ol), .idle(idle), .dbg(dbg)
    );
    always @(posedge clk_mem)
        sdo <= ^{in_ready, out_valid, idle, ox, oy, ozi, os, ot, ol};
endmodule
