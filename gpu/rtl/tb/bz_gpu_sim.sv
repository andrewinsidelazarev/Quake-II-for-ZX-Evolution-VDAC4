// =====================================================================================================
// bz_gpu_sim.sv — верх для стенда tb_gpu.cpp: «бомж-3D» этапа B (bz_gpu) и настоящий мост SDRAM (sd_bridge).
// =====================================================================================================
//
// Контроллер SDRAM (sdram_burst) и саму микросхему стенд заменяет моделью на C++: она отвечает на
// команды моста (c_req …) с задержками, похожими на настоящие, и держит 8 МБ памяти. Мост — настоящий,
// со своими очередями и двумя тактами (clk — 60,75 МГц, clk_sd — вдвое быстрее): так стенд проверяет и
// порядок операций клиентов в мосте (быстрая очередь текселей, сбор слов записи).
module bz_gpu_sim (
    input  wire        clk,
    input  wire        clk_sd,
    input  wire        rst,
    input  wire [31:0] cmd_data,
    input  wire        cmd_valid,
    output wire        cmd_ready,
    input  wire        swap_ack,
    output wire        bad_cmd,
    output wire        frame_pulse,
    output wire        idle,
    output wire [7:0]  dbg_pix,
    // сторона контроллера (домен clk_sd)
    output wire        c_req,
    output wire        c_we,
    output wire [20:0] c_addr,
    output wire [2:0]  c_len,
    input  wire        c_ack,
    input  wire        c_wr_take,
    output wire [31:0] c_wdata,
    output wire [3:0]  c_wmask,
    input  wire        c_rd_valid,
    input  wire [31:0] c_rd_data,
    output wire        c_tag,
    input  wire        c_rd_tag,
    // занятость моста (стенду: всё ли уже в памяти)
    output wire        m_req,
    // SPEC 0.4: MARK и чтение SDRAM в окно (bz_rdwin; окно здесь читается тактом clk)
    output wire [31:0] mark,
    input  wire        rd_go,
    input  wire [22:0] rd_addr,
    input  wire [7:0]  rd_len,
    output wire        rd_busy,
    input  wire [9:0]  win_raddr,
    output wire [31:0] win_rdata
);
    wire        g_req, g_we, m_we, m_ack, m_wr_take, m_rd_valid, init_done;
    wire [20:0] g_addr, m_addr;
    wire [2:0]  g_tag, m_tag, m_ack_tag, m_wr_tag, m_rd_tag;
    // копия в окно — как в bz_top: свой запрос раньше рисования, метка 110 (у bz_top — общая с Evo; чей
    // ack — по выбору на прошлом фронте, rb_sel_q)
    localparam [2:0] TAG_RB = 3'b110;
    wire        rb_req, rb_ack, rb_rd_valid;
    wire [20:0] rb_addr;
    reg         rb_sel_q;
    always @(posedge clk) rb_sel_q <= rst ? 1'b0 : rb_req;
    assign m_req  = rb_req || g_req;
    assign m_we   = rb_req ? 1'b0 : g_we;
    assign m_addr = rb_req ? rb_addr : g_addr;
    assign m_tag  = rb_req ? TAG_RB : g_tag;
    assign rb_ack      = m_ack && m_ack_tag == TAG_RB && rb_sel_q;
    assign rb_rd_valid = m_rd_valid && m_rd_tag == TAG_RB;
    bz_rdwin rdwin (
        .clk(clk), .rst_n(!rst), .go(rd_go), .addr(rd_addr), .len(rd_len), .busy(rd_busy),
        .b_req(rb_req), .b_addr(rb_addr), .b_ack(rb_ack), .b_rd_valid(rb_rd_valid), .b_rd_data(m_rd_data),
        .rclk(clk), .raddr(win_raddr), .rdata(win_rdata)
    );
    wire [5:0]  m_wr_idx, m_rd_idx;
    wire [31:0] m_wdata, m_rd_data;
    wire [3:0]  m_wmask;
    wire [22:0] shown_fb;
    wire [31:0] shown_blend;
    wire        pal_we;
    wire [9:0]  pal_addr;
    wire [7:0]  pal_data;

    bz_gpu gpu (
        .clk(clk), .rst(rst),
        .cmd_data(cmd_data), .cmd_valid(cmd_valid), .cmd_ready(cmd_ready),
        .m_req(g_req), .m_we(g_we), .m_addr(g_addr), .m_tag(g_tag),
        .m_ack(m_ack), .m_ack_tag(m_ack_tag), .m_wr_take(m_wr_take), .m_wr_idx(m_wr_idx), .m_wr_tag(m_wr_tag),
        .m_wdata(m_wdata), .m_wmask(m_wmask),
        .m_rd_valid(m_rd_valid), .m_rd_idx(m_rd_idx), .m_rd_tag(m_rd_tag), .m_rd_data(m_rd_data),
        .shown_fb(shown_fb), .shown_blend(shown_blend), .frame_pulse(frame_pulse), .swap_ack(swap_ack),
        .bad_cmd(bad_cmd), .mark(mark), .pal_we(pal_we), .pal_addr(pal_addr), .pal_data(pal_data),
        .idle(idle), .dbg_pix(dbg_pix)
    );

    sd_bridge #(.TAGW(3), .PREFETCH_HEAD(1'b1)) bridge (     // как в bz_top
        .clk(clk), .rst_n(!rst), .clk_sd(clk_sd), .rst_sd_n(!rst),
        .req(m_req), .we(m_we), .addr(m_addr), .req_len(3'd0), .req_tag(m_tag),
        .ack(m_ack), .ack_tag(m_ack_tag),
        .wr_take(m_wr_take), .wr_idx(m_wr_idx), .wr_tag(m_wr_tag),
        .wdata(m_wdata), .wmask(m_wmask), .wdata2(32'd0),
        .rd_valid(m_rd_valid), .rd_idx(m_rd_idx), .rd_tag(m_rd_tag), .rd_data(m_rd_data),
        .rd_phase(2'd0), .init_done(init_done),
        .c_req(c_req), .c_we(c_we), .c_addr(c_addr), .c_len(c_len), .c_ack(c_ack),
        .c_wr_take(c_wr_take), .c_wdata(c_wdata), .c_wmask(c_wmask),
        .c_rd_valid(c_rd_valid), .c_rd_data(c_rd_data), .c_rd_phase(), .c_init_done(1'b1),
        .c_tag(c_tag), .c_rd_tag(c_rd_tag)
    );
endmodule
