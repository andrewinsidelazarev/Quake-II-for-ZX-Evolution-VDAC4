// =====================================================================================================
// bz_scan.sv — вывод кадра «бомж-3D» на HDMI: картинка 424×240 (байт = индекс палитры) → 1280×720p60.
// =====================================================================================================
//
// РАЗВЁРТКА
//   Монитор рисует кадр строка за строкой, точка за точкой, 60 раз в секунду. В строке 1650 тактов
//   точки (74,25 МГц): 1280 видимых, потом гашение — передняя площадка 110, строчный синхроимпульс 40,
//   задняя площадка 220. В кадре 750 строк: 720 видимых и 30 гашения (5 + 5 импульс + 20). Это формат
//   CEA-861 номер 4 (VIC 4). Счётчики hc (точка в строке) и vc (строка) и есть развёртка.
//
// ОКНО И МАСШТАБ (как в слоте 0: своей настройки нет — слово пользователя 23.11)
//   SIZE 0 — 100 %: 1272×720 — каждая точка источника ровно 3×3 точки экрана, по бокам по 4 точки чёрного;
//   SIZE 1 — 95 %:  1208×684;   SIZE 2 — 90 %: 1144×648 — для старых HDMI-приёмников, режущих края.
//   Номер точки источника считается приращением (DDA — digital differential analyzer, «цифровой
//   дифференциальный анализатор»): на каждую точку экрана к счётчику ax прибавляем 424 (ширину
//   источника); как только ax дорос до ширины окна — вычитаем её и переходим к следующей точке
//   источника. Так без деления получаем floor(c · 424 / ширина_окна). То же по строкам. При 100 % —
//   ровно каждая третья точка и каждая третья строка.
//
// БУФЕР СТРОКИ ВМЕСТО БОЛЬШОГО КАДРА
//   Кадр 1280×720 в SDRAM не хранится: на каждую строку экрана из SDRAM читается одна строка источника
//   (432 байта, 14 пачек по 32 байта) в маленький буфер строки (блочная память BSRAM, два банка: в один
//   пишется следующая строка, из другого выводится текущая) и растягивается на лету. Это ~19 МБ/с
//   полосы SDRAM вместо ~55 МБ/с, если бы растянутый кадр хранить и читать целиком.
//
// ДВА ДОМЕНА ТАКТИРОВАНИЯ
//   pixel_clk (74,25 МГц) — развёртка, чтение буфера строки, палитра, вспышка, выход в hdmi_tx;
//   clk (clk_mem, 60,75 МГц) — выборка строки из SDRAM (клиент sd_bridge), запись буфера строки и палитры.
//   Запрос строки идёт из домена пикселей переключателем (toggle) в начале строки: на выборку есть вся
//   строка (22 мкс). Память буфера строки и палитры — «на два такта» (пишется по одному такту, читается
//   по другому) — так BSRAM умеет сама.
//
// СМЕНА КАДРОВ БЕЗ РАЗРЫВА
//   Адрес показываемого кадра берётся заново только на первой строке окна, поэтому кадр всегда выводится
//   целиком, от начала. В этот момент вывод говорит ядру swap_ack: «новый кадр взял — в старый можно
//   рисовать».
//
// ПАЛИТРА И ВСПЫШКА
//   Байт кадра — номер цвета (0…255); палитра (256 × RGB, из SETUP) даёт сам цвет. Вспышка Quake
//   (урон — красная, подбор — жёлтая, под водой — синяя) — на выводе: v + ((цвет − v) · доля) >> 8.
//
// Шаг строки источника в SDRAM — 432 байта (W вверх до 16, SPEC.md), поэтому нечётные строки начинаются
// с середины пачки: выборка — с адреса, выровненного вниз до 32, и в буфере сдвиг 16 байт.
module bz_scan #(
    parameter int H_ACT = 1280, H_FP = 110, H_SYNC = 40, H_BP = 220,   // 720p60 (CEA-861 VIC 4)
    parameter int V_ACT = 720,  V_FP = 5,   V_SYNC = 5,  V_BP = 20,
    parameter int SRC_W = 424,  SRC_H = 240, SRC_S = 432
) (
    // домен clk_mem
    input  wire        clk,
    input  wire        rst_n,
    input  wire [22:0] shown_fb,        // адрес показанного кадра (bz_core)
    input  wire        frame_pulse,     // ядро выполнило FRAME
    input  wire [31:0] shown_blend,     // вспышка: R, G, B, доля
    output reg         swap_ack,        // такт: вывод взял новый кадр
    input  wire        pal_we,          // палитра от SETUP: байт индекса 0…767
    input  wire [9:0]  pal_addr,
    input  wire [7:0]  pal_data,
    input  wire [1:0]  size,            // 0 — 100 %, 1 — 95 %, 2 — 90 %
    // клиент моста (только чтение)
    output reg         b_req,
    output reg  [20:0] b_addr,
    input  wire        b_ack,
    input  wire        b_rd_valid,
    input  wire [31:0] b_rd_data,
    // домен пикселей
    input  wire        pixel_clk,
    input  wire        prst_n,
    output reg  [7:0]  r,
    output reg  [7:0]  g,
    output reg  [7:0]  b,
    output reg         de,
    output reg         hs,
    output reg         vs
);
    localparam int H_TOT = H_ACT + H_FP + H_SYNC + H_BP;
    localparam int V_TOT = V_ACT + V_FP + V_SYNC + V_BP;
    localparam int BURSTS = (SRC_W + 16 + 31) / 32;           // 14 пачек на строку

    // ================================================================ домен clk_mem
    // вспышка и размер — в домен пикселей через переключатель (значения меняются редко и держатся)
    reg [31:0] blend_h;
    reg        blend_tg;
    reg [1:0]  size_h;
    reg        size_tg;
    reg        have_frame;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            blend_h <= 32'd0; blend_tg <= 1'b0; size_h <= 2'd0; size_tg <= 1'b0; have_frame <= 1'b0;
        end else begin
            if (frame_pulse) begin
                blend_h    <= shown_blend;
                blend_tg   <= ~blend_tg;
                have_frame <= 1'b1;
            end
            if (size != size_h) begin
                size_h  <= size;
                size_tg <= ~size_tg;
            end
        end
    end

    // палитра: байты R, G, B подряд — собрать тройку и записать словом 24 бит
    reg [7:0]  pr, pg;
    reg [1:0]  pch;
    reg [7:0]  pidx;
    reg        pal_wr;
    reg [23:0] pal_wd;
    reg [7:0]  pal_wa;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            pch <= 2'd0; pidx <= 8'd0; pal_wr <= 1'b0;
        end else begin
            pal_wr <= 1'b0;
            if (pal_we) begin
                if (pal_addr == 10'd0) begin
                    pr <= pal_data; pch <= 2'd1; pidx <= 8'd0;
                end else if (pch == 2'd0) begin
                    pr <= pal_data; pch <= 2'd1;
                end else if (pch == 2'd1) begin
                    pg <= pal_data; pch <= 2'd2;
                end else begin
                    pal_wa <= pidx;
                    pal_wd <= {pr, pg, pal_data};
                    pal_wr <= 1'b1;
                    pidx   <= pidx + 1'b1;
                    pch    <= 2'd0;
                end
            end
        end
    end

    // запрос строки из домена пикселей
    reg        rq_tg;                   // домен пикселей
    reg [7:0]  rq_row;
    reg        rq_bank, rq_first;
    (* ASYNC_REG = "TRUE" *) reg [2:0] rq_s;
    reg [22:0] act_fb;                  // кадр, который сейчас выводится
    reg [22:0] fa;                      // адрес следующей пачки
    reg [3:0]  nreq;                    // пачек ещё запросить
    reg [6:0]  wcnt;                    // слов принято
    reg        fbank;
    reg        lb_we;
    reg [7:0]  lb_wa;
    reg [31:0] lb_wd;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rq_s <= 3'd0; b_req <= 1'b0; nreq <= 4'd0; swap_ack <= 1'b0; act_fb <= 23'd0; lb_we <= 1'b0;
            wcnt <= 7'd0;
        end else begin
            rq_s     <= {rq_s[1:0], rq_tg};
            swap_ack <= 1'b0;
            lb_we    <= 1'b0;
            if (rq_s[2] ^ rq_s[1]) begin
                // поля запроса держатся строку — уже устойчивы
                begin : start
                    reg [22:0] fbn, la;
                    fbn = rq_first ? shown_fb : act_fb;
                    if (rq_first) begin
                        act_fb   <= shown_fb;
                        swap_ack <= 1'b1;
                    end
                    la = fbn + 23'(rq_row) * 23'(SRC_S);
                    fa <= {la[22:5], 5'd0};
                end
                fbank <= rq_bank;
                nreq  <= 4'(BURSTS);
                wcnt  <= 7'd0;
            end else if (nreq != 0 && !b_req) begin
                b_req  <= 1'b1;
                b_addr <= fa[22:2];
            end else if (b_req && b_ack) begin
                b_req <= 1'b0;
                fa    <= fa + 23'd32;
                nreq  <= nreq - 1'b1;
            end
            if (b_rd_valid) begin
                lb_we <= 1'b1;
                lb_wa <= {fbank, wcnt};
                lb_wd <= b_rd_data;
                wcnt  <= wcnt + 1'b1;
            end
        end
    end

    // ================================================================ память на два такта
    reg [31:0] lbuf [0:255];            // буфер строки: {банк, слово[6:0]}
    reg [23:0] pal  [0:255];
    always @(posedge clk) begin
        if (lb_we)  lbuf[lb_wa] <= lb_wd;
        if (pal_wr) pal[pal_wa] <= pal_wd;
    end

    // ================================================================ домен пикселей
    reg [10:0] hc;
    reg [9:0]  vc;
    (* ASYNC_REG = "TRUE" *) reg [2:0] bl_s, sz_s, hf_s;
    reg [31:0] blend_p;
    reg [1:0]  size_p, size_f;          // size_f — размер этого кадра
    reg [10:0] wx, ww;
    reg [9:0]  wy, wh;
    reg [7:0]  sy;                      // строка источника текущей строки вывода
    reg [10:0] ay;
    reg [8:0]  sx;
    reg [10:0] ax;
    reg        in_y;

    wire [9:0] nl = (vc == V_TOT - 1) ? 10'd0 : vc + 1'b1;     // следующая строка вывода
    wire       h_win = hc >= wx && hc < wx + ww;

    // окно по размеру (ширина кратна 4 от 424 · k, высота — 240 · k)
    always @* begin
        case (size_f)
            2'd1:    begin ww = 11'd1208; wh = 10'd684; end
            2'd2:    begin ww = 11'd1144; wh = 10'd648; end
            default: begin ww = 11'd1272; wh = 10'd720; end
        endcase
        wx = 11'((H_ACT - ww) / 2);
        wy = 10'((V_ACT - wh) / 2);
    end

    // конвейер точки: 0 — адрес буфера строки, 1 — слово, 2 — палитра, 3 — вспышка
    reg [3:0]  d_de, d_hs, d_vs, d_win;
    reg [1:0]  d_byte0, d_byte1;
    reg [31:0] lb_q;
    reg [7:0]  lb_ra;
    reg [23:0] pal_q;

    always @(posedge pixel_clk or negedge prst_n) begin
        if (!prst_n) begin
            hc <= 11'd0; vc <= 10'd0; rq_tg <= 1'b0; bl_s <= 3'd0; sz_s <= 3'd0; hf_s <= 3'd0;
            blend_p <= 32'd0; size_p <= 2'd0; size_f <= 2'd0; d_de <= 4'd0; d_hs <= 4'd0; d_vs <= 4'd0;
            d_win <= 4'd0; sy <= 8'd0; ay <= 11'd0; in_y <= 1'b0;
        end else begin
            bl_s <= {bl_s[1:0], blend_tg};
            sz_s <= {sz_s[1:0], size_tg};
            hf_s <= {hf_s[1:0], have_frame};
            if (bl_s[2] ^ bl_s[1]) blend_p <= blend_h;
            if (sz_s[2] ^ sz_s[1]) size_p <= size_h;

            // развёртка
            if (hc == H_TOT - 1) begin
                hc <= 11'd0;
                vc <= (vc == V_TOT - 1) ? 10'd0 : vc + 1'b1;
            end else
                hc <= hc + 1'b1;

            // начало строки: строка источника для этой строки (sy) и запрос на следующую (nl)
            if (hc == 0) begin : vrow
                // DDA по y: строка r окна → floor(r · 240 / wh); шаг: +240, перенос через wh
                reg [7:0]  s1;
                reg [10:0] a1;
                if (vc == V_TOT - 1)
                    size_f <= size_p;                         // размер меняется только между кадрами
                if (vc == wy) begin
                    s1 = 8'd0; a1 = 11'd0;
                end else if (ay + SRC_H >= wh) begin
                    s1 = sy + 1'b1; a1 = ay + 11'(SRC_H) - 11'(wh);
                end else begin
                    s1 = sy; a1 = ay + 11'(SRC_H);
                end
                sy <= s1;
                ay <= a1;
                if (vc == wy)
                    in_y <= 1'b1;
                else if (vc == wy + wh)
                    in_y <= 1'b0;
            end
            // запрос строки источника для следующей строки вывода (nl) — тактом позже, от уже защёлкнутых
            // sy, ay: «шаг DDA → ещё шаг → запрос» в одном такте не укладывался в 74,25 МГц (сборка этапа B
            // 23.11, −0,16 нс). На строку вывода — 1650 тактов, запросу хватает и второго.
            if (hc == 1) begin : vreq
                reg [7:0]  s2;
                s2 = (ay + SRC_H >= wh) ? sy + 1'b1 : sy;     // строка источника для nl
                if (nl == wy) begin
                    rq_row <= 8'd0; rq_bank <= nl[0]; rq_first <= 1'b1; rq_tg <= ~rq_tg;
                end else if (nl > wy && nl < wy + wh) begin
                    rq_row   <= s2;
                    rq_bank  <= nl[0];
                    rq_first <= 1'b0;
                    rq_tg    <= ~rq_tg;
                end
            end

            // DDA по x: точка c окна → floor(c · 424 / ww); sx готовится на такт раньше своей точки
            if (hc + 1'b1 == wx) begin
                sx <= 9'd0; ax <= 11'd0;
            end else if (h_win) begin
                if (ax + SRC_W >= ww) begin
                    ax <= ax + 11'(SRC_W) - ww;
                    sx <= sx + 1'b1;
                end else
                    ax <= ax + 11'(SRC_W);
            end

            // ступень 0: адрес слова в буфере строки (сдвиг 16 байт у нечётной строки источника)
            begin : s0
                reg [8:0] bi;
                bi = sx + (sy[0] ? 9'd16 : 9'd0);
                lb_ra     <= {vc[0], bi[8:2]};
                d_byte0   <= bi[1:0];
            end
            d_de  <= {d_de[2:0],  hc < H_ACT && vc < V_ACT};
            d_hs  <= {d_hs[2:0],  hc >= H_ACT + H_FP && hc < H_ACT + H_FP + H_SYNC};
            d_vs  <= {d_vs[2:0],  vc >= V_ACT + V_FP && vc < V_ACT + V_FP + V_SYNC};
            d_win <= {d_win[2:0], h_win && in_y && hc < H_ACT && hf_s[2]};
            // ступень 1: слово
            lb_q    <= lbuf[lb_ra];
            d_byte1 <= d_byte0;
            // ступень 2: индекс → палитра
            pal_q <= pal[8'(lb_q >> {d_byte1, 3'b000})];
            // ступень 3: вспышка v + ((цвет − v) · доля) >> 8 и выход
            begin : s3
                reg signed [17:0] dr, dg, db;
                dr = ($signed({10'd0, blend_p[7:0]})   - $signed({10'd0, pal_q[23:16]})) * $signed({10'd0, blend_p[31:24]});
                dg = ($signed({10'd0, blend_p[15:8]})  - $signed({10'd0, pal_q[15:8]}))  * $signed({10'd0, blend_p[31:24]});
                db = ($signed({10'd0, blend_p[23:16]}) - $signed({10'd0, pal_q[7:0]}))   * $signed({10'd0, blend_p[31:24]});
                r  <= d_win[2] ? 8'($signed({10'd0, pal_q[23:16]}) + (dr >>> 8)) : 8'd0;
                g  <= d_win[2] ? 8'($signed({10'd0, pal_q[15:8]})  + (dg >>> 8)) : 8'd0;
                b  <= d_win[2] ? 8'($signed({10'd0, pal_q[7:0]})   + (db >>> 8)) : 8'd0;
            end
            de <= d_de[2];
            hs <= d_hs[2];
            vs <= d_vs[2];
        end
    end
endmodule
