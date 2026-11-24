// =====================================================================================================
// bz_gpu.sv — «бомж-3D» этапа B целиком: ядро, растеризатор, конвейер точки и их память.
// =====================================================================================================
//
// ИЗ ЧЕГО СОСТОИТ
//   bz_core2  — разбирает команды; многоугольники отдаёт растеризатору, точки FILL/POINT/PIC — конвейеру;
//   bz_raster — многоугольник → точки (x, y, zi, s, t, l), по точке за ~1,4 такта;
//   bz_pix    — точка → z-тест, тексель, свет, прозрачность, запись в SDRAM;
//   bz_memif  — память ядра (WRITE, таблицы SETUP) с маленьким кэшем строк;
//   арбитр    — шесть клиентов SDRAM на один порт моста sd_bridge.
//   Снаружи — порт команд, порт моста SDRAM и кадр для вывода (адрес, вспышка, палитра).
//
// КЛИЕНТЫ SDRAM И МЕТКИ
//   Мост (sd_bridge) различает клиентов по 3-битной метке: по ней он возвращает ack, забирает слова
//   записи (wr_take) и раздаёт слова чтения (rd_valid). Порядок операций мост держит внутри одной метки.
//     100 — тексели (кэш текстур): у моста для этой метки своя быстрая очередь, мимо общей;
//     001 — куски z;   011 — куски цвета;   101 — alphamap;   111 — lightmap;   010 — ядро (memif).
//   Метки 000 (вывод) и 110 (загрузки с Evo) — у оболочки (bz_top), они старше всех здесь.
//   Арбитр — простой приоритет в этом порядке: тексели и z нужнее всего (конвейер стоит, пока ждёт).
//   Запрос держится до своего ack, поэтому, если арбитр в какой-то такт показывает мосту другого
//   клиента, запрос не теряется — он просто подождёт.
module bz_gpu (
    input  wire        clk,
    input  wire        rst,
    // ---- поток команд
    input  wire [31:0] cmd_data,
    input  wire        cmd_valid,
    output wire        cmd_ready,
    // ---- клиентская сторона моста SDRAM (sd_bridge)
    output wire        m_req,
    output wire        m_we,
    output wire [20:0] m_addr,
    output wire [2:0]  m_tag,
    input  wire        m_ack,
    input  wire [2:0]  m_ack_tag,
    input  wire        m_wr_take,
    input  wire [5:0]  m_wr_idx,
    input  wire [2:0]  m_wr_tag,
    output wire [31:0] m_wdata,
    output wire [3:0]  m_wmask,
    input  wire        m_rd_valid,
    input  wire [5:0]  m_rd_idx,
    input  wire [2:0]  m_rd_tag,
    input  wire [31:0] m_rd_data,
    // ---- вывод кадра
    output wire [22:0] shown_fb,
    output wire [31:0] shown_blend,
    output wire        frame_pulse,
    input  wire        swap_ack,
    output wire        bad_cmd,
    output wire [31:0] mark,            // SPEC 0.4: регистр MARK (bz_core2)
    output wire        pal_we,
    output wire [9:0]  pal_addr,
    output wire [7:0]  pal_data,
    output wire        idle,
    output wire [7:0]  dbg_pix
);
    localparam [2:0] T_TX = 3'b100, T_ZB = 3'b001, T_CO = 3'b011, T_AM = 3'b101, T_LR = 3'b111, T_CORE = 3'b010;

    // ------------------------------------------------------------------ ядро
    wire        mem_req, mem_we, mem_nocache, mem_ack, mem_inv;
    wire [22:0] mem_addr;
    wire [31:0] mem_wdata, mem_rdata;
    wire [3:0]  mem_be;
    wire        r_valid, r_ready, r_tag, r_idle;
    wire [4:0]  r_n;
    wire [15:0] r_mode, r_sx0, r_sy0, r_sx1, r_sy1;
    wire        r_ld_we;
    wire [4:0]  r_ld_idx;
    wire [31:0] r_ld_data;
    wire        g_sel, g_valid, g_end, g_tag;
    wire [15:0] g_x, g_y;
    wire [31:0] g_zi, g_s, g_t;
    wire        ps_we, ps_tag, ps_tlin, ps_lslot;
    wire [11:0] ps_mode;
    wire [15:0] ps_k16, ps_ta, ps_tb, ps_tls;
    wire [22:0] ps_taddr;
    wire [6:0]  ps_lw, ps_lh, ps_phase;
    wire [3:0]  ps_lsh;
    wire [31:0] ps_lsmin, ps_ltmin;
    wire        p_rel, p_rel_tag, p_busy, p_ready;
    wire        p_in_ready, p_pix_busy;      // сам конвейер точки (за буфером bz_skid)
    wire [22:0] fb_draw, zaddr, amap;
    wire [15:0] scr_s;
    wire        cmap_we, turb_we, lm_go, lm_slot, lm_busy, p_flush, p_inv;
    wire [11:0] cmap_waddr;
    wire [31:0] cmap_wdata, turb_data;
    wire [6:0]  turb_addr, lm_w, lm_h;
    wire [22:0] lm_addr;
    wire        core_idle;

    bz_core2 core (
        .clk(clk), .rst(rst),
        .cmd_data(cmd_data), .cmd_valid(cmd_valid), .cmd_ready(cmd_ready),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wdata(mem_wdata), .mem_be(mem_be),
        .mem_nocache(mem_nocache), .mem_ack(mem_ack), .mem_rdata(mem_rdata), .mem_inv(mem_inv),
        .shown_fb(shown_fb), .shown_blend(shown_blend), .frame_pulse(frame_pulse), .swap_ack(swap_ack),
        .bad_cmd(bad_cmd), .mark(mark), .pal_we(pal_we), .pal_addr(pal_addr), .pal_data(pal_data),
        .r_valid(r_valid), .r_ready(r_ready), .r_n(r_n), .r_mode(r_mode),
        .r_ld_we(r_ld_we), .r_ld_idx(r_ld_idx), .r_ld_data(r_ld_data),
        .r_sx0(r_sx0), .r_sy0(r_sy0), .r_sx1(r_sx1), .r_sy1(r_sy1), .r_tag(r_tag), .r_idle(r_idle),
        .g_sel(g_sel), .g_valid(g_valid), .g_end(g_end), .g_x(g_x), .g_y(g_y), .g_zi(g_zi), .g_s(g_s),
        .g_t(g_t), .g_tag(g_tag), .p_ready(p_ready),
        .ps_we(ps_we), .ps_tag(ps_tag), .ps_mode(ps_mode), .ps_k16(ps_k16), .ps_taddr(ps_taddr),
        .ps_tlin(ps_tlin), .ps_ta(ps_ta), .ps_tb(ps_tb), .ps_tls(ps_tls), .ps_lslot(ps_lslot),
        .ps_lw(ps_lw), .ps_lh(ps_lh), .ps_lsh(ps_lsh), .ps_lsmin(ps_lsmin), .ps_ltmin(ps_ltmin),
        .ps_phase(ps_phase), .p_rel(p_rel), .p_rel_tag(p_rel_tag), .p_busy(p_busy),
        .fb_draw_o(fb_draw), .zaddr_o(zaddr), .amap_o(amap), .scr_s_o(scr_s),
        .cmap_we(cmap_we), .cmap_waddr(cmap_waddr), .cmap_wdata(cmap_wdata),
        .turb_we(turb_we), .turb_addr(turb_addr), .turb_data(turb_data),
        .lm_go(lm_go), .lm_slot(lm_slot), .lm_addr(lm_addr), .lm_w(lm_w), .lm_h(lm_h), .lm_busy(lm_busy),
        .p_flush(p_flush), .p_inv(p_inv), .idle(core_idle)
    );

    // ------------------------------------------------------------------ растеризатор
    wire        ro_valid, ro_ready, ro_tag, ro_end;
    wire [15:0] ro_x, ro_y, r_dbg;
    wire [31:0] ro_zi, ro_s, ro_t, ro_l;

    // Порт загрузки — через регистр (23.11.2026). Ядро выдаёт r_ld_* комбинационно из своего автомата
    // (номер слова — из nget и pn), а растеризатор пишет по этому номеру в массивы вершин vx/vy. Без
    // регистра синтезатор Gowin на шаге «inference» разбирает всю эту цепочку как адрес записи памяти —
    // 6 часов вместо 15 секунд (проверено пробами: регистр на номер и строб — быстро, на данные — нет).
    // Слово доходит на такт позже — это безопасно: многоугольник (r_valid) ядро отдаёт не раньше чем
    // через 2 такта после последнего слова (S_TS5/S_GET → S_PREP → S_P_SUB), а r_ready между словом и
    // его записью упасть не может (падает только при приёме многоугольника). Заодно короче путь тайминга.
    reg         q_ld_we;
    reg  [4:0]  q_ld_idx;
    reg  [31:0] q_ld_data;
    always @(posedge clk) begin
        q_ld_we   <= r_ld_we && !rst;
        q_ld_idx  <= r_ld_idx;
        q_ld_data <= r_ld_data;
    end

    bz_raster raster (
        .clk(clk), .rst(rst),
        .ld_we(q_ld_we), .ld_idx(q_ld_idx), .ld_data(q_ld_data),
        .in_valid(r_valid), .in_ready(r_ready), .in_n(r_n), .in_mode(r_mode),
        .in_sx0(r_sx0), .in_sy0(r_sy0), .in_sx1(r_sx1), .in_sy1(r_sy1), .in_tag(r_tag),
        .out_valid(ro_valid), .out_ready(ro_ready), .out_x(ro_x), .out_y(ro_y),
        .out_zi(ro_zi), .out_s(ro_s), .out_t(ro_t), .out_l(ro_l), .out_tag(ro_tag), .out_end(ro_end),
        .idle(r_idle), .dbg(r_dbg)
    );

    // ------------------------------------------------------------------ конвейер точки: вход — растеризатор
    // или ядро (пока ядро выдаёт точки FILL/POINT/PIC, растеризатор пуст — ядро этого дождалось)
    wire        pi_valid = g_sel ? g_valid : ro_valid;
    // Буфер на две точки (bz_skid, 24.11.2026, тайминг ядра 0.02). Готовность конвейера точки (in_ready)
    // считается из всех его ступеней (промах кэша текселей, z, цвет) — длинная логика. Раньше она шла
    // прямо в разрешение тактов растеризатора (ro_ready): в сборке e 170 из 200 худших путей clk_mem —
    // «конвейер точки → растеризатор», −1,2 нс. Теперь растеризатор и ядро видят готовность буфера — она
    // из регистра; буфер сам принимает точку за такт (пропускная способность прежняя). Порядок точек тот
    // же; занятость буфера входит в p_busy (слив перед FRAME, WRITE, MARK и idle её ждут).
    wire [161:0] pi_data = g_sel ? {g_x, g_y, g_zi, g_s, g_t, 32'd0, g_tag, g_end}
                                 : {ro_x, ro_y, ro_zi, ro_s, ro_t, ro_l, ro_tag, ro_end};
    wire         sk_valid, sk_busy;
    wire [161:0] sk_data;
    bz_skid #(.W(162)) skid (
        .clk(clk), .rst(rst),
        .s_valid(pi_valid), .s_data(pi_data), .s_ready(p_ready),
        .m_valid(sk_valid), .m_data(sk_data), .m_ready(p_in_ready), .busy(sk_busy)
    );
    assign      ro_ready = !g_sel && p_ready;
    assign      p_busy   = p_pix_busy || sk_busy;

    wire        tx_req, tx_ack, tx_rd;
    wire [20:0] tx_addr;
    wire        zb_req, zb_we, zb_ack, zb_wr_take, zb_rd;
    wire [20:0] zb_addr;
    wire [31:0] zb_wdata;
    wire [3:0]  zb_wmask;
    wire        co_req, co_we, co_ack, co_wr_take, co_rd;
    wire [20:0] co_addr;
    wire [31:0] co_wdata;
    wire [3:0]  co_wmask;
    wire        am_req, am_ack, am_rd;
    wire [20:0] am_addr;
    wire        lr_req, lr_ack, lr_rd;
    wire [20:0] lr_addr;

    bz_pix pix (
        .clk(clk), .rst(rst),
        .in_valid(sk_valid), .in_ready(p_in_ready),
        .in_x(sk_data[161:146]), .in_y(sk_data[145:130]),
        .in_zi(sk_data[129:98]), .in_s(sk_data[97:66]), .in_t(sk_data[65:34]),
        .in_l(sk_data[33:2]), .in_tag(sk_data[1]), .in_end(sk_data[0]),
        .rel(p_rel), .rel_tag(p_rel_tag),
        .ps_we(ps_we), .ps_tag(ps_tag), .ps_mode(ps_mode), .ps_k16(ps_k16), .ps_taddr(ps_taddr),
        .ps_tlin(ps_tlin), .ps_ta(ps_ta), .ps_tb(ps_tb), .ps_tls(ps_tls), .ps_lslot(ps_lslot),
        .ps_lw(ps_lw), .ps_lh(ps_lh), .ps_lsh(ps_lsh), .ps_lsmin(ps_lsmin), .ps_ltmin(ps_ltmin),
        .ps_phase(ps_phase),
        .fb_draw(fb_draw), .zaddr(zaddr), .amap(amap), .scr_s(scr_s),
        .cmap_we(cmap_we), .cmap_waddr(cmap_waddr), .cmap_wdata(cmap_wdata),
        .turb_we(turb_we), .turb_addr(turb_addr), .turb_data(turb_data),
        .lm_go(lm_go), .lm_slot(lm_slot), .lm_addr(lm_addr), .lm_w(lm_w), .lm_h(lm_h), .lm_busy(lm_busy),
        .flush(p_flush), .inv(p_inv), .busy(p_pix_busy),
        .m_wr_idx(m_wr_idx), .m_rd_idx(m_rd_idx), .m_rd_data(m_rd_data),
        .tx_req(tx_req), .tx_addr(tx_addr), .tx_ack(tx_ack), .tx_rd(tx_rd),
        .zb_req(zb_req), .zb_we(zb_we), .zb_addr(zb_addr), .zb_ack(zb_ack), .zb_wr_take(zb_wr_take),
        .zb_wdata(zb_wdata), .zb_wmask(zb_wmask), .zb_rd(zb_rd),
        .co_req(co_req), .co_we(co_we), .co_addr(co_addr), .co_ack(co_ack), .co_wr_take(co_wr_take),
        .co_wdata(co_wdata), .co_wmask(co_wmask), .co_rd(co_rd),
        .am_req(am_req), .am_addr(am_addr), .am_ack(am_ack), .am_rd(am_rd),
        .lr_req(lr_req), .lr_addr(lr_addr), .lr_ack(lr_ack), .lr_rd(lr_rd),
        .dbg(dbg_pix)
    );

    // ------------------------------------------------------------------ память ядра
    wire        mi_req, mi_we, mi_ack, mi_wr_take, mi_rd;
    wire [20:0] mi_addr;
    wire [31:0] mi_wdata;
    wire [3:0]  mi_wmask;

    bz_memif #(.LB(1)) memif (
        .clk(clk), .rst_n(!rst),
        .c_req(mem_req), .c_we(mem_we), .c_addr(mem_addr), .c_wdata(mem_wdata), .c_be(mem_be),
        .c_nocache(mem_nocache), .inv(mem_inv), .c_ack(mem_ack), .c_rdata(mem_rdata),
        .b_req(mi_req), .b_we(mi_we), .b_addr(mi_addr), .b_ack(mi_ack),
        .b_wr_take(mi_wr_take), .b_wr_idx(m_wr_idx), .b_wdata(mi_wdata), .b_wmask(mi_wmask),
        .b_rd_valid(mi_rd), .b_rd_idx(m_rd_idx), .b_rd_data(m_rd_data)
    );

    // ------------------------------------------------------------------ арбитр: приоритет по порядку
    assign m_req  = tx_req || zb_req || co_req || am_req || lr_req || mi_req;
    assign m_tag  = tx_req ? T_TX : zb_req ? T_ZB : co_req ? T_CO : am_req ? T_AM : lr_req ? T_LR : T_CORE;
    assign m_we   = tx_req ? 1'b0 : zb_req ? zb_we : co_req ? co_we : am_req ? 1'b0 : lr_req ? 1'b0 : mi_we;
    assign m_addr = tx_req ? tx_addr : zb_req ? zb_addr : co_req ? co_addr : am_req ? am_addr :
                    lr_req ? lr_addr : mi_addr;
    // ответы — по метке
    assign tx_ack = m_ack && m_ack_tag == T_TX;
    assign zb_ack = m_ack && m_ack_tag == T_ZB;
    assign co_ack = m_ack && m_ack_tag == T_CO;
    assign am_ack = m_ack && m_ack_tag == T_AM;
    assign lr_ack = m_ack && m_ack_tag == T_LR;
    assign mi_ack = m_ack && m_ack_tag == T_CORE;
    assign zb_wr_take = m_wr_take && m_wr_tag == T_ZB;
    assign co_wr_take = m_wr_take && m_wr_tag == T_CO;
    assign mi_wr_take = m_wr_take && m_wr_tag == T_CORE;
    assign m_wdata = (m_wr_tag == T_ZB) ? zb_wdata : (m_wr_tag == T_CO) ? co_wdata : mi_wdata;
    assign m_wmask = (m_wr_tag == T_ZB) ? zb_wmask : (m_wr_tag == T_CO) ? co_wmask : mi_wmask;
    assign tx_rd = m_rd_valid && m_rd_tag == T_TX;
    assign zb_rd = m_rd_valid && m_rd_tag == T_ZB;
    assign co_rd = m_rd_valid && m_rd_tag == T_CO;
    assign am_rd = m_rd_valid && m_rd_tag == T_AM;
    assign lr_rd = m_rd_valid && m_rd_tag == T_LR;
    assign mi_rd = m_rd_valid && m_rd_tag == T_CORE;

    assign idle = core_idle && r_idle && !p_busy && !mem_req;
endmodule

// =====================================================================================================
// bz_skid — буфер на две записи между источником и приёмником с «готовностью» (valid/ready).
// =====================================================================================================
// Зачем: разрезать длинный путь «готовность приёмника → разрешение тактов источника». Источник видит
// s_ready из регистра (= второе место свободно), а не готовность приёмника этого такта. Запись идёт в
// главное место (оно и есть выход m_data); если приёмник в этом такте не взял, а пришла ещё одна —
// она ждёт во втором («запасном», skid) и переходит в главное, как только оно освободится. Так буфер
// принимает и отдаёт по записи за такт, а порядок записей прежний.
module bz_skid #(parameter int W = 8) (
    input  wire         clk,
    input  wire         rst,
    input  wire         s_valid,
    input  wire [W-1:0] s_data,
    output wire         s_ready,
    output wire         m_valid,
    output wire [W-1:0] m_data,
    input  wire         m_ready,
    output wire         busy
);
    reg         v0, v1;
    reg [W-1:0] d0, d1;
    assign s_ready = !v1;
    assign m_valid = v0;
    assign m_data  = d0;
    assign busy    = v0 || v1;
    wire take = s_valid && !v1;
    always @(posedge clk)
        if (rst) begin
            v0 <= 1'b0;
            v1 <= 1'b0;
        end else if (!v0 || m_ready) begin      // главное место свободно или отдаётся в этом такте
            if (v1) begin
                v0 <= 1'b1;                      // запасная — в главное (новой не взяли: s_ready = 0)
                d0 <= d1;
                v1 <= 1'b0;
            end else begin
                v0 <= take;
                if (take) d0 <= s_data;
            end
        end else if (take) begin                  // главное занято и не отдаётся — новая ждёт в запасном
            v1 <= 1'b1;
            d1 <= s_data;
        end
endmodule
