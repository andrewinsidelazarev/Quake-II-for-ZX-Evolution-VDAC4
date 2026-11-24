// =====================================================================================================
// bz_core2_wrap.sv — обёртка для пробной сборки Gowin одного ядра этапа B (логика и частота), не для платы.
// =====================================================================================================
// Как у bz_raster_wrap: входы — из сдвигового регистра, все выходы свёрнуты XOR в один вывод.
module bz_core2_wrap (
    input  wire clk27,
    input  wire sdi,
    input  wire go,
    output reg  sdo
);
    wire clk_mem, clk_sd, clkp, lock;
    fb_pll_sd pll_sd (.clkin(clk27), .phase(4'd0), .clk_sd(clk_sd), .clk_mem(clk_mem), .clkoutp(clkp), .lock(lock));

    localparam int NIN = 80;
    reg  [NIN-1:0] sh;
    always @(posedge clk_mem) sh <= {sh[NIN-2:0], sdi};

    wire        cmd_ready, mem_req, mem_we, mem_nocache, mem_inv, frame_pulse, bad_cmd, pal_we;
    wire [22:0] mem_addr, shown_fb, fb_draw, zaddr, amap, ps_taddr, lm_addr;
    wire [31:0] mem_wdata, shown_blend, cmap_wdata, turb_data, ps_lsmin, ps_ltmin;
    wire [3:0]  mem_be, ps_lsh;
    wire [9:0]  pal_addr;
    wire [7:0]  pal_data;
    wire        r_valid, r_tag, g_sel, g_valid, g_end, g_tag, ps_we, ps_tag, ps_tlin, ps_lslot;
    wire [4:0]  r_n;
    wire [15:0] r_mode, r_sx0, r_sy0, r_sx1, r_sy1, g_x, g_y, ps_k16, ps_ta, ps_tb, ps_tls, scr_s;
    wire [511:0] r_xy;
    wire [383:0] r_g;
    wire [31:0] g_zi, g_s, g_t;
    wire [11:0] ps_mode, cmap_waddr;
    wire [6:0]  ps_lw, ps_lh, ps_phase, turb_addr, lm_w, lm_h;
    wire        cmap_we, turb_we, lm_go, lm_slot, p_flush, p_inv, idle;

    bz_core2 c (
        .clk(clk_mem), .rst(!lock),
        .cmd_data(sh[31:0]), .cmd_valid(go), .cmd_ready(cmd_ready),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wdata(mem_wdata), .mem_be(mem_be),
        .mem_nocache(mem_nocache), .mem_ack(sh[32]), .mem_rdata(sh[64:33]), .mem_inv(mem_inv),
        .shown_fb(shown_fb), .shown_blend(shown_blend), .frame_pulse(frame_pulse), .swap_ack(sh[65]),
        .bad_cmd(bad_cmd), .pal_we(pal_we), .pal_addr(pal_addr), .pal_data(pal_data),
        .r_valid(r_valid), .r_ready(sh[66]), .r_n(r_n), .r_mode(r_mode), .r_xy(r_xy), .r_g(r_g),
        .r_sx0(r_sx0), .r_sy0(r_sy0), .r_sx1(r_sx1), .r_sy1(r_sy1), .r_tag(r_tag), .r_idle(sh[67]),
        .g_sel(g_sel), .g_valid(g_valid), .g_end(g_end), .g_x(g_x), .g_y(g_y), .g_zi(g_zi), .g_s(g_s),
        .g_t(g_t), .g_tag(g_tag), .p_ready(sh[68]),
        .ps_we(ps_we), .ps_tag(ps_tag), .ps_mode(ps_mode), .ps_k16(ps_k16), .ps_taddr(ps_taddr),
        .ps_tlin(ps_tlin), .ps_ta(ps_ta), .ps_tb(ps_tb), .ps_tls(ps_tls), .ps_lslot(ps_lslot),
        .ps_lw(ps_lw), .ps_lh(ps_lh), .ps_lsh(ps_lsh), .ps_lsmin(ps_lsmin), .ps_ltmin(ps_ltmin),
        .ps_phase(ps_phase), .p_rel(sh[69]), .p_rel_tag(sh[70]), .p_busy(sh[71]),
        .fb_draw_o(fb_draw), .zaddr_o(zaddr), .amap_o(amap), .scr_s_o(scr_s),
        .cmap_we(cmap_we), .cmap_waddr(cmap_waddr), .cmap_wdata(cmap_wdata),
        .turb_we(turb_we), .turb_addr(turb_addr), .turb_data(turb_data),
        .lm_go(lm_go), .lm_slot(lm_slot), .lm_addr(lm_addr), .lm_w(lm_w), .lm_h(lm_h), .lm_busy(sh[72]),
        .p_flush(p_flush), .p_inv(p_inv), .idle(idle)
    );
    always @(posedge clk_mem)
        sdo <= ^{cmd_ready, mem_req, mem_we, mem_nocache, mem_inv, frame_pulse, bad_cmd, pal_we, mem_addr, shown_fb,
                 fb_draw, zaddr, amap, ps_taddr, lm_addr, mem_wdata, shown_blend, cmap_wdata, turb_data, ps_lsmin,
                 ps_ltmin, mem_be, ps_lsh, pal_addr, pal_data, r_valid, r_tag, g_sel, g_valid, g_end, g_tag, ps_we,
                 ps_tag, ps_tlin, ps_lslot, r_n, r_mode, r_sx0, r_sy0, r_sx1, r_sy1, g_x, g_y, ps_k16, ps_ta, ps_tb,
                 ps_tls, scr_s, r_xy, r_g, g_zi, g_s, g_t, ps_mode, cmap_waddr, ps_lw, ps_lh, ps_phase, turb_addr,
                 lm_w, lm_h, cmap_we, turb_we, lm_go, lm_slot, p_flush, p_inv, idle};
endmodule
