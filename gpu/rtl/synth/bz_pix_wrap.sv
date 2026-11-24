// =====================================================================================================
// bz_pix_wrap.sv — обёртка для пробной сборки Gowin одного конвейера точки (логика и частота), не для платы.
// =====================================================================================================
// Как у bz_raster_wrap: входы — из длинного сдвигового регистра (один вывод sdi), все выходы свёрнуты
// исключающим ИЛИ в один вывод sdo — синтезатор не может ничего выбросить. Триггеры регистра (~800) —
// цена обёртки, не конвейера.
module bz_pix_wrap (
    input  wire clk27,
    input  wire sdi,
    input  wire go,
    output reg  sdo
);
    wire clk_mem, clk_sd, clkp, lock;
    fb_pll_sd pll_sd (.clkin(clk27), .phase(4'd0), .clk_sd(clk_sd), .clk_mem(clk_mem), .clkoutp(clkp), .lock(lock));

    localparam int NIN = 900;
    reg  [NIN-1:0] sh;
    always @(posedge clk_mem) sh <= {sh[NIN-2:0], sdi};

    wire        in_ready, rel, rel_tag, lm_busy, busy;
    wire        tx_req, zb_req, zb_we, co_req, co_we, am_req, lr_req;
    wire [20:0] tx_addr, zb_addr, co_addr, am_addr, lr_addr;
    wire [31:0] zb_wdata, co_wdata;
    wire [3:0]  zb_wmask, co_wmask;
    wire [7:0]  dbg;

    bz_pix p (
        .clk(clk_mem), .rst(!lock),
        .in_valid(go), .in_ready(in_ready),
        .in_x(sh[15:0]), .in_y(sh[31:16]), .in_zi(sh[63:32]), .in_s(sh[95:64]), .in_t(sh[127:96]),
        .in_l(sh[159:128]), .in_tag(sh[160]), .in_end(sh[161]),
        .rel(rel), .rel_tag(rel_tag),
        .ps_we(sh[162]), .ps_tag(sh[163]), .ps_mode(sh[175:164]), .ps_k16(sh[191:176]),
        .ps_taddr(sh[214:192]), .ps_tlin(sh[215]), .ps_ta(sh[231:216]), .ps_tb(sh[247:232]),
        .ps_tls(sh[263:248]), .ps_lslot(sh[264]), .ps_lw(sh[271:265]), .ps_lh(sh[278:272]),
        .ps_lsh(sh[282:279]), .ps_lsmin(sh[314:283]), .ps_ltmin(sh[346:315]), .ps_phase(sh[353:347]),
        .fb_draw(sh[376:354]), .zaddr(sh[399:377]), .amap(sh[422:400]), .scr_s(sh[438:423]),
        .cmap_we(sh[439]), .cmap_waddr(sh[451:440]), .cmap_wdata(sh[483:452]),
        .turb_we(sh[484]), .turb_addr(sh[491:485]), .turb_data(sh[523:492]),
        .lm_go(sh[524]), .lm_slot(sh[525]), .lm_addr(sh[548:526]), .lm_w(sh[555:549]), .lm_h(sh[562:556]),
        .lm_busy(lm_busy), .flush(sh[563]), .inv(sh[564]), .busy(busy),
        .m_wr_idx(sh[570:565]), .m_rd_idx(sh[576:571]), .m_rd_data(sh[608:577]),
        .tx_req(tx_req), .tx_addr(tx_addr), .tx_ack(sh[609]), .tx_rd(sh[610]),
        .zb_req(zb_req), .zb_we(zb_we), .zb_addr(zb_addr), .zb_ack(sh[611]), .zb_wr_take(sh[612]),
        .zb_wdata(zb_wdata), .zb_wmask(zb_wmask), .zb_rd(sh[613]),
        .co_req(co_req), .co_we(co_we), .co_addr(co_addr), .co_ack(sh[614]), .co_wr_take(sh[615]),
        .co_wdata(co_wdata), .co_wmask(co_wmask), .co_rd(sh[616]),
        .am_req(am_req), .am_addr(am_addr), .am_ack(sh[617]), .am_rd(sh[618]),
        .lr_req(lr_req), .lr_addr(lr_addr), .lr_ack(sh[619]), .lr_rd(sh[620]),
        .dbg(dbg)
    );
    always @(posedge clk_mem)
        sdo <= ^{in_ready, rel, rel_tag, lm_busy, busy, tx_req, zb_req, zb_we, co_req, co_we, am_req, lr_req,
                 tx_addr, zb_addr, co_addr, am_addr, lr_addr, zb_wdata, co_wdata, zb_wmask, co_wmask, dbg};
endmodule
