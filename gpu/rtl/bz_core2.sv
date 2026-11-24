// =====================================================================================================
// bz_core2.sv — ядро этапа B: разбор команд, а рисуют растеризатор (bz_raster) и конвейер точки (bz_pix).
// =====================================================================================================
//
// ЧЕМ ОТЛИЧАЕТСЯ ОТ ЭТАПА A (bz_core.sv)
//   Ядро этапа A всё делало само, точка за точкой, ~25 тактов на точку, — это образец для сверки. Здесь
//   ядро — «диспетчер»: разбирает поток команд (gpu/SPEC.md) и раздаёт работу.
//     POLY, TRIS  — многоугольник уходит в растеризатор (bz_raster), его точки — в конвейер точки (bz_pix);
//                   у треугольника моделей ядро сначала само считает градиенты (постановка, как в этапе A);
//     FILL, POINT, PIC — ядро само перебирает их точки (прямоугольник) и подаёт в тот же конвейер точки:
//                   z и цвет пишутся пачками, тексели картинки идут через кэш текстур;
//     TEX, LMAP, TIME, SCISSOR — только запоминаются: многоугольник забирает их в банк конвейера точки;
//     WRITE, SETUP, FRAME — работают с памятью или с кадром целиком, поэтому сначала ждут, пока всё
//                   нарисованное до них дойдёт до SDRAM («слив», ниже).
//
// БАНКИ КОНВЕЙЕРА ТОЧКИ
//   У конвейера точки два банка состояния (режим, текстура, lightmap). Ядро пишет банк pb (ps_we) в тот
//   же такт, когда отдаёт многоугольник растеризатору; точки несут номер банка; когда метка конца
//   выходит из конвейера, приходит rel — банк снова свободен (pbusy). Пока банк занят, следующий
//   многоугольник ждёт: так в конвейере не больше двух многоугольников с разным состоянием.
//
// LIGHTMAP
//   Перед многоугольником с lightmap конвейер должен держать этот lightmap у себя (bz_pix грузит его
//   целиком в слот). После команды LMAP (и после WRITE — память могла измениться) ядро грузит lightmap в
//   свободный слот — тот, которым не пользуются многоугольники в конвейере, — и только потом отдаёт
//   многоугольник.
//
// СЛИВ И «ЗАБОР»
//   Слив (S_DRAIN): ждать, пока растеризатор и конвейер пусты, и попросить конвейер записать куски z и
//   цвета в SDRAM (flush). Нужен перед WRITE (может переписать текстуру, которую ещё читают), SETUP (меняет
//   адреса кадра и таблицы), FRAME (кадр должен быть дорисован).
//   Забор (fence): ядро пишет память через свой переходник (bz_memif), конвейер читает текстуры своей
//   быстрой очередью моста — она может обогнать записи ядра. Поэтому после WRITE и SETUP перед первой
//   точкой ядро читает слово мимо кэша (ответ придёт, только когда все его записи сделаны) и сбрасывает
//   кэши конвейера (inv).
module bz_core2 (
    input  wire        clk,
    input  wire        rst,
    // ---- поток команд
    input  wire [31:0] cmd_data,
    input  wire        cmd_valid,
    output wire        cmd_ready,
    // ---- память ядра (через bz_memif): req держится до ack
    output reg         mem_req,
    output reg         mem_we,
    output reg  [22:0] mem_addr,
    output reg  [31:0] mem_wdata,
    output reg  [3:0]  mem_be,
    output reg         mem_nocache,     // чтение мимо кэша (забор)
    input  wire        mem_ack,
    input  wire [31:0] mem_rdata,
    output reg         mem_inv,         // такт: кэш переходника забыть
    // ---- вывод кадра
    output reg  [22:0] shown_fb,
    output reg  [31:0] shown_blend,
    output reg         frame_pulse,
    input  wire        swap_ack,
    output reg         bad_cmd,
    output reg  [31:0] mark,            // SPEC 0.4: число последней выполненной команды MARK
    output reg         pal_we,
    output reg  [9:0]  pal_addr,
    output reg  [7:0]  pal_data,
    // ---- растеризатор
    output wire        r_valid,
    input  wire        r_ready,
    output wire [4:0]  r_n,
    output wire [15:0] r_mode,
    output reg         r_ld_we,         // порт загрузки растеризатора: слово многоугольника (пока r_ready)
    output reg  [4:0]  r_ld_idx,        //   0…15 — вершина {y, x}, 16 + j — градиент j
    output reg  [31:0] r_ld_data,
    output wire [15:0] r_sx0, r_sy0, r_sx1, r_sy1,
    output wire        r_tag,
    input  wire        r_idle,
    // ---- точки ядра в конвейер (FILL, POINT, PIC); g_sel = 1 — вход конвейера у ядра, не у растеризатора
    output wire        g_sel,
    output wire        g_valid,
    output wire        g_end,
    output wire [15:0] g_x, g_y,
    output wire [31:0] g_zi, g_s, g_t,
    output wire        g_tag,
    input  wire        p_ready,
    // ---- конвейер точки: банк состояния
    output wire        ps_we,
    output wire        ps_tag,
    output wire [11:0] ps_mode,
    output wire [15:0] ps_k16,
    output wire [22:0] ps_taddr,
    output wire        ps_tlin,
    output wire [15:0] ps_ta,
    output wire [15:0] ps_tb,
    output wire [15:0] ps_tls,
    output wire        ps_lslot,
    output wire [6:0]  ps_lw,
    output wire [6:0]  ps_lh,
    output wire [3:0]  ps_lsh,
    output wire [31:0] ps_lsmin,
    output wire [31:0] ps_ltmin,
    output wire [6:0]  ps_phase,
    input  wire        p_rel,
    input  wire        p_rel_tag,
    input  wire        p_busy,
    // ---- конвейер точки: общие настройки, таблицы, lightmap, обслуживание
    output wire [22:0] fb_draw_o,
    output wire [22:0] zaddr_o,
    output wire [22:0] amap_o,
    output wire [15:0] scr_s_o,
    output reg         cmap_we,
    output reg  [11:0] cmap_waddr,
    output reg  [31:0] cmap_wdata,
    output reg         turb_we,
    output reg  [6:0]  turb_addr,
    output reg  [31:0] turb_data,
    output wire        lm_go,
    output wire        lm_slot,
    output wire [22:0] lm_addr,
    output wire [6:0]  lm_w,
    output wire [6:0]  lm_h,
    input  wire        lm_busy,
    output wire        p_flush,
    output reg         p_inv,
    output wire        idle
);
    // ------------------------------------------------------------------ коды команд
    localparam [7:0] OP_TEX = 8'h01, OP_LMAP = 8'h02, OP_POLY = 8'h03, OP_POINT = 8'h04,
                     OP_PIC = 8'h05, OP_FILL = 8'h06, OP_FRAME = 8'h07, OP_TIME = 8'h08,
                     OP_WRITE = 8'h09, OP_SETUP = 8'h0A, OP_SCISSOR = 8'h0B,
                     OP_VBUF = 8'h0C, OP_STBUF = 8'h0D, OP_TRIS = 8'h0E,
                     OP_MARK = 8'h0F;     // SPEC 0.4 (24.11.2026)

    // ------------------------------------------------------------------ состояния
    localparam [5:0]
        S_IDLE = 0, S_GET = 1, S_EXEC = 2, S_MEM = 3, S_WSTREAM = 4, S_FWAIT = 5,
        S_VB = 6, S_STB = 7, S_TRI0 = 8, S_TRI1 = 9, S_TL = 10, S_TS1 = 11, S_TS2 = 12, S_TS2B = 13,
        S_TS3 = 14, S_TS3W = 15, S_TS4 = 16, S_TS4W = 17, S_TS5 = 18,
        S_SETUP0 = 19, S_SETUP_CM = 20, S_SETUP_PAL = 21, S_SETUP_PALB = 22, S_SETUP_TURB = 23,
        S_DRAIN = 24, S_DRAIN_W = 25, S_PREP = 26, S_PREP_I = 27, S_PREP_W = 28,
        S_P_SUB = 29, S_LM = 30, S_LM_W = 31,
        S_G_MUL = 32, S_G_ALLOC = 33, S_G_PIX = 34, S_G_END = 35, S_FRAME2 = 36, S_SETUP_END = 37,
        S_TS2C = 38, S_TS3C = 39, S_TS4C = 40, S_TSC = 41, S_TSD = 42, S_G_MUL2 = 43,
        S_TS2D = 44, S_TSA = 45, S_TS3D = 46, S_TS3E = 47, S_TS4D = 48, S_TS4E = 49, S_TSC2 = 50, S_G_MUL3 = 51,
        S_TS2X = 52, S_TS3X = 53, S_TS4X = 54, S_TSCX = 55, S_G_MULX = 56, S_G_CHK = 57,
        S_MARK2 = 58;

    reg [5:0]  state, mret, d_next, p_next;
    // слова текущей команды (до 9: SETUP); у POLY слова идут сразу в вершины и градиенты (ниже), а не
    // сюда: выбор градиента из буфера по номеру 1 + n + i — двенадцать мультиплексоров 32 → 1 по 32 бита
    // (первая сборка этапа B — тысячи LUT)
    (* syn_ramstyle = "registers" *) reg [31:0] cw [0:8];
    reg [5:0]  nget, need;
    reg [23:0] wcount;                  // WRITE: сколько слов осталось
    reg [31:0] waddr;
    reg [31:0] mrd;                     // прочитанное слово

    reg [9:0]  vb_n, st_n;              // VBUF/STBUF: сколько записей осталось принять
    reg [8:0]  vb_i, st_i;
    reg [1:0]  vb_k;                    // слово вершины: 0 — {x, y}, 1 — zi, 2 — свет
    reg [7:0]  tri_n;                   // TRIS: треугольников осталось
    reg        in_tris;
    // (слово POLY уходит прямо в растеризатор — берётся, только когда тот готов его принять)
    assign cmd_ready = (state == S_IDLE) || (state == S_GET && (op != OP_POLY || r_ready)) ||
                       (state == S_WSTREAM && wcount != 0) ||
                       (state == S_VB && vb_n != 0) || (state == S_STB && st_n != 0) ||
                       (state == S_TRI0 && tri_n != 0) || (state == S_TRI1);
    assign idle = (state == S_IDLE) && !cmd_valid;

    // ------------------------------------------------------------------ настройки
    reg [22:0] fb0, fb1, zaddr, amap;   // адреса SDRAM 8 МБ — 23 бита
    reg [15:0] scr_w, scr_h;
    wire [15:0] scr_s = (scr_w + 16'd15) & ~16'd15;   // шаг строки: W вверх до 16
    reg        draw;
    wire [22:0] fb_draw = draw ? fb1 : fb0;
    assign fb_draw_o = fb_draw;
    assign zaddr_o   = zaddr;
    assign amap_o    = amap;
    assign scr_s_o   = scr_s;
    reg [1:0]  pal_sub;
    reg [6:0]  phase;
    reg [31:0] src_addr;
    reg [12:0] cpy_i;

    reg [22:0] t_addr;  reg t_lin;  reg [3:0] t_wl, t_hl;  reg [15:0] t_lw, t_lh, t_ls;
    reg [22:0] l_addr;  reg [15:0] l_stride;  reg [7:0] l_rows;  reg [3:0] l_shift;
    reg [31:0] l_smin, l_tmin;
    reg signed [15:0] sx0, sy0, sx1, sy1;     // ножницы: экран меньше 2^15

    // ------------------------------------------------------------------ многоугольник (для растеризатора)
    // Вершины и градиенты многоугольника ядро не хранит: слова POLY сразу уходят в растеризатор через его
    // порт загрузки (растеризатор берёт их, пока свободен: R1 кончил прежний многоугольник, банк градиентов
    // свободен). Раньше у ядра и у растеризатора было по копии (~900 триггеров у каждого) и шина в 896
    // проводов между ними — первая сборка этапа B не развелась (CLS 97 %).
    reg [4:0]  pn;
    reg [15:0] pmode;
    // градиенты треугольника, пока идёт постановка: tg[3·k + j], k — величина (0 zi, 1 u, 2 v, 3 l), j — C, A, B
    (* syn_ramstyle = "distributed_ram" *) reg [31:0] tg [0:15];
    reg [3:0]  tk;                      // слово треугольника, которое уходит в растеризатор (S_TS5)
    assign r_n    = pn;
    assign r_mode = pmode;
    assign r_sx0  = sx0;
    assign r_sy0  = sy0;
    assign r_sx1  = sx1;
    assign r_sy1  = sy1;

    // режим POLY (SPEC.md) → режим точки bz_pix: небо — z-тест с z = 0 (ZRAW, значение 0) и без записи z
    function [11:0] pix_mode (input [15:0] m);
        reg sky;
        begin
            sky = m[5];
            pix_mode = {1'b0, 1'b0, m[9], m[8:7], m[6], sky, !sky & m[10], !sky & m[4], sky | m[3], m[2:1]};
        end
    endfunction

    // ------------------------------------------------------------------ банки конвейера точки
    reg        pb;                      // следующий банк
    reg        pbusy [0:1];             // банк занят многоугольником в конвейере
    reg        b_lm [0:1], b_slot [0:1]; // многоугольник банка берёт lightmap из слота b_slot
    reg        lm_cur, lm_ok;           // слот текущего lightmap; он загружен и годен
    reg        need_fence;              // после WRITE/SETUP: забор и сброс кэшей перед точками
    wire       uses_lm = pmode[2:1] == 2'd0;
    wire       lm_ns = ~lm_cur;         // слот, в который грузится следующий lightmap
    wire       slot_busy = (pbusy[0] && b_lm[0] && b_slot[0] == lm_ns) || (pbusy[1] && b_lm[1] && b_slot[1] == lm_ns);
    assign lm_go   = state == S_LM && !slot_busy && !lm_busy;
    assign lm_slot = lm_ns;
    assign lm_addr = l_addr;
    assign lm_w    = l_stride[6:0];
    assign lm_h    = l_rows[6:0];

    // многоугольник уходит в растеризатор — тем же тактом банк конвейера точки получает его состояние
    assign r_valid = state == S_P_SUB && !pbusy[pb] && !(uses_lm && !lm_ok);
    assign r_tag   = pb;

    // ------------------------------------------------------------------ точки ядра (FILL, POINT, PIC)
    reg [15:0] gx0, gx1, gy1, gcx, gcy;  // прямоугольник [gx0, gx1) × [.., gy1), текущая точка
    reg        gx1s, gy1s;               // бит 16 (знак) x1 и y1 — для проверки «пуст ли» в S_G_CHK
    reg signed [15:0] gox, goy;          // PIC: левый верх картинки (i = x − gox, j = y − goy)
    reg [31:0] g_ss, g_ts, g_s0, gs, gt, gzi;
    reg        g_fade, gtag;
    reg [11:0] g_pmode;
    reg [15:0] g_k16;
    reg [22:0] g_taddr;
    reg [15:0] g_tls;
    wire       g_alloc = state == S_G_ALLOC && r_idle && !pbusy[pb];
    // заливка с затемнением (как Draw_FadeScreen): пишутся точки, где (x & 3) ≠ (y & 1)·2
    wire       g_pred  = !g_fade || (gcx[1:0] != {gcy[0], 1'b0});
    assign g_sel   = state == S_G_PIX || state == S_G_END;
    assign g_valid = (state == S_G_PIX && g_pred) || state == S_G_END;
    assign g_end   = state == S_G_END;
    assign g_x     = gcx;
    assign g_y     = gcy;
    assign g_zi    = gzi;
    assign g_s     = gs;
    assign g_t     = gt;
    assign g_tag   = gtag;

    // состояние для банка: многоугольник (S_P_SUB) или команда ядра (S_G_ALLOC)
    wire gsel = state == S_G_ALLOC;
    assign ps_we    = (r_valid && r_ready) || g_alloc;
    assign ps_tag   = pb;
    assign ps_mode  = gsel ? g_pmode : pix_mode (pmode);
    assign ps_k16   = gsel ? g_k16 : 16'd0;
    assign ps_taddr = gsel ? g_taddr : t_addr;
    assign ps_tlin  = gsel ? 1'b1 : t_lin;
    assign ps_ta    = gsel ? 16'hFFFF : t_lin ? t_lw : {8'd0, t_hl, t_wl};
    assign ps_tb    = gsel ? 16'hFFFF : t_lh;
    assign ps_tls   = gsel ? g_tls : t_ls;
    assign ps_lslot = lm_cur;
    assign ps_lw    = l_stride[6:0];
    assign ps_lh    = l_rows[6:0];
    assign ps_lsh   = l_shift;
    assign ps_lsmin = l_smin;
    assign ps_ltmin = l_tmin;
    assign ps_phase = phase;

    // слив: растеризатор и конвейер пусты, банки свободны — тогда «куски в SDRAM»
    wire drained = r_idle && !pbusy[0] && !pbusy[1] && !p_busy;
    assign p_flush = state == S_DRAIN && drained;

    // ------------------------------------------------------------------ треугольники моделей
    // Буферы вершин (VBUF) и точек шкурки (STBUF) — BSRAM на 512 записей; треугольник читает свои три
    // вершины по одной за такт.
    reg [15:0] tri_mode;
    reg [31:0] tw0, tw1;
    reg [1:0]  tli;                     // какая вершина читается (S_TL)
    wire [8:0] v_ra = tw0[9 * tli +: 9];
    wire [8:0] s_ra = tw1[9 * tli +: 9];
    wire [31:0] vb_xy, vb_zi, vb_l, st_q;
    wire        vb_go = state == S_VB && vb_n != 0 && cmd_valid;
    bz_ram #(.AW(9), .DW(32)) u_vxy (.clk(clk), .we(vb_go && vb_k == 2'd0), .wa(vb_i), .wd(cmd_data),
                                     .re(1'b1), .ra(v_ra), .rd(vb_xy));
    bz_ram #(.AW(9), .DW(32)) u_vzi (.clk(clk), .we(vb_go && vb_k == 2'd1), .wa(vb_i), .wd(cmd_data),
                                     .re(1'b1), .ra(v_ra), .rd(vb_zi));
    bz_ram #(.AW(9), .DW(32)) u_vl  (.clk(clk), .we(vb_go && vb_k == 2'd2), .wa(vb_i), .wd(cmd_data),
                                     .re(1'b1), .ra(v_ra), .rd(vb_l));
    bz_ram #(.AW(9), .DW(32)) u_st  (.clk(clk), .we(state == S_STB && st_n != 0 && cmd_valid), .wa(st_i),
                                     .wd(cmd_data), .re(1'b1), .ra(s_ra), .rd(st_q));

    reg signed [15:0] tu [0:2], tv [0:2];        // вершины треугольника: целые точки экрана
    (* syn_ramstyle = "registers" *) reg [31:0] tf [0:11];   // величины вершин: [3·k + i], k — zi, s, t, l
    reg signed [32:0] df1r, df2r;       // разности величины ka: f1 − f0, f2 − f0
    reg signed [16:0] du1, dv1, du2, dv2;
    (* syn_dspstyle = "logic" *) reg signed [35:0] td;   // d = du1·dv2 − du2·dv1 — удвоенная площадь со знаком
    reg [1:0]  ka;                      // величина (0 zi, 1 s, 2 t, 3 l)
    reg [31:0] ga, gb;                  // её A и B
    reg        tneg;

    // делитель постановки: radix-4 с полной длиной частного (48 бит — 24 такта, без сдвигателя)
    reg         div_start;
    reg  [47:0] div_num;
    reg  [31:0] div_den;
    wire        div_done;
    wire [47:0] div_q;
    bz_div4 #(.W(48), .WD(32), .FIXQ(48)) u_div (.clk(clk), .rst(rst), .start(div_start), .num(div_num),
                                                 .den(div_den), .busy(), .done(div_done), .quot(div_q), .rem());

    wire [31:0] q_sgn  = tneg ? 32'(-div_q) : div_q[31:0];

    // ОБЩИЙ УМНОЖИТЕЛЬ. Постановке треугольника нужно 8 произведений на величину (d, числители A и B,
    // значение C), точкам PIC — два на команду. Каждое нужно раз в десятки тактов, поэтому умножитель один
    // (36 × 18 бит со знаком — один блок DSP), а произведения идут по тактам автомата: в каждом состоянии
    // свои множители (mul_x, mul_y — только из регистров), произведение — в регистре mul_pr к следующему
    // такту, сумма копится в mul_acc. Раньше это было ~10 блоков DSP из 24 (их почти все съедает
    // растеризатор), а цепочка «выбор величины → разность → умножение → разность → делимое» за один такт
    // давала 40 МГц при нужных 60,75 (пробная сборка Gowin) — теперь каждое звено в своём такте.
    // Множители тоже идут через регистры (mul_x, mul_y — входные регистры блока DSP): без них выбор
    // множителя по состоянию автомата шёл прямо в умножитель, а у блока DSP без входного регистра
    // «setup» ~4 нс — сборка этапа B 23.11 не уложилась в такт. Поэтому произведение множителей,
    // выбранных в состоянии S, — в mul_pr через ДВА такта: после каждой пары состояний с
    // множителями стоит такт ожидания (S_TS2X, S_TS3X, S_TS4X, S_TSCX, S_G_MULX).
    reg  signed [35:0] mul_xs;
    reg  signed [17:0] mul_ys;
    always @* begin
        mul_xs = 36'sd0;
        mul_ys = 18'sd0;
        case (state)
            S_TS2:  begin mul_xs = 36'(du1);  mul_ys = 18'(dv2); end
            S_TS2C: begin mul_xs = 36'(du2);  mul_ys = 18'(dv1); end
            S_TS3:  begin mul_xs = 36'(df1r); mul_ys = 18'(dv2); end
            S_TS3C: begin mul_xs = 36'(df2r); mul_ys = 18'(dv1); end
            S_TS4:  begin mul_xs = 36'(df2r); mul_ys = 18'(du1); end
            S_TS4C: begin mul_xs = 36'(df1r); mul_ys = 18'(du2); end
            S_TSC:  begin mul_xs = 36'($signed(ga));    mul_ys = 18'(tu[0]); end   // нужны младшие 32 бита
            S_TSC2: begin mul_xs = 36'($signed(gb));    mul_ys = 18'(tv[0]); end
            S_G_MUL:  begin mul_xs = 36'($signed(g_ss)); mul_ys = $signed({2'b00, gx0 - gox}); end
            S_G_MUL2: begin mul_xs = 36'($signed(g_ts)); mul_ys = $signed({2'b00, gcy - goy}); end
            default: ;
        endcase
    end
    reg  signed [35:0] mul_x;
    reg  signed [17:0] mul_y;
    reg  signed [53:0] mul_pr;
    // вычитания по 54 бита Gowin иначе кладёт в сумматоры блоков DSP (ALU54D) — а блоки нужны умножителям
    (* syn_dspstyle = "logic" *) reg signed [53:0] mul_acc, mul_dr;
    always @(posedge clk) begin
        mul_x  <= mul_xs;
        mul_y  <= mul_ys;
        mul_pr <= mul_x * mul_y;
    end
    // floor(n / d) при d > 0: n ≥ 0 — обычное деление; n < 0 — −ceil(|n| / d) = −((|n| + d − 1) / d)
    wire signed [53:0] td54  = 54'(td);
    (* syn_dspstyle = "logic" *) wire [47:0] n_div = (mul_dr < 0) ? 48'(-mul_dr + td54 - 1) : 48'(mul_dr);

    // ------------------------------------------------------------------ загрузка многоугольника в растеризатор
    // POLY: слово потока n ≤ pn — вершина n − 1, дальше — градиент. TRIS: градиенты по мере постановки —
    // в tg (A в S_TS3W, B в S_TS4W, C в S_TSD), потом S_TS5: три вершины (16·u + 8 — целая точка Quake —
    // центр точки адаптера) и 12 градиентов из tg — по слову за такт, пока растеризатор готов.
    wire [31:0] tri_c = tf[3 * ka] - mul_acc[31:0] - mul_pr[31:0];   // C = f0 − A·u0 − B·v0 (по модулю 2^32)
    reg         tg_we;
    reg  [3:0]  tg_wi;
    reg  [31:0] tg_wd;
    wire [1:0]  tv5 = 2'(tk);
    always @* begin
        tg_we = 1'b0; tg_wi = 4'd0; tg_wd = q_sgn;
        r_ld_we = 1'b0; r_ld_idx = 5'd0; r_ld_data = cmd_data;
        case (state)
            S_GET:
                if (cmd_valid && op == OP_POLY && r_ready) begin
                    r_ld_we  = 1'b1;
                    r_ld_idx = (nget <= {1'b0, pn}) ? 5'(nget - 6'd1) : 5'(6'd15 + nget - {1'b0, pn});
                end
            S_TS3W: if (div_done) begin tg_we = 1'b1; tg_wi = 4'(3 * ka + 1); end
            S_TS4W: if (div_done) begin tg_we = 1'b1; tg_wi = 4'(3 * ka + 2); end
            S_TSD:  begin tg_we = 1'b1; tg_wi = 4'(3 * ka); tg_wd = tri_c; end
            S_TS5:
                if (r_ready) begin
                    r_ld_we = 1'b1;
                    if (tk < 4'd3) begin
                        r_ld_idx  = 5'(tk);
                        r_ld_data = {16'(tv[tv5] * 16 + 8), 16'(tu[tv5] * 16 + 8)};
                    end else begin
                        r_ld_idx  = 5'(5'd13 + {1'b0, tk});          // 16 + (tk − 3)
                        r_ld_data = tg[4'(tk - 4'd3)];
                    end
                end
            default: ;
        endcase
    end
    always @(posedge clk)
        if (tg_we)
            tg[tg_wi] <= tg_wd;

    // ------------------------------------------------------------------ память ядра
    task automatic mreq (input we, input [31:0] a, input [31:0] wd, input [3:0] be, input [5:0] ret);
        begin
            mem_req     <= 1'b1;
            mem_we      <= we;
            mem_addr    <= a[22:0];
            mem_wdata   <= wd;
            mem_be      <= be;
            mem_nocache <= 1'b0;
            mret        <= ret;
            state       <= S_MEM;
        end
    endtask

    task automatic rd (input [31:0] a, input [5:0] ret);
        mreq (1'b0, a, 32'd0, 4'd0, ret);
    endtask

    wire [7:0] op = cw[0][31:24];
    // Прямоугольник команды: у POINT — квадрат size × size, обрезанный ножницами; у FILL и PIC — w × h,
    // обрезанный экраном. Одна схема на три команды (раньше — три копии сумматоров и сравнений). Всё — со
    // знаком в 17 битах: размер — беззнаковое поле, а в смеси со знаковым Verilog считал бы всё без знака,
    // и частица левее экрана обрезалась бы неверно. x0 ≥ 0 всегда (нижняя граница — 0 или ножницы ≥ 0).
    wire               rq_pt = op == OP_POINT;
    wire signed [16:0] rq_x  = 17'($signed(cw[1][15:0]));
    wire signed [16:0] rq_y  = 17'($signed(cw[1][31:16]));
    wire signed [16:0] rq_w  = rq_pt ? {13'd0, cw[0][11:8]} : {1'b0, cw[2][15:0]};
    wire signed [16:0] rq_h  = rq_pt ? {13'd0, cw[0][11:8]} : {1'b0, cw[2][31:16]};
    wire signed [16:0] rq_lx = rq_pt ? 17'(sx0) : 17'sd0;
    wire signed [16:0] rq_ly = rq_pt ? 17'(sy0) : 17'sd0;
    wire signed [16:0] rq_hx = rq_pt ? 17'(sx1) : $signed({1'b0, scr_w});
    wire signed [16:0] rq_hy = rq_pt ? 17'(sy1) : $signed({1'b0, scr_h});
    wire signed [16:0] rq_ex = rq_x + rq_w;
    wire signed [16:0] rq_ey = rq_y + rq_h;
    wire signed [16:0] rq_x0 = (rq_x < rq_lx) ? rq_lx : rq_x;
    wire signed [16:0] rq_y0 = (rq_y < rq_ly) ? rq_ly : rq_y;
    wire signed [16:0] rq_x1 = (rq_ex > rq_hx) ? rq_hx : rq_ex;
    wire signed [16:0] rq_y1 = (rq_ey > rq_hy) ? rq_hy : rq_ey;
    integer ii;

    always @(posedge clk) begin
        div_start   <= 1'b0;
        frame_pulse <= 1'b0;
        pal_we      <= 1'b0;
        cmap_we     <= 1'b0;
        turb_we     <= 1'b0;
        mem_inv     <= 1'b0;
        p_inv       <= 1'b0;
        if (p_rel)
            pbusy[p_rel_tag] <= 1'b0;
        if (rst) begin
            state   <= S_IDLE;
            mem_req <= 1'b0;
            draw    <= 1'b0;
            bad_cmd <= 1'b0;
            mark    <= 32'd0;
            nget    <= 0;
            shown_fb <= 0;
            shown_blend <= 0;
            phase   <= 0;
            sx0 <= 0; sy0 <= 0; sx1 <= 0; sy1 <= 0;
            vb_n <= 0; st_n <= 0; tri_n <= 0; in_tris <= 1'b0;
            pb <= 1'b0; pbusy[0] <= 1'b0; pbusy[1] <= 1'b0;
            lm_cur <= 1'b0; lm_ok <= 1'b0; need_fence <= 1'b0;
            l_stride <= 16'd1; l_rows <= 8'd1;
        end else begin
            case (state)
            // ---------------------------------------------------- приём команды
            S_IDLE:
                if (cmd_valid) begin
                    cw[0] <= cmd_data;
                    nget  <= 6'd1;
                    case (cmd_data[31:24])
                        OP_TEX:     need <= 3;
                        OP_LMAP:    need <= 5;
                        OP_POLY:    need <= 6'(1 + cmd_data[7:0] + 9 + ((cmd_data[10:9] == 2'd1) ? 3 : 0));
                        OP_POINT:   need <= 3;
                        OP_PIC:     need <= 7;
                        OP_FILL:    need <= 4;
                        OP_FRAME:   need <= 2;
                        OP_TIME:    need <= 2;
                        OP_MARK:    need <= 2;
                        OP_WRITE:   need <= 2;
                        OP_SETUP:   need <= 9;
                        OP_SCISSOR: need <= 3;
                        default:    need <= 0;
                    endcase
                    // (градиент света у POLY без света по вершинам не приходит — в банке растеризатора остаётся
                    // прежний; растеризатор посчитает по нему l, но конвейер точки берёт l только у света по
                    // вершинам)
                    if (cmd_data[31:24] == OP_POLY) begin
                        pn    <= cmd_data[4:0];
                        pmode <= cmd_data[23:8];
                    end
                    case (cmd_data[31:24])
                        OP_TEX, OP_LMAP, OP_POLY, OP_POINT, OP_PIC, OP_FILL, OP_FRAME, OP_TIME,
                        OP_WRITE, OP_SETUP, OP_SCISSOR, OP_MARK: state <= S_GET;
                        OP_VBUF:
                            if (cmd_data[9:0] != 0 && cmd_data[9:0] <= 10'd512) begin
                                vb_n <= cmd_data[9:0]; vb_i <= 9'd0; vb_k <= 2'd0; state <= S_VB;
                            end else
                                bad_cmd <= 1'b1;
                        OP_STBUF:
                            if (cmd_data[9:0] != 0 && cmd_data[9:0] <= 10'd512) begin
                                st_n <= cmd_data[9:0]; st_i <= 9'd0; state <= S_STB;
                            end else
                                bad_cmd <= 1'b1;
                        OP_TRIS: begin
                            tri_n    <= cmd_data[7:0];
                            tri_mode <= cmd_data[23:8];
                            in_tris  <= 1'b1;
                            state    <= S_TRI0;
                        end
                        default: bad_cmd <= 1'b1;
                    endcase
                end

            S_GET:
                if (cmd_valid && (op != OP_POLY || r_ready)) begin
                    if (nget < 6'd9)
                        cw[nget[3:0]] <= cmd_data;
                    // POLY: слово уходит в растеризатор (r_ld_* выше)
                    nget <= nget + 1'b1;
                    if (nget + 1'b1 == need)
                        state <= S_EXEC;
                end

            S_MEM:
                if (mem_ack) begin
                    mem_req <= 1'b0;
                    mrd     <= mem_rdata;
                    state   <= mret;
                end

            // ---------------------------------------------------- исполнение
            S_EXEC: begin
                state <= S_IDLE;
                case (op)
                OP_TEX: begin
                    t_addr <= cw[1][22:0];
                    t_lin  <= cw[0][16];
                    if (cw[0][16]) begin
                        t_lw <= (cw[0][15:0] == 0) ? 16'd1 : cw[0][15:0];
                        t_lh <= (cw[2][15:0] == 0) ? 16'd1 : cw[2][15:0];
                        t_ls <= cw[2][31:16];
                    end else begin
                        t_wl <= cw[2][3:0];
                        t_hl <= cw[2][7:4];
                    end
                end
                OP_LMAP: begin
                    l_addr   <= cw[1][22:0];
                    l_stride <= (cw[2][15:0] == 0) ? 16'd1 : cw[2][15:0];
                    l_rows   <= (cw[2][23:16] == 0) ? 8'd1 : cw[2][23:16];
                    l_shift  <= cw[2][27:24];
                    l_smin   <= cw[3];
                    l_tmin   <= cw[4];
                    lm_ok    <= 1'b0;
                    // слот lightmap — 64 × 64 клеток (SPEC.md); больше конвейер не держит
                    if (cw[2][15:0] > 16'd64 || cw[2][23:16] > 8'd64)
                        bad_cmd <= 1'b1;
                end
                OP_TIME: phase <= cw[1][6:0];
                // MARK (SPEC 0.4): «всё, что шло раньше, выполнено». WRITE и так кончается, когда мост принял
                // его последнее слово (а мост пишет и читает по порядку приёма); слив конвейера — чтобы и
                // рисование до MARK было закончено, как обещает SPEC. Потом число — в регистр MARK (LINK 0x90).
                OP_MARK: begin
                    d_next <= S_MARK2;
                    state  <= S_DRAIN;
                end
                OP_SCISSOR: begin
                    sx0 <= ($signed(cw[1][15:0]) < 0) ? 16'sd0 : $signed(cw[1][15:0]);
                    sy0 <= ($signed(cw[1][31:16]) < 0) ? 16'sd0 : $signed(cw[1][31:16]);
                    sx1 <= ($signed(cw[2][15:0]) > $signed({16'd0, scr_w})) ? scr_w : cw[2][15:0];
                    sy1 <= ($signed(cw[2][31:16]) > $signed({16'd0, scr_h})) ? scr_h : cw[2][31:16];
                end
                OP_FRAME: begin
                    d_next <= S_FRAME2;
                    state  <= S_DRAIN;
                end
                OP_WRITE: begin
                    wcount <= cw[0][23:0];
                    waddr  <= cw[1];
                    d_next <= S_WSTREAM;
                    state  <= S_DRAIN;
                end
                OP_SETUP: begin
                    d_next <= S_SETUP0;
                    state  <= S_DRAIN;
                end
                OP_POLY: begin                    // вершины и градиенты уже на месте (S_GET)
                    p_next <= S_P_SUB;
                    state  <= S_PREP;
                end
                // FILL, POINT, PIC: прямоугольник [x0, x1) × [y0, y1) — общей схемой (rq_* выше); пуст ли
                // он, проверяет S_G_CHK тактом позже
                OP_POINT: begin
                    // квадрат size × size от (x, y), обрезанный ножницами; z-тест ≥ и запись z на любой
                    // прозрачности (как R_DrawParticle Quake)
                    gx0 <= rq_x0[15:0]; gcy <= rq_y0[15:0]; gx1 <= rq_x1[15:0]; gy1 <= rq_y1[15:0];
                    gx1s <= rq_x1[16];  gy1s <= rq_y1[16];
                    gox <= rq_x[15:0];  goy <= rq_y[15:0];
                    g_ss <= 32'd0; g_ts <= 32'd0;
                    gzi  <= cw[2];
                    g_fade  <= 1'b0;
                    g_pmode <= {1'b0, 1'b1, 1'b0, cw[0][13:12], 1'b0, 1'b0, 1'b0, 1'b1, 1'b1, 2'd2};
                    g_k16   <= {8'd0, cw[0][7:0]};
                    state   <= S_G_CHK;
                end
                OP_PIC: begin
                    // картинка: точка (x + i, y + j) берёт тексель (i·ss >> 16, j·ts >> 16) из строк addr + k·stride
                    gx0 <= rq_x0[15:0]; gcy <= rq_y0[15:0]; gx1 <= rq_x1[15:0]; gy1 <= rq_y1[15:0];
                    gx1s <= rq_x1[16];  gy1s <= rq_y1[16];
                    gox <= rq_x[15:0];  goy <= rq_y[15:0];
                    g_ss <= cw[5]; g_ts <= cw[6];
                    gzi  <= 32'd0;
                    g_fade  <= 1'b0;
                    g_pmode <= {1'b0, 1'b0, cw[0][0], 2'd0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 2'd2};
                    g_k16   <= 16'd0;
                    g_taddr <= cw[3][22:0];
                    g_tls   <= cw[4][15:0];
                    state   <= S_G_CHK;
                end
                OP_FILL: begin
                    // прямоугольник, обрезанный экраном: z (FILL z — только запись z) или цвет
                    gx0 <= rq_x0[15:0]; gcy <= rq_y0[15:0]; gx1 <= rq_x1[15:0]; gy1 <= rq_y1[15:0];
                    gx1s <= rq_x1[16];  gy1s <= rq_y1[16];
                    gox <= rq_x0[15:0]; goy <= rq_y0[15:0];
                    g_ss <= 32'd0; g_ts <= 32'd0;
                    gzi  <= 32'd0;
                    g_fade <= !cw[0][0] && cw[0][1];
                    if (cw[0][0]) begin                // z: ZWRITE + ZRAW + NOCOL (+ SOLID — тексель не нужен)
                        g_pmode <= {1'b1, 1'b1, 1'b0, 2'd0, 1'b0, 1'b1, 1'b0, 1'b1, 1'b0, 2'd2};
                        g_k16   <= cw[3][15:0];
                    end else begin                    // цвет: SOLID (с затемнением — цвет 0)
                        g_pmode <= {1'b0, 1'b1, 1'b0, 2'd0, 1'b0, 1'b0, 1'b0, 1'b0, 1'b0, 2'd2};
                        g_k16   <= cw[0][1] ? 16'd0 : {8'd0, cw[3][7:0]};
                    end
                    state <= S_G_CHK;
                end
                default: ;
                endcase
            end

            // ---------------------------------------------------- VBUF, STBUF (запись в BSRAM — vb_go выше)
            S_VB:
                if (vb_n == 0)
                    state <= S_IDLE;
                else if (cmd_valid) begin
                    if (vb_k == 2'd2) begin
                        vb_k <= 2'd0;
                        vb_i <= vb_i + 1'b1;
                        vb_n <= vb_n - 1'b1;
                    end else
                        vb_k <= vb_k + 1'b1;
                end
            S_STB:
                if (st_n == 0)
                    state <= S_IDLE;
                else if (cmd_valid) begin
                    st_i <= st_i + 1'b1;
                    st_n <= st_n - 1'b1;
                end
            // ---------------------------------------------------- TRIS: два слова — и постановка
            S_TRI0:
                if (tri_n == 0) begin
                    in_tris <= 1'b0;
                    state   <= S_IDLE;
                end else if (cmd_valid) begin
                    tw0   <= cmd_data;
                    state <= S_TRI1;
                end
            S_TRI1:
                if (cmd_valid) begin
                    tw1   <= cmd_data;
                    tri_n <= tri_n - 1'b1;
                    tli   <= 2'd0;
                    state <= S_TL;
                end
            // три вершины из BSRAM: адрес вершины tli — в этом такте, её слова — в следующем
            S_TL: begin
                if (tli != 2'd0) begin : tri_load
                    reg [1:0] i;
                    i = tli - 2'd1;
                    tu[i]     <= vb_xy[15:0];
                    tv[i]     <= vb_xy[31:16];
                    tf[i]     <= vb_zi;
                    tf[3 + i] <= {st_q[15:0], 16'd0};
                    tf[6 + i] <= {st_q[31:16], 16'd0};
                    tf[9 + i] <= vb_l << 16;
                end
                tli <= tli + 1'b1;
                if (tli == 2'd3)
                    state <= S_TS1;
            end
            S_TS1: begin
                du1 <= 17'(tu[1]) - 17'(tu[0]); dv1 <= 17'(tv[1]) - 17'(tv[0]);
                du2 <= 17'(tu[2]) - 17'(tu[0]); dv2 <= 17'(tv[2]) - 17'(tv[0]);
                state <= S_TS2;
            end
            // d = du1·dv2 − du2·dv1 на общем умножителе: множители в S_TS2 и S_TS2C, произведения — через
            // два такта (в S_TS2X и S_TS2D)
            S_TS2:
                state <= S_TS2C;
            S_TS2C:
                state <= S_TS2X;
            S_TS2X: begin
                mul_acc <= mul_pr;                    // du1·dv2
                state   <= S_TS2D;
            end
            S_TS2D: begin
                td    <= 36'(mul_acc - mul_pr);       // − du2·dv1
                state <= S_TS2B;
            end
            S_TS2B:
                if (td <= 0)
                    state <= S_TRI0;      // спиной к нам или вырожден — не рисуется
                else begin
                    ka    <= 2'd0;
                    state <= S_TSA;
                end
            // величина ka: разности f1 − f0, f2 − f0
            S_TSA: begin
                df1r  <= $signed({tf[3 * ka + 1][31], tf[3 * ka + 1]}) - $signed({tf[3 * ka][31], tf[3 * ka]});
                df2r  <= $signed({tf[3 * ka + 2][31], tf[3 * ka + 2]}) - $signed({tf[3 * ka][31], tf[3 * ka]});
                state <= S_TS3;
            end
            // A: числитель df1·dv2 − df2·dv1, потом деление на d
            S_TS3:
                state <= S_TS3C;
            S_TS3C:
                state <= S_TS3X;
            S_TS3X: begin
                mul_acc <= mul_pr;
                state   <= S_TS3D;
            end
            S_TS3D: begin
                mul_dr <= mul_acc - mul_pr;
                state  <= S_TS3E;
            end
            S_TS3E: begin
                div_num   <= n_div;
                div_den   <= td[31:0];
                tneg      <= mul_dr < 0;
                div_start <= 1'b1;
                state     <= S_TS3W;
            end
            S_TS3W:
                if (div_done) begin
                    ga    <= q_sgn;
                    state <= S_TS4;
                end
            // B: du1·df2 − du2·df1
            S_TS4:
                state <= S_TS4C;
            S_TS4C:
                state <= S_TS4X;
            S_TS4X: begin
                mul_acc <= mul_pr;
                state   <= S_TS4D;
            end
            S_TS4D: begin
                mul_dr <= mul_acc - mul_pr;
                state  <= S_TS4E;
            end
            S_TS4E: begin
                div_num   <= n_div;
                div_den   <= td[31:0];
                tneg      <= mul_dr < 0;
                div_start <= 1'b1;
                state     <= S_TS4W;
            end
            S_TS4W:
                if (div_done) begin
                    gb    <= q_sgn;
                    state <= S_TSC;
                end
            // C = f0 − A·u0 − B·v0 по модулю 2^32
            S_TSC:
                state <= S_TSC2;
            S_TSC2:
                state <= S_TSCX;
            S_TSCX: begin
                mul_acc <= mul_pr;
                state   <= S_TSD;
            end
            S_TSD:                        // C — в tg (A и B ушли туда в S_TS3W, S_TS4W)
                if (ka == 2'd3) begin
                    tk    <= 4'd0;
                    state <= S_TS5;
                end else begin
                    ka    <= ka + 1'b1;
                    state <= S_TSA;
                end
            S_TS5:                        // 3 вершины и 12 градиентов — в растеризатор; дальше как POLY
                if (r_ready) begin
                    tk <= tk + 1'b1;
                    if (tk == 4'd14) begin
                        pn     <= 5'd3;
                        pmode  <= tri_mode;
                        p_next <= S_P_SUB;
                        state  <= S_PREP;
                    end
                end

            // ---------------------------------------------------- перед точками: забор после записей ядра
            S_PREP:
                if (need_fence) begin
                    rd (32'd0, S_PREP_I);
                    mem_nocache <= 1'b1;
                end else
                    state <= p_next;
            S_PREP_I: begin
                p_inv      <= 1'b1;               // кэши текстур и alphamap могли устареть
                need_fence <= 1'b0;
                state      <= S_PREP_W;
            end
            S_PREP_W:
                if (!p_busy)
                    state <= p_next;

            // ---------------------------------------------------- многоугольник → растеризатор
            S_P_SUB:
                if (uses_lm && !lm_ok)
                    state <= S_LM;
                else if (r_valid && r_ready) begin
                    pbusy[pb]  <= 1'b1;
                    b_lm[pb]   <= uses_lm;
                    b_slot[pb] <= lm_cur;
                    pb    <= ~pb;
                    state <= in_tris ? S_TRI0 : S_IDLE;
                end
            // lightmap — в свободный слот (lm_go выше), потом многоугольник
            S_LM:
                if (lm_go)
                    state <= S_LM_W;
            S_LM_W:
                if (!lm_busy) begin
                    lm_cur <= lm_ns;
                    lm_ok  <= 1'b1;
                    state  <= S_P_SUB;
                end

            // ---------------------------------------------------- точки ядра
            // прямоугольник пуст — команда кончилась (сравнение — тактом позже сумматоров S_EXEC: вместе
            // с ними и выбором состояния не укладывалось в такт, сборка этапа B 23.11)
            S_G_CHK:
                state <= ($signed({1'b0, gx0}) < $signed({gx1s, gx1}) &&
                          $signed({1'b0, gcy}) < $signed({gy1s, gy1})) ? S_G_MUL : S_IDLE;
            // PIC: s, t первой точки = (x0 − x)·ss, (y0 − y)·ts — дальше прибавлением; у FILL/POINT шаги 0
            S_G_MUL:
                state <= S_G_MUL2;
            S_G_MUL2:
                state <= S_G_MULX;
            S_G_MULX: begin
                g_s0  <= mul_pr[31:0];                    // (x0 − x)·ss
                state <= S_G_MUL3;
            end
            S_G_MUL3: begin
                gt     <= mul_pr[31:0];                   // (y0 − y)·ts
                p_next <= S_G_ALLOC;
                state  <= S_PREP;
            end
            S_G_ALLOC:
                if (g_alloc) begin
                    pbusy[pb]  <= 1'b1;
                    b_lm[pb]   <= 1'b0;
                    gtag  <= pb;
                    pb    <= ~pb;
                    gcx   <= gx0;
                    gs    <= g_s0;
                    state <= S_G_PIX;
                end
            S_G_PIX:
                if (!g_pred || p_ready) begin
                    if (gcx + 16'd1 == gx1) begin
                        gcx <= gx0;
                        gs  <= g_s0;
                        gt  <= gt + g_ts;
                        if (gcy + 16'd1 == gy1)
                            state <= S_G_END;
                        else
                            gcy <= gcy + 1'b1;
                    end else begin
                        gcx <= gcx + 1'b1;
                        gs  <= gs + g_ss;
                    end
                end
            S_G_END:
                if (p_ready)
                    state <= S_IDLE;

            // ---------------------------------------------------- слив: всё нарисованное — в SDRAM
            S_DRAIN:
                if (drained)
                    state <= S_DRAIN_W;           // p_flush — в этом такте (выше)
            S_DRAIN_W:
                if (!p_busy) begin
                    mem_inv <= 1'b1;               // кэш переходника мог держать старые z и цвет
                    state   <= d_next;
                end

            // ---------------------------------------------------- MARK
            S_MARK2: begin
                mark  <= cw[1];
                state <= S_IDLE;
            end

            // ---------------------------------------------------- FRAME
            S_FRAME2: begin
                shown_blend <= cw[1];
                shown_fb    <= fb_draw;
                draw        <= ~draw;
                frame_pulse <= 1'b1;
                state       <= S_FWAIT;      // в прежний показанный кадр — только после смены
            end
            S_FWAIT:
                if (swap_ack)
                    state <= S_IDLE;

            // ---------------------------------------------------- WRITE: данные потока — в память
            S_WSTREAM:
                if (wcount == 0) begin
                    need_fence <= 1'b1;
                    lm_ok      <= 1'b0;           // lightmap в памяти мог измениться
                    state      <= S_IDLE;
                end else if (cmd_valid) begin
                    wcount <= wcount - 1'b1;
                    waddr  <= waddr + 4;
                    mreq (1'b1, waddr, cmd_data, 4'b1111, S_WSTREAM);
                end

            // ---------------------------------------------------- SETUP: адреса и таблицы
            S_SETUP0: begin
                fb0   <= cw[1][22:0];
                fb1   <= cw[2][22:0];
                zaddr <= cw[3][22:0];
                scr_w <= cw[4][15:0];
                scr_h <= cw[4][31:16];
                amap  <= cw[6][22:0];
                draw  <= 1'b0;
                sx0 <= 0; sy0 <= 0;
                sx1 <= cw[4][15:0];
                sy1 <= cw[4][31:16];
                src_addr <= cw[5];
                cpy_i <= 0;
                state <= S_SETUP_CM;
            end
            // colormap: 4096 слов — в BSRAM конвейера точки, слово за словом
            S_SETUP_CM: begin
                if (cpy_i != 0) begin
                    cmap_we    <= 1'b1;
                    cmap_waddr <= 12'(cpy_i - 1'b1);
                    cmap_wdata <= mrd;
                end
                if (cpy_i == 13'd4096) begin
                    cpy_i <= 0;
                    src_addr <= cw[7];
                    state <= S_SETUP_PAL;
                end else begin
                    cpy_i <= cpy_i + 1'b1;
                    rd (src_addr + {17'd0, cpy_i, 2'b00}, S_SETUP_CM);
                end
            end
            // палитра — блоку вывода: слово читается, его 4 байта уходят по одному за такт
            S_SETUP_PAL:
                if (cpy_i == 13'd192) begin
                    cpy_i <= 0;
                    src_addr <= cw[8];
                    state <= S_SETUP_TURB;
                end else begin
                    pal_sub <= 2'd0;
                    rd (src_addr + {17'd0, cpy_i, 2'b00}, S_SETUP_PALB);
                end
            S_SETUP_PALB: begin
                pal_we   <= 1'b1;
                pal_addr <= {cpy_i[7:0], pal_sub};
                pal_data <= 8'(mrd >> {pal_sub, 3'b000});
                pal_sub  <= pal_sub + 1'b1;
                if (pal_sub == 2'd3) begin
                    cpy_i <= cpy_i + 1'b1;
                    state <= S_SETUP_PAL;
                end
            end
            S_SETUP_TURB: begin
                if (cpy_i != 0) begin
                    turb_we   <= 1'b1;
                    turb_addr <= 7'(cpy_i - 1'b1);
                    turb_data <= mrd;
                end
                if (cpy_i == 13'd128)
                    state <= S_SETUP_END;
                else begin
                    cpy_i <= cpy_i + 1'b1;
                    rd (src_addr + {17'd0, cpy_i, 2'b00}, S_SETUP_TURB);
                end
            end
            S_SETUP_END: begin
                need_fence <= 1'b1;
                lm_ok      <= 1'b0;
                state      <= S_IDLE;
            end
            default: state <= S_IDLE;
            endcase
        end
    end
endmodule
