// =====================================================================================================
// bz_raster.sv — растеризатор этапа B: многоугольник → поток точек, по точке за такт (версия 5).
// =====================================================================================================
//
// ЧТО ДЕЛАЕТ
//   На вход — многоугольник, как в команде POLY (gpu/SPEC.md): до 16 вершин в Q12.4, режим и градиенты
//   четырёх величин (zi = 1/z, u, v, свет l). На выход — каждая точка внутри многоугольника, строки сверху
//   вниз, в строке слева направо: её x, y и значения zi, s, t, l в этой точке. Дальше точки пойдут в
//   конвейер «z-тест → тексель → свет → запись»; память здесь не нужна вовсе. Порядок и значения — ровно
//   как у C-модели (gpu/model/bomzh.c, функции poly и segment): стенд gpu/rtl/tb/tb_raster.cpp сравнивает
//   каждую точку на всей трассе Quake 2.
//
// ТРИ СТУПЕНИ, РАБОТАЮТ ОДНОВРЕМЕННО
//   R1, рёбра. Верхняя и нижняя вершины, потом строки сверху вниз двумя «цепочками» рёбер (вперёд и назад
//      по номерам вершин). Строка — обычно за один такт: оба края шагают приращением (DDA, ниже), отрезок
//      [xl, xr) уходит в очередь отрезков. Деление — только когда начинается новое ребро; у каждой
//      цепочки свой делитель (край N/D, потом шаг S/D), обе цепочки делят одновременно.
//   R2, куски. Режет отрезок на куски по колонкам экрана в 16 точек. У перспективы на конце каждого куска
//      деление w = 2^46 / zi; делителей четыре, и деления следующих кусков идут, пока выдаются точки этого.
//      Устроено как «выдача заданий» (сторона I: геометрия куска и запуск деления на свободном делителе) и
//      «приём» (сторона C: по порядку забирает частное, умножает s и t сразу двумя умножителями, считает
//      шаг и отдаёт кусок R3 — 3 такта на кусок). Между ними — очередь заданий на 4 места, к R3 — очередь
//      кусков на 2 места.
//
// ГДЕ БЫЛИ ПОТЕРИ (стенд считает такты без точки на выходе; трасса Quake 2, 300 кадров)
//   v1 — 8,3 такта на точку (делитель по биту, всё по очереди); bz_div4 — 2,9; v2 (строка за такт, два
//   банка, два делителя перспективы) — 1,44: из лишних 0,44 — 0,19 ждали делитель, 0,12 — умножения и шаг
//   стороны C. v3 добавляет делители и сокращает сторону C — 1,22 такта на точку.
//
// ЦЕНА В ЛОГИКЕ (пробная сборка Gowin одного узла)
//   v3 занял ~9 тыс. LUT и 2 тыс. ALU: 8 делителей с выравниванием (сдвигатель на 48 бит в каждом) и выборки
//   из общего массива градиентов по составному индексу. v4: длина частного известна заранее (край ребра
//   < 2^13, шаг ребра < 2^17, перспектива < 2^32), поэтому делители без выравнивания (bz_div4 FIXQ), на
//   цепочку рёбер — один делитель (край, потом шаг), градиенты — раздельные массивы C, A, B.
//   v5: длинные пути (19,4 нс при периоде 16,5) разрезаны на ступени: кусок на стороне I — 3 такта (длина,
//   значения на конце, задание), начало ребра — такт на множители, такт на произведения и модуль.
//   R3, точки. По описанию куска выдаёт точки: к zi, s, t, l на каждом шаге прибавляет их приращение.
//
// ДВА БАНКА ГРАДИЕНТОВ — ЧТОБЫ НЕ ЖДАТЬ
//   R2 нужны градиенты многоугольника, пока он режет его отрезки. Если бы они лежали в одном месте,
//   R1 не мог бы взять следующий многоугольник, пока R2 не кончил прежний. Поэтому банков два: R1 пишет
//   новый многоугольник в свободный банк и сразу идёт по его рёбрам, а R2 ещё доделывает прежний из
//   другого. Каждый отрезок в очереди помнит свой банк; конец многоугольника — особая запись в очереди,
//   по ней банк освобождается.
//
// DDA КРАЯ РЕБРА (digital differential analyzer — «пошаговый вычислитель»)
//   Край ребра p→q в строке с центром yc: xs = ceil(N / D), N = (p.x − 8)·dy + (yc − p.y)·dx, D = 16·dy.
//   На следующей строке N больше на S = 16·dx. Храним частное q и «недостачу» r: N = q·D − r, 0 ≤ r < D.
//   Шаг S = qs·D + rs (qs = floor(S / D), 0 ≤ rs < D) считается один раз на ребро. Тогда
//     N + S = (q + qs)·D − (r − rs):  r − rs ≥ 0 — q' = q + qs, r' = r − rs;  иначе q' = q + qs + 1, r' = r − rs + D.
//   Итог тот же, что деление на каждой строке, до бита.
//
// МЕТКА И КОНЕЦ (для конвейера точки bz_pix)
//   У многоугольника, кроме градиентов, есть режим, текстура, lightmap — их держит конвейер точки в своих
//   двух банках. Чтобы он знал, чьё состояние брать, точка несёт метку многоугольника (out_tag, вход
//   in_tag), а после последней точки идёт особый выход out_end = 1: по нему конвейер освободит банк.
module bz_raster (
    input  wire          clk,
    input  wire          rst,
    // ---- вход: многоугольник. Сначала его слова — через порт загрузки (ld_*, слово за такт, только пока
    //      in_ready = 1): номер 0…15 — вершина {y, x} в Q12.4, 16 + j — градиент j = 3·величина + {C, A, B}
    //      (величины zi, u, v, l). Потом in_valid с остальными полями (держатся, пока in_ready = 0).
    input  wire          ld_we,
    input  wire [4:0]    ld_idx,
    input  wire [31:0]   ld_data,
    input  wire          in_valid,
    output wire          in_ready,          // R1 свободен и банк градиентов свободен — можно грузить и подавать
    input  wire [4:0]    in_n,              // вершин, 3…16
    input  wire [15:0]   in_mode,           // режим POLY (бит 0 — перспектива)
    input  wire [15:0]   in_sx0, in_sy0, in_sx1, in_sy1,   // ножницы [sx0, sx1) × [sy0, sy1)
    input  wire          in_tag,            // метка многоугольника (банк состояния конвейера точки)
    // ---- выход: точки (держатся, пока out_ready = 0)
    output reg           out_valid,
    input  wire          out_ready,
    output reg  [15:0]   out_x,
    output reg  [15:0]   out_y,
    output reg  [31:0]   out_zi,
    output reg  [31:0]   out_s,
    output reg  [31:0]   out_t,
    output reg  [31:0]   out_l,
    output reg           out_tag,           // метка многоугольника этой точки
    output reg           out_end,           // 1 — не точка, а «многоугольник кончился» (после его точек;
                                            //     у пустого многоугольника — только она)
    output wire          idle,              // нет работы ни в одной ступени
    output wire [15:0]   dbg                // только стенду: состояния ступеней (где теряются такты)
);
    integer ii;

    // =================================================================================================
    // Банки градиентов и режима (пишет R1 при приёме, читает R2; освобождает R2 по записи «конец»)
    // =================================================================================================
    // (* syn_ramstyle = "registers" *) у массивов на 2–8 мест: держать в триггерах. Иначе Gowin делает их
    // распределённой памятью (SSRAM) с копией на каждого читателя, а блок SSRAM занимает ячейку логики целиком;
    // триггеры же почти бесплатны — свободных мест под них в занятых ячейках много (сборка этапа B, 23.11).
    // градиент величины k (0 zi, 1 u, 2 v, 3 l) многоугольника в банке b: {gC, gA, gB}[{b, k}] — значение
    // в точке (0, 0) и прибавки на шаг по x и по y
    (* syn_ramstyle = "registers" *) reg [31:0] gC [0:7];
    (* syn_ramstyle = "registers" *) reg [31:0] gA [0:7];
    (* syn_ramstyle = "registers" *) reg [31:0] gB [0:7];
    (* syn_ramstyle = "registers" *) reg        mb [0:1];                    // банк — перспектива
    (* syn_ramstyle = "registers" *) reg        bank_busy [0:1];
    (* syn_ramstyle = "registers" *) reg        tgb [0:1];                   // метка многоугольника в банке
    reg        r1_bank;                     // банк многоугольника, который у R1

    // =================================================================================================
    // Очередь отрезков R1 → R2: 8 мест {конец?, банк, y, xl, xr}
    // =================================================================================================
    reg        fq_e [0:7], fq_b [0:7];
    reg [15:0] fq_y [0:7], fq_l [0:7], fq_r [0:7];
    reg [3:0]  fq_w, fq_rp;
    wire [3:0] fq_used  = fq_w - fq_rp;
    wire       fq_full  = fq_used == 4'd8;
    wire       fq_empty = fq_used == 4'd0;
    // Отрезок строки сначала ложится в регистр sp_*, в очередь — тактом позже (если не пуст): сравнения
    // краёв и запись в очередь в одном такте не укладывались в 60,75 МГц (сборка этапа B 23.11). Место
    // под него держится заранее: новый отрезок — только пока в очереди не больше 6 записей (fq_full1).
    reg               sp_v, sp_b;
    reg        [15:0] sp_y;
    reg signed [15:0] sp_l, sp_r;
    wire       fq_full1 = fq_used >= 4'd7;

    // =================================================================================================
    // R1 — рёбра
    // =================================================================================================
    localparam [2:0] E_IDLE = 0, E_TB = 1, E_INIT = 2, E_RUN = 3, E_END = 4, E_CHK = 5;
    reg [2:0]  e_st;
    reg [4:0]  pn;
    reg signed [15:0] vx [0:15], vy [0:15];
    reg signed [31:0] scx0, scy0, scx1, scy1;
    reg [3:0]  top, bot, a0, a1, b0, b1;
    reg [4:0]  tbi;
    reg signed [31:0] ey, ey1;
    // 27.11.2026 (ядро 0.04): «строки кончились» (ey ≥ ey1) — регистр. Сравнение 32 бит прямо в условии
    // E_RUN разрешало весь такт рёбер и отрезка: путь ey → сравнение → … → dq, −1,4 нс при 60,75 МГц.
    // Флаг ставится в E_CHK (такт после E_INIT) и на каждой строке — для ey + 1.
    reg        e_fin;
    // DDA цепочек (0 — A, 1 — B)
    (* syn_ramstyle = "registers" *) reg        dv [0:1];
    (* syn_ramstyle = "registers" *) reg [3:0]  de [0:1];
    (* syn_ramstyle = "registers" *) reg signed [31:0] dq [0:1];
    (* syn_ramstyle = "registers" *) reg signed [31:0] dqs [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] dr [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] drs [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] dD [0:1];
    (* syn_ramstyle = "registers" *) reg [2:0]  ph [0:1];                    // начало ребра: 0 — нет, 3 — множители, 1 — деление N, 2 — деление S
    // p.x − 8, dy, yc − p.y, dx (в 1/16 точки): все меньше 2^17 по модулю (вершины — int16, строка < 2048),
    // поэтому 18 бит со знаком: два произведения 18 × 18 — один блок DSP на цепочку (с 20 битами было
    // по два блока 36 × 36 на произведение — 8 из 24 блоков кристалла)
    (* syn_ramstyle = "registers" *) reg signed [17:0] ea [0:1];
    (* syn_ramstyle = "registers" *) reg signed [17:0] eb [0:1];
    (* syn_ramstyle = "registers" *) reg signed [17:0] ec [0:1];
    (* syn_ramstyle = "registers" *) reg signed [17:0] ed [0:1];
    (* syn_ramstyle = "registers" *) reg signed [47:0] es [0:1];             // S = 16·dx
    (* syn_ramstyle = "registers" *) reg signed [47:0] en [0:1];             // N (защёлкнут между умножением и модулем)
    (* syn_ramstyle = "registers" *) reg        n_neg [0:1];
    (* syn_ramstyle = "registers" *) reg        s_neg [0:1];

    wire signed [31:0] yc = (ey <<< 4) + 32'sd8;
    wire [3:0] a1_next = (a1 == pn - 1) ? 4'd0 : a1 + 4'd1;
    wire [3:0] b1_next = (b1 == 4'd0) ? pn - 5'd1 : b1 - 4'd1;
    wire       adv_a = a1 != bot && 32'(vy[a1]) <= yc;
    wire       adv_b = b1 != bot && 32'(vy[b1]) <= yc;
    // ребро цепочки c: p → q (в цикле ниже — выбор (c == 0 ? A : B))
    wire       ok0 = dv[0] && de[0] == a0;  // DDA цепочки готов для этого ребра и этой строки
    wire       ok1 = dv[1] && de[1] == b0;

    // числитель N, шаг S и знаменатель D ребра цепочки c в строке yc
    function signed [47:0] f_n (input signed [15:0] px, input signed [15:0] py, input signed [15:0] qx,
                                input signed [15:0] qy, input signed [31:0] ycc);
        f_n = 48'((40'(px) - 40'sd8) * (40'(qy) - 40'(py)) + (40'(ycc) - 40'(py)) * (40'(qx) - 40'(px)));
    endfunction
    function signed [47:0] f_s (input signed [15:0] px, input signed [15:0] qx);
        f_s = 48'((40'(qx) - 40'(px)) <<< 4);
    endfunction
    function [31:0] f_d (input signed [15:0] py, input signed [15:0] qy);
        f_d = 32'(40'(qy) - 40'(py)) << 4;
    endfunction

    // делитель R1 — один на обе цепочки (с v5b: два делителя — ~800 ячеек, а кристалл полон). Цепочка
    // берёт его на оба своих деления подряд (край N/D, потом шаг S/D) — e_busy, e_own; вторая ждёт.
    // Частное короче 2^18: край ребра |N/D| < 2^12 (точка на экране), шаг |S/D| = |dx/dy| ≤ 2^16 — поэтому
    // без выравнивания (FIXQ = 18, 9 шагов по 2 цифры)
    reg         e_start;
    reg  [47:0] e_num;
    reg  [31:0] e_den;
    reg         e_busy, e_own;
    wire        e_done;
    wire [47:0] e_q;
    wire [31:0] e_r;
    bz_div4 #(.W(48), .WD(32), .FIXQ(18)) div_e (.clk(clk), .rst(rst), .start(e_start), .num(e_num), .den(e_den),
                                                 .busy(), .done(e_done), .quot(e_q), .rem(e_r));

    assign in_ready = e_st == E_IDLE && !bank_busy[r1_bank];

    // =================================================================================================
    // R2 — куски: сторона I (выдача) и сторона C (приём), очередь заданий между ними
    // =================================================================================================
    function [16:0] recip (input [4:0] n);
        case (n)
            1: recip = 65536;  2: recip = 32768;  3: recip = 21845;  4: recip = 16384;
            5: recip = 13107;  6: recip = 10922;  7: recip = 9362;   8: recip = 8192;
            9: recip = 7281;   10: recip = 6553;  11: recip = 5957;  12: recip = 5461;
            13: recip = 5041;  14: recip = 4681;  15: recip = 4369;  default: recip = 4096;
        endcase
    endfunction
    // шаг внутри куска: (a1 − a0) · (1 / n) >> 16. Разность и 1 / n — в регистрах (такт C_STEP), здесь —
    // только умножение: «очередь → 1 / n → умножитель → очередь кусков» в одном такте не укладывалась в
    // 60,75 МГц (сборка этапа B 23.11).
    function [31:0] step_mul (input signed [33:0] d, input [16:0] rc);
        reg signed [51:0] p;
        begin
            p = d * $signed({1'b0, rc});
            step_mul = p[47:16];
        end
    endfunction
    function [31:0] zclamp (input [31:0] zi);
        zclamp = ($signed(zi) < 32'sd32768) ? 32'd32768 : zi;
    endfunction

    // делители перспективы: два (v3–v5 — четыре: 1,22–1,39 такта на точку против 1,44 у двух), задание — на
    // первый свободный. Два — ради места в кристалле: растеризатор всё равно ждёт конвейер точки (bz_pix),
    // у которого на точку уходит больше (промахи кэша, чтение кусков z), а делитель — ~500 ячеек логики.
    localparam int NU = 2;
    (* syn_ramstyle = "registers" *) reg         p_start [0:NU-1];
    (* syn_ramstyle = "registers" *) reg  [31:0] p_den [0:NU-1];
    wire        p_done [0:NU-1];
    wire [47:0] p_q [0:NU-1];
    (* syn_ramstyle = "registers" *) reg         own [0:NU-1];  // делитель занят заданием; его частное готово и ещё не взято
    (* syn_ramstyle = "registers" *) reg         rdy [0:NU-1];
    genvar gp;
    generate for (gp = 0; gp < NU; gp = gp + 1) begin : g_pdiv
        // частное 2^46 / zi < 2^32 при zi ≥ 2^15 — фиксированно 32 бита, 16 шагов
        bz_div4 #(.W(48), .WD(32), .FIXQ(32)) div_p (.clk(clk), .rst(rst), .start(p_start[gp]), .num(48'h4000_0000_0000),
                                          .den(p_den[gp]), .busy(), .done(p_done[gp]), .quot(p_q[gp]), .rem());
    end endgenerate

    // очередь заданий: тип 0 — конец многоугольника, 1 — начало отрезка (только деление), 2 — кусок с
    // перспективой, 3 — аффинный кусок (без деления)
    localparam [1:0] J_END = 0, J_START = 1, J_SEG = 2, J_AFF = 3;
    reg [1:0]  jt [0:3];
    reg [1:0]  ju [0:3];
    reg        jb [0:3];
    reg [15:0] jx [0:3], jy [0:3];
    reg [4:0]  jn [0:3];
    reg [31:0] jzi [0:3], jl [0:3], ja [0:3], jv [0:3];   // ja/jv: su/tv (или s/t у аффинного)
    reg [2:0]  jq_w, jq_r;
    wire [2:0] jq_used  = jq_w - jq_r;
    wire       jq_full  = jq_used == 3'd4;
    wire       jq_empty = jq_used == 3'd0;
    wire [1:0] jh = jq_r[1:0];              // голова очереди

    // сторона I
    localparam [2:0] I_IDLE = 0, I_BASE = 1, I_START = 2, I_SEG = 3, I_V1 = 4, I_SEG2 = 5;
    reg [2:0]  i_st;
    reg [4:0]  sn;                          // длина куска (защёлкнута в I_SEG)
    reg [15:0] snx;                         // его конец
    reg [31:0] v1 [0:3];                    // значения на конце куска (защёлкнуты в I_V1)
    reg [15:0] iy, ix, ixr;
    reg        ib;
    reg [1:0]  ga;
    reg [31:0] v0 [0:3];                    // zi, u, v, l в начале очередного куска
    wire [31:0] g_base = gC[{ib, ga}] + gA[{ib, ga}] * {16'd0, ix} + gB[{ib, ga}] * {16'd0, iy};
    wire [15:0] i_nx = ((ix | 16'd15) + 16'd1 < ixr) ? (ix | 16'd15) + 16'd1 : ixr;
    wire [4:0]  i_n  = 5'(i_nx - ix);

    // первый свободный делитель перспективы (приоритетный выбор: меньший номер)
    reg         pu_any;
    reg  [1:0]  pu_sel;
    always @* begin
        pu_any = 1'b0;
        pu_sel = 2'd0;
        for (ii = NU - 1; ii >= 0; ii = ii - 1)
            if (!own[ii]) begin
                pu_any = 1'b1;
                pu_sel = 2'(ii);
            end
    end

    // сторона C
    localparam [2:0] C_IDLE = 0, C_MUL = 1, C_EMIT = 2, C_STEP = 3;
    reg [2:0]  c_st;
    reg signed [33:0] cd_s, cd_t;           // s1 − s0, t1 − t0 куска (C_STEP)
    reg [16:0] c_rc;                        // 1 / n куска в 1.16 (C_STEP)
    reg [31:0] pm_as, pm_at, pm_w;          // su, tv и частное w — защёлкнуты, умножение в следующем такте
    wire signed [64:0] pm_ps = $signed(pm_as) * $signed({1'b0, pm_w});
    wire signed [64:0] pm_pt = $signed(pm_at) * $signed({1'b0, pm_w});
    wire [31:0] ms = pm_ps[51:20];          // s = (su · w) >> 20
    wire [31:0] mt = pm_pt[51:20];          // t = (tv · w) >> 20
    reg [31:0] s1r, t1r;
    reg [31:0] ps0, pt0;                    // s, t в начале куска

    // очередь кусков к R3: 2 места
    (* syn_ramstyle = "registers" *) reg [15:0] sg_x [0:1];
    (* syn_ramstyle = "registers" *) reg [15:0] sg_y [0:1];
    (* syn_ramstyle = "registers" *) reg [4:0]  sg_n [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_zi [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_azi [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_s [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_ds [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_t [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_dt [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_l [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] sg_al [0:1];
    (* syn_ramstyle = "registers" *) reg        sg_e [0:1];     // место — конец многоугольника; метка многоугольника
    (* syn_ramstyle = "registers" *) reg        sg_tg [0:1];
    reg [1:0]  sg_w, sg_r;
    wire [1:0] sg_used = sg_w - sg_r;
    wire       sg_full = sg_used == 2'd2;
    wire       sg_any  = sg_used != 2'd0;
    wire       sw = sg_w[0];                // куда пишет сторона C
    wire       sr = sg_r[0];                // откуда берёт R3

    // =================================================================================================
    // R3 — точки
    // =================================================================================================
    reg        r3_busy;
    reg [15:0] cx, cy;
    reg [4:0]  ck, cn;
    reg [31:0] czi, cazi, cs, cds, ct, cdt, cl, cal;
    reg        cend, ctag;
    wire emit = r3_busy && (!out_valid || out_ready);
    wire last = emit && (ck == cn - 1'b1);
    wire take = sg_any && (!r3_busy || last);

    assign dbg = {e_st, i_st[1:0], c_st, r3_busy, sg_any, fq_empty, jq_empty, ph[0] != 3'd0, ph[1] != 3'd0,
                  own[0], own[NU - 1]};      // (стенду: заняты ли делители перспективы)
    assign idle = e_st == E_IDLE && !bank_busy[0] && !bank_busy[1] && !sp_v && fq_empty && i_st == I_IDLE &&
                  jq_empty && c_st == C_IDLE && !sg_any && !r3_busy && !out_valid;

    // =================================================================================================
    // Всё в одном always: у ступеней общие флаги (занятость банков, делителей, места куска)
    // =================================================================================================
    always @(posedge clk) begin
        e_start <= 1'b0;
        for (ii = 0; ii < NU; ii = ii + 1)
            p_start[ii] <= 1'b0;
        if (rst) begin
            e_st <= E_IDLE; r1_bank <= 1'b0; bank_busy[0] <= 1'b0; bank_busy[1] <= 1'b0;
            fq_w <= 4'd0; fq_rp <= 4'd0; jq_w <= 3'd0; jq_r <= 3'd0;
            i_st <= I_IDLE; c_st <= C_IDLE;
            for (ii = 0; ii < NU; ii = ii + 1) begin
                own[ii] <= 1'b0;
                rdy[ii] <= 1'b0;
            end
            dv[0] <= 1'b0; dv[1] <= 1'b0; ph[0] <= 3'd0; ph[1] <= 3'd0; e_busy <= 1'b0;
            sg_w <= 2'd0; sg_r <= 2'd0; r3_busy <= 1'b0; out_valid <= 1'b0; sp_v <= 1'b0;
        end else begin
            // ============================================ отрезок прошлого такта — в очередь (пустой — нет)
            // (новый отрезок ниже, в R1, ставит sp_v <= 1 позже в этом же блоке — его запись главнее)
            if (sp_v) begin
                if (sp_l < sp_r) begin
                    fq_e[fq_w[2:0]] <= 1'b0;
                    fq_b[fq_w[2:0]] <= sp_b;
                    fq_y[fq_w[2:0]] <= sp_y;
                    fq_l[fq_w[2:0]] <= sp_l;
                    fq_r[fq_w[2:0]] <= sp_r;
                    fq_w <= fq_w + 1'b1;
                end
                sp_v <= 1'b0;
            end

            // ============================================ загрузка многоугольника (R1 свободен, банк r1_bank свободен)
            if (ld_we && in_ready) begin
                if (!ld_idx[4]) begin
                    vx[ld_idx[3:0]] <= ld_data[15:0];
                    vy[ld_idx[3:0]] <= ld_data[31:16];
                end else
                    case (ld_idx[3:0])
                        4'd0:  gC[{r1_bank, 2'd0}] <= ld_data;
                        4'd1:  gA[{r1_bank, 2'd0}] <= ld_data;
                        4'd2:  gB[{r1_bank, 2'd0}] <= ld_data;
                        4'd3:  gC[{r1_bank, 2'd1}] <= ld_data;
                        4'd4:  gA[{r1_bank, 2'd1}] <= ld_data;
                        4'd5:  gB[{r1_bank, 2'd1}] <= ld_data;
                        4'd6:  gC[{r1_bank, 2'd2}] <= ld_data;
                        4'd7:  gA[{r1_bank, 2'd2}] <= ld_data;
                        4'd8:  gB[{r1_bank, 2'd2}] <= ld_data;
                        4'd9:  gC[{r1_bank, 2'd3}] <= ld_data;
                        4'd10: gA[{r1_bank, 2'd3}] <= ld_data;
                        default: gB[{r1_bank, 2'd3}] <= ld_data;
                    endcase
            end

            // ============================================ R1
            case (e_st)
            E_IDLE:
                if (in_valid && in_ready) begin
                    pn <= in_n;                 // вершины и градиенты уже загружены (порт ld_*)
                    mb[r1_bank] <= in_mode[0];
                    tgb[r1_bank] <= in_tag;
                    bank_busy[r1_bank] <= 1'b1;
                    scx0 <= 32'($signed(in_sx0)); scy0 <= 32'($signed(in_sy0));
                    scx1 <= 32'($signed(in_sx1)); scy1 <= 32'($signed(in_sy1));
                    top <= 4'd0; bot <= 4'd0; tbi <= 5'd1;
                    e_st <= E_TB;
                end
            E_TB:
                if (tbi == pn)
                    e_st <= E_INIT;
                else begin
                    if (vy[tbi[3:0]] < vy[top]) top <= tbi[3:0];
                    if (vy[tbi[3:0]] > vy[bot]) bot <= tbi[3:0];
                    tbi <= tbi + 1'b1;
                end
            E_INIT: begin : init
                reg signed [31:0] y0, y1;
                y0 = (32'(vy[top]) + 32'sd7) >>> 4;
                y1 = (32'(vy[bot]) + 32'sd7) >>> 4;
                ey  <= (y0 < scy0) ? scy0 : y0;
                ey1 <= (y1 > scy1) ? scy1 : y1;
                a0 <= top; b0 <= top;
                a1 <= (top == pn - 1) ? 4'd0 : top + 4'd1;
                b1 <= (top == 4'd0) ? pn - 5'd1 : top - 4'd1;
                dv[0] <= 1'b0; dv[1] <= 1'b0;
                e_st <= E_CHK;
            end
            E_CHK: begin
                e_fin <= ey >= ey1;
                e_st  <= E_RUN;
            end
            // строка: сдвинуть цепочки → начать рёбра (деления) → отрезок и шаг DDA
            E_RUN:
                if (e_fin)
                    e_st <= E_END;
                else if (adv_a || adv_b) begin
                    if (adv_a) begin a0 <= a1; a1 <= a1_next; end
                    if (adv_b) begin b0 <= b1; b1 <= b1_next; end
                end else begin : edges
                    reg e_take;                   // делитель уже занят (или взят в этом такте цепочкой A)
                    e_take = e_busy;
                    for (ii = 0; ii < 2; ii = ii + 1)
                        if (!(ii == 0 ? ok0 : ok1))
                            case (ph[ii])
                            3'd0: begin : edge_start      // ребро p → q началось: такт 1 — множители
                                reg signed [15:0] px, py, qx, qy;
                                px = vx[(ii == 0 ? a0 : b0)]; py = vy[(ii == 0 ? a0 : b0)];
                                qx = vx[(ii == 0 ? a1 : b1)]; qy = vy[(ii == 0 ? a1 : b1)];
                                ea[ii] <= 18'(px) - 18'sd8;
                                eb[ii] <= 18'(qy) - 18'(py);
                                ec[ii] <= 18'(yc - 32'(py));
                                ed[ii] <= 18'(qx) - 18'(px);
                                dD[ii] <= f_d (py, qy);
                                es[ii] <= f_s (px, qx);
                                ph[ii] <= 3'd3;
                            end
                            3'd3: begin                   // такт 2 — N = (p.x − 8)·dy + (yc − p.y)·dx
                                en[ii] <= 48'(36'(ea[ii]) * 36'(eb[ii]) + 36'(ec[ii]) * 36'(ed[ii]));
                                ph[ii] <= 3'd4;
                            end
                            3'd4:                         // такт 3 — модуль и деление |N| / D (делитель свободен)
                                if (!e_take) begin
                                    e_take    = 1'b1;
                                    e_busy   <= 1'b1;
                                    e_own    <= 1'(ii);
                                    n_neg[ii] <= en[ii] < 0;
                                    e_num    <= (en[ii] < 0) ? 48'(-en[ii]) : 48'(en[ii]);
                                    e_den    <= dD[ii];
                                    e_start  <= 1'b1;
                                    ph[ii]   <= 3'd1;
                                end
                            3'd1:
                                if (e_done && e_own == 1'(ii)) begin : edge_n
                                    // |N| = Q·D + R0: N ≥ 0 — ceil = Q + (R0 ≠ 0), r = D − R0 (или 0);
                                    // N < 0 — ceil = −Q, r = R0. Сразу второе деление — шаг S = 16·dx = qs·D + rs
                                    reg signed [47:0] fs;
                                    if (!n_neg[ii]) begin
                                        dq[ii] <= 32'(e_q) + ((e_r != 0) ? 32'sd1 : 32'sd0);
                                        dr[ii] <= (e_r != 0) ? dD[ii] - e_r : 32'd0;
                                    end else begin
                                        dq[ii] <= -32'(e_q);
                                        dr[ii] <= e_r;
                                    end
                                    fs = es[ii];
                                    s_neg[ii] <= fs < 0;
                                    e_num     <= (fs < 0) ? 48'(-fs) : 48'(fs);
                                    e_den     <= dD[ii];
                                    e_start   <= 1'b1;
                                    ph[ii]    <= 3'd2;
                                end
                            default:
                                if (e_done && e_own == 1'(ii)) begin
                                    // |S| = Q·D + R0: S ≥ 0 — qs = Q, rs = R0; S < 0 — qs = −Q − (R0 ≠ 0), rs = D − R0 (или 0)
                                    if (!s_neg[ii]) begin
                                        dqs[ii] <= 32'(e_q);
                                        drs[ii] <= e_r;
                                    end else begin
                                        dqs[ii] <= -32'(e_q) - ((e_r != 0) ? 32'sd1 : 32'sd0);
                                        drs[ii] <= (e_r != 0) ? dD[ii] - e_r : 32'd0;
                                    end
                                    e_busy <= 1'b0;
                                    dv[ii] <= 1'b1;
                                    de[ii] <= (ii == 0 ? a0 : b0);
                                    ph[ii] <= 3'd0;
                                end
                            endcase
                    // обе цепочки готовы — отрезок этой строки и шаг обеих на следующую
                    if (ok0 && ok1 && !fq_full1) begin : span
                        // края — в 16 битах: на экране |x| < 2^15, короче цепочки переноса (тайминг)
                        reg signed [15:0] q0, q1, l, r;
                        reg signed [32:0] rn;
                        q0 = dq[0][15:0];
                        q1 = dq[1][15:0];
                        l = (q0 < q1) ? q0 : q1;
                        r = (q0 < q1) ? q1 : q0;
                        // 27.11.2026 (ядро 0.04): ножницы — со знаком. Выборка части scx0[15:0] в Verilog беззнаковая, и
                        // сравнение с ней шло без знака: край левее окна (l < 0) не зажимался, а отрезок целиком левее
                        // окна (r < 0) зажимался справа и рисовался на всю строку (стенд tb_raster, трасса demo1 с
                        // гранями без отсечения: кадр 124, l = −51). До 0.04 ESP вершин левее окна не шлёт.
                        if (l < $signed(scx0[15:0])) l = $signed(scx0[15:0]);
                        if (r > $signed(scx1[15:0])) r = $signed(scx1[15:0]);
                        // в очередь — тактом позже (выше), если l < r
                        sp_v <= 1'b1;
                        sp_b <= r1_bank;
                        sp_y <= ey[15:0];
                        sp_l <= l;
                        sp_r <= r;
                        for (ii = 0; ii < 2; ii = ii + 1) begin
                            rn = $signed({1'b0, dr[ii]}) - $signed({1'b0, drs[ii]});
                            if (rn < 0) begin
                                dq[ii] <= dq[ii] + dqs[ii] + 32'sd1;
                                dr[ii] <= 32'(rn + $signed({1'b0, dD[ii]}));
                            end else begin
                                dq[ii] <= dq[ii] + dqs[ii];
                                dr[ii] <= 32'(rn);
                            end
                        end
                        ey <= ey + 1;
                        e_fin <= ey + 1 >= ey1;
                    end
                end
            // конец многоугольника — особая запись в очереди: по ней R2 освободит банк
            E_END:                          // (после последнего отрезка: он уходит в очередь раньше)
                if (!fq_full && !sp_v) begin
                    fq_e[fq_w[2:0]] <= 1'b1;
                    fq_b[fq_w[2:0]] <= r1_bank;
                    fq_w    <= fq_w + 1'b1;
                    r1_bank <= ~r1_bank;
                    e_st    <= E_IDLE;
                end
            default: e_st <= E_IDLE;
            endcase

            // ============================================ делители: частное готово
            for (ii = 0; ii < NU; ii = ii + 1)
                if (p_done[ii])
                    rdy[ii] <= 1'b1;

            // ============================================ R2, сторона I — выдача заданий
            case (i_st)
            I_IDLE:
                if (!fq_empty) begin
                    if (fq_e[fq_rp[2:0]]) begin
                        if (!jq_full) begin
                            jt[jq_w[1:0]] <= J_END;
                            jb[jq_w[1:0]] <= fq_b[fq_rp[2:0]];
                            jq_w  <= jq_w + 1'b1;
                            fq_rp <= fq_rp + 1'b1;
                        end
                    end else begin
                        iy    <= fq_y[fq_rp[2:0]];
                        ix    <= fq_l[fq_rp[2:0]];
                        ixr   <= fq_r[fq_rp[2:0]];
                        ib    <= fq_b[fq_rp[2:0]];
                        fq_rp <= fq_rp + 1'b1;
                        ga    <= 2'd0;
                        i_st  <= I_BASE;
                    end
                end
            // значения четырёх величин в начале отрезка — по одной за такт
            I_BASE: begin
                v0[ga] <= g_base;
                ga     <= ga + 1'b1;
                if (ga == 2'd3)
                    i_st <= mb[ib] ? I_START : I_SEG;
            end
            // перспектива: деление в начале отрезка (s, t начала посчитает сторона C)
            I_START:
                if (pu_any && !jq_full) begin
                    p_den[pu_sel]   <= zclamp (v0[0]);
                    p_start[pu_sel] <= 1'b1;
                    own[pu_sel]     <= 1'b1;
                    jt[jq_w[1:0]] <= J_START;
                    ju[jq_w[1:0]] <= pu_sel;
                    jb[jq_w[1:0]] <= ib;
                    ja[jq_w[1:0]] <= v0[1];
                    jv[jq_w[1:0]] <= v0[2];
                    jq_w <= jq_w + 1'b1;
                    i_st <= I_SEG;
                end
            // очередной кусок, такт 1: длина до границы колонки 16
            I_SEG:
                if (ix >= ixr)
                    i_st <= I_IDLE;
                else begin
                    sn   <= i_n;
                    snx  <= i_nx;
                    i_st <= I_V1;
                end
            // такт 2: значения на конце куска (v0 + A·n) — по ним деление перспективы и следующий кусок
            I_V1: begin
                for (ii = 0; ii < 4; ii = ii + 1)
                    v1[ii] <= v0[ii] + gA[{ib, 2'(ii)}] * {27'd0, sn};
                i_st <= I_SEG2;
            end
            // такт 3: задание в очередь; у перспективы — деление на конце куска
            I_SEG2:
                if (!jq_full && (!mb[ib] || pu_any)) begin
                    jt[jq_w[1:0]]  <= mb[ib] ? J_SEG : J_AFF;
                    ju[jq_w[1:0]]  <= pu_sel;
                    jb[jq_w[1:0]]  <= ib;
                    jx[jq_w[1:0]]  <= ix;
                    jy[jq_w[1:0]]  <= iy;
                    jn[jq_w[1:0]]  <= sn;
                    jzi[jq_w[1:0]] <= v0[0];
                    jl[jq_w[1:0]]  <= v0[3];
                    ja[jq_w[1:0]]  <= mb[ib] ? v1[1] : v0[1];
                    jv[jq_w[1:0]]  <= mb[ib] ? v1[2] : v0[2];
                    jq_w <= jq_w + 1'b1;
                    if (mb[ib]) begin
                        p_den[pu_sel]   <= zclamp (v1[0]);
                        p_start[pu_sel] <= 1'b1;
                        own[pu_sel]     <= 1'b1;
                    end
                    for (ii = 0; ii < 4; ii = ii + 1)
                        v0[ii] <= v1[ii];
                    ix   <= snx;
                    i_st <= I_SEG;
                end
            default: i_st <= I_IDLE;
            endcase

            // ============================================ R2, сторона C — приём по порядку
            case (c_st)
            C_IDLE:
                if (!jq_empty)
                    case (jt[jh])
                    J_END:
                        // конец многоугольника: банк градиентов свободен, а R3 выдаст метку конца
                        if (!sg_full) begin
                            sg_e[sw]  <= 1'b1;
                            sg_tg[sw] <= tgb[jb[jh]];
                            sg_n[sw]  <= 5'd1;
                            sg_w <= sg_w + 1'b1;
                            bank_busy[jb[jh]] <= 1'b0;
                            jq_r <= jq_r + 1'b1;
                        end
                    J_AFF:
                        if (!sg_full) begin
                            sg_x[sw] <= jx[jh]; sg_y[sw] <= jy[jh]; sg_n[sw] <= jn[jh];
                            sg_e[sw] <= 1'b0; sg_tg[sw] <= tgb[jb[jh]];
                            sg_zi[sw] <= jzi[jh]; sg_azi[sw] <= gA[{jb[jh], 2'd0}];
                            sg_s[sw]  <= ja[jh];  sg_ds[sw]  <= gA[{jb[jh], 2'd1}];
                            sg_t[sw]  <= jv[jh];  sg_dt[sw]  <= gA[{jb[jh], 2'd2}];
                            sg_l[sw]  <= jl[jh];  sg_al[sw]  <= gA[{jb[jh], 2'd3}];
                            sg_w <= sg_w + 1'b1;
                            jq_r <= jq_r + 1'b1;
                        end
                    default:                        // J_START, J_SEG: ждать частное своего делителя
                        if (rdy[ju[jh]]) begin
                            pm_as <= ja[jh];
                            pm_at <= jv[jh];
                            pm_w  <= p_q[ju[jh]][31:0];
                            c_st  <= C_MUL;
                        end
                    endcase
            // s и t одновременно; делитель свободен
            C_MUL: begin
                own[ju[jh]] <= 1'b0;
                rdy[ju[jh]] <= 1'b0;
                if (jt[jh] == J_START) begin
                    ps0  <= ms;
                    pt0  <= mt;
                    jq_r <= jq_r + 1'b1;
                    c_st <= C_IDLE;
                end else begin
                    s1r  <= ms;
                    t1r  <= mt;
                    c_st <= C_STEP;
                end
            end
            // разности s, t на куске и 1 / n — в регистры
            C_STEP: begin
                cd_s <= $signed({s1r[31], s1r}) - $signed({ps0[31], ps0});
                cd_t <= $signed({t1r[31], t1r}) - $signed({pt0[31], pt0});
                c_rc <= recip (jn[jh]);
                c_st <= C_EMIT;
            end
            // шаг внутри куска (умножение) и выдача — в одном такте
            C_EMIT:
                if (!sg_full) begin
                    sg_x[sw] <= jx[jh]; sg_y[sw] <= jy[jh]; sg_n[sw] <= jn[jh];
                    sg_e[sw] <= 1'b0; sg_tg[sw] <= tgb[jb[jh]];
                    sg_zi[sw] <= jzi[jh]; sg_azi[sw] <= gA[{jb[jh], 2'd0}];
                    sg_s[sw]  <= ps0;     sg_ds[sw]  <= step_mul (cd_s, c_rc);
                    sg_t[sw]  <= pt0;     sg_dt[sw]  <= step_mul (cd_t, c_rc);
                    sg_l[sw]  <= jl[jh];  sg_al[sw]  <= gA[{jb[jh], 2'd3}];
                    sg_w <= sg_w + 1'b1;
                    ps0  <= s1r;
                    pt0  <= t1r;
                    jq_r <= jq_r + 1'b1;
                    c_st <= C_IDLE;
                end
            default: c_st <= C_IDLE;
            endcase

            // ============================================ R3
            if (out_valid && out_ready && !emit)
                out_valid <= 1'b0;
            if (emit) begin
                out_valid <= 1'b1;
                out_x  <= cx;  out_y <= cy;
                out_zi <= czi; out_s <= cs; out_t <= ct; out_l <= cl;
                out_tag <= ctag; out_end <= cend;
                cx  <= cx + 1'b1;
                czi <= czi + cazi;
                cs  <= cs + cds;
                ct  <= ct + cdt;
                cl  <= cl + cal;
                ck  <= ck + 1'b1;
            end
            if (take) begin
                r3_busy <= 1'b1;
                cx  <= sg_x[sr];  cy  <= sg_y[sr];  ck <= 5'd0;  cn <= sg_n[sr];
                czi <= sg_zi[sr]; cazi <= sg_azi[sr];
                cs  <= sg_s[sr];  cds  <= sg_ds[sr];
                ct  <= sg_t[sr];  cdt  <= sg_dt[sr];
                cl  <= sg_l[sr];  cal  <= sg_al[sr];
                cend <= sg_e[sr]; ctag <= sg_tg[sr];
                sg_r <= sg_r + 1'b1;
            end else if (last)
                r3_busy <= 1'b0;
        end
    end
endmodule
