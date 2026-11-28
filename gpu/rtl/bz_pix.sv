// =====================================================================================================
// bz_pix.sv — конвейер точки этапа B: z-тест, тексель, свет, прозрачность, запись — по точке за такт.
// =====================================================================================================
//
// ЧТО ДЕЛАЕТ
//   Растеризатор (bz_raster) выдаёт точки многоугольника: x, y и значения zi, s, t, l в этой точке. Здесь
//   с каждой точкой делается то же, что в C-модели (gpu/model/bomzh.c, функция segment), — до бита:
//     z-тест с z-буфером, тексель по s, t (с волной воды), свет (lightmap или вершины) через colormap,
//     прозрачность через alphamap, запись цвета в кадр и z в z-буфер.
//   Те же ступени рисуют и команды ядра FILL, POINT, PIC: ядро само выдаёт их точки на этот же вход
//   (режимы SOLID, ZRAW, NOCOL ниже).
//
// КОНВЕЙЕР
//   Точка проходит восемь ступеней, по такту на каждую; в каждой ступени — своя точка, все восемь
//   работают одновременно, как рабочие на ленте. Поэтому, хотя на одну точку уходит 8 тактов, из
//   конвейера выходит по точке за такт.
//     A — вход: z в 16 бит, номер точки в кадре y·S + x, клетка lightmap (синус волны читается уже);
//     B — волна (s, t + синус), координаты текселя (повтор или зажим), адреса клеток lightmap;
//     T — начало строки текселя, адреса z и цвета в SDRAM, чтение четырёх клеток lightmap;
//     C — адрес текселя, чтение кэша текстур, первая половина смешивания клеток lightmap;
//     H — метки кэша текстур с выхода BSRAM: попадание; строка света 0…63 (ступень с 27.11.2026);
//     D — промах кэша текстур — «стоп», строка грузится из SDRAM;
//     E — кусок z: тест и запись z; ключ (тексель 255 прозрачен); чтение colormap;
//     F — кусок цвета: прозрачность (alphamap), запись цвета.
//
// «СТОП» ВСЕГО КОНВЕЙЕРА
//   Иногда ступень не может закончить точку за такт: в кэше нет строки текстуры, нужный кусок z ещё не
//   прочитан из SDRAM, прозрачной точке нужен второй такт. Тогда сигнал adv («шагнуть») = 0, и все
//   регистры конвейера стоят на месте, пока причина не уйдёт. Это проще, чем у каждой ступени свой
//   «готов/жду»: один общий сигнал, и ни одна точка не теряется и не обгоняет другую.
//   Побочные действия ступени (запись z, цвета) делаются только в такт, когда точка уходит дальше
//   (adv = 1): точка, стоящая несколько тактов, пишет один раз.
//
// КУСКИ (CHUNK) Z И ЦВЕТА
//   SDRAM читается и пишется пачками по 32 байта. В 32 байтах — 16 значений z (по 2 байта) или 32
//   точки цвета (по байту). Растеризатор выдаёт точки кусками строки по 16 колонок — почти всегда точки
//   подряд попадают в одну пачку. Поэтому у каждого буфера свой «кусок»: регистр на 32 байта, копия
//   одной пачки. Точки читают и пишут кусок; в SDRAM он уходит целиком, когда точка попросит другой
//   кусок (или ядро попросит сброс). Маска «записано» (dm) — по дорожке (значению): в SDRAM пишутся
//   только они, через маску байт. Читать кусок из SDRAM нужно, только если точке нужно старое значение
//   (z-тест, прозрачность) — заливка z (FILL) и обычный цвет только пишут.
//   Вытесненный кусок копируется в «буфер записи» — мост SDRAM заберёт его слова потом, а кусок уже
//   свободен под следующие точки.
//
// КЭШИ
//   Текстуры — 8 КБ, 2 пути (у каждой строки памяти два возможных места), строка 32 байта.
//   Alphamap (таблица прозрачности 64 КБ) — 2 КБ, прямое отображение (одно место у строки).
//   Lightmap — не кэш: ядро перед многоугольником грузит его lightmap целиком (до 64×64 клеток) в один
//   из двух слотов; пока грузится второй слот, точки читают первый.
//   Colormap (16 КБ) и синус волны (128 слов) — копии в BSRAM, грузит команда SETUP.
//
// БАНКИ СОСТОЯНИЯ
//   У многоугольника свой режим, своя текстура, свой lightmap. Точки двух соседних многоугольников
//   идут в конвейере вперемешку по ступеням, поэтому состояние лежит в двух банках: ядро пишет банк,
//   точки несут его номер (tag), каждая ступень читает «свой» банк. За последней точкой идёт метка
//   конца (end); когда она выходит из F, банк свободен (rel) — ядро может писать в него снова.
//
// ПОРЯДОК В ПАМЯТИ (почему результат точно как у модели)
//   Модель рисует точки строго по очереди. Здесь точка тоже читает z и цвет в той же ступени, где
//   пишет (кусок — один на буфер), поэтому следующая точка видит запись предыдущей, даже если это
//   одна и та же точка экрана у двух многоугольников. Мост SDRAM держит порядок операций одного
//   клиента: запись вытесненного куска и чтение того же куска позже не переставятся.
//   Кэши текстур и alphamap только читают; после WRITE (новая текстура) ядро их сбрасывает (inv).
module bz_pix (
    input  wire        clk,
    input  wire        rst,
    // ---- точки: от растеризатора или от ядра; держатся, пока in_ready = 0
    input  wire        in_valid,
    output wire        in_ready,
    input  wire [15:0] in_x,
    input  wire [15:0] in_y,
    input  wire [31:0] in_zi,
    input  wire [31:0] in_s,
    input  wire [31:0] in_t,
    input  wire [31:0] in_l,
    input  wire        in_tag,          // банк состояния
    input  wire        in_end,          // не точка: метка «многоугольник кончился»
    output reg         rel,             // такт: метка вышла из F — банк rel_tag свободен
    output reg         rel_tag,
    // ---- состояние многоугольника: ядро пишет банк ps_tag до его первой точки
    input  wire        ps_we,
    input  wire        ps_tag,
    input  wire [11:0] ps_mode,         // режим точки (биты M_* ниже)
    input  wire [15:0] ps_k16,          // ZRAW — значение z; SOLID — цвет (младший байт)
    input  wire [22:0] ps_taddr,        // текстура: адрес
    input  wire        ps_tlin,         //   1 — LINEAR (строки подряд, края зажаты), 0 — TILED
    input  wire [15:0] ps_ta,           //   TILED: [3:0] log2 ширины, [7:4] log2 высоты; LINEAR: ширина
    input  wire [15:0] ps_tb,           //   LINEAR: высота
    input  wire [15:0] ps_tls,          //   LINEAR: шаг строки в байтах
    input  wire        ps_lslot,        // lightmap: слот, клеток в строке, строк (1…64), сдвиг, smin, tmin
    input  wire [6:0]  ps_lw,
    input  wire [6:0]  ps_lh,
    input  wire [3:0]  ps_lsh,
    input  wire [31:0] ps_lsmin,
    input  wire [31:0] ps_ltmin,
    input  wire [6:0]  ps_phase,        // фаза волны (команда TIME)
    // ---- общие настройки: меняются только при пустом конвейере (SETUP, FRAME)
    input  wire [22:0] fb_draw,         // рисуемый кадр (байтовый адрес)
    input  wire [22:0] zaddr,           // z-буфер
    input  wire [22:0] amap,            // alphamap 256×256
    input  wire [15:0] scr_s,           // шаг строки кадра в точках (W вверх до 16)
    // ---- таблицы из SETUP (конвейер пуст)
    input  wire        cmap_we,         // colormap: слово cmap_waddr = байты 4·cmap_waddr … +3
    input  wire [11:0] cmap_waddr,
    input  wire [31:0] cmap_wdata,
    input  wire        turb_we,         // синус волны
    input  wire [6:0]  turb_addr,
    input  wire [31:0] turb_data,
    // ---- загрузка lightmap в слот (ядро ждёт lm_busy = 0)
    input  wire        lm_go,
    input  wire        lm_slot,
    input  wire [22:0] lm_addr,
    input  wire [6:0]  lm_w,
    input  wire [6:0]  lm_h,
    output wire        lm_busy,
    // ---- обслуживание (только при пустом конвейере)
    input  wire        flush,           // такт: куски z и цвета — в SDRAM, сами куски забыть
    input  wire        inv,             // такт: кэши текстур и alphamap забыть
    output wire        busy,            // в конвейере точки, идёт запись, чтение, загрузка или сброс
    // ---- SDRAM: ответы моста, общие для всех клиентов (кому — говорят *_wr_take, *_rd ниже)
    input  wire [5:0]  m_wr_idx,
    input  wire [5:0]  m_rd_idx,
    input  wire [31:0] m_rd_data,
    // клиент «текстуры» (только чтение, своя быстрая очередь моста)
    output reg         tx_req,
    output reg  [20:0] tx_addr,
    input  wire        tx_ack,
    input  wire        tx_rd,
    // клиент «z»
    output wire        zb_req,
    output wire        zb_we,
    output wire [20:0] zb_addr,
    input  wire        zb_ack,
    input  wire        zb_wr_take,
    output wire [31:0] zb_wdata,
    output wire [3:0]  zb_wmask,
    input  wire        zb_rd,
    // клиент «цвет»
    output wire        co_req,
    output wire        co_we,
    output wire [20:0] co_addr,
    input  wire        co_ack,
    input  wire        co_wr_take,
    output wire [31:0] co_wdata,
    output wire [3:0]  co_wmask,
    input  wire        co_rd,
    // клиент «alphamap» (чтение)
    output reg         am_req,
    output reg  [20:0] am_addr,
    input  wire        am_ack,
    input  wire        am_rd,
    // клиент «lightmap» (чтение)
    output reg         lr_req,
    output reg  [20:0] lr_addr,
    input  wire        lr_ack,
    input  wire        lr_rd,
    // ---- стенду: причины «стопа»
    output wire [7:0]  dbg
);
    integer ii;

    // биты режима точки (ps_mode). Ядро переводит в них режим POLY (gpu/SPEC.md) и команды FILL/POINT/PIC:
    //   [1:0] свет: 0 — lightmap, 1 — вершины (l), 2, 3 — без света
    //   ZTEST — сравнивать z; ZWRITE — писать z; ZGT — сравнение строгое (>), иначе ≥;
    //   ZRAW — z точки = ps_k16 (небо: 0, заливка z: значение), иначе z = zenc(zi);
    //   WARP — волна; [8:7] прозрачность: 1 — 33 %, 2, 3 — 66 %; KEY — тексель 255 прозрачен;
    //   SOLID — цвет = ps_k16[7:0], текстуры нет; NOCOL — цвет не писать (заливка z)
    localparam int M_ZTEST = 2, M_ZWRITE = 3, M_ZGT = 4, M_ZRAW = 5, M_WARP = 6, M_KEY = 9,
                   M_SOLID = 10, M_NOCOL = 11;

    // =================================================================================================
    // Банки состояния многоугольника
    // =================================================================================================
    // (* syn_ramstyle = "registers" *) — держать в триггерах, не в распределённой памяти: у таких массивов
    // несколько читателей сразу (банк — у каждой ступени свой, кусок — точка и буфер записи), а память
    // Gowin копирует на каждого читателя (первая сборка этапа B: 363 блока SSRAM)
    (* syn_ramstyle = "registers" *) reg [11:0] bk_mode  [0:1];
    (* syn_ramstyle = "registers" *) reg [15:0] bk_k16   [0:1];
    (* syn_ramstyle = "registers" *) reg [22:0] bk_taddr [0:1];
    (* syn_ramstyle = "registers" *) reg        bk_tlin  [0:1];
    (* syn_ramstyle = "registers" *) reg [15:0] bk_ta    [0:1];
    (* syn_ramstyle = "registers" *) reg [15:0] bk_tb    [0:1];
    (* syn_ramstyle = "registers" *) reg [15:0] bk_tls   [0:1];
    (* syn_ramstyle = "registers" *) reg        bk_lslot [0:1];
    (* syn_ramstyle = "registers" *) reg [6:0]  bk_lw    [0:1];
    (* syn_ramstyle = "registers" *) reg [6:0]  bk_lh    [0:1];
    (* syn_ramstyle = "registers" *) reg [3:0]  bk_lsh   [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] bk_lsmin [0:1];
    (* syn_ramstyle = "registers" *) reg [31:0] bk_ltmin [0:1];
    (* syn_ramstyle = "registers" *) reg [6:0]  bk_phase [0:1];
    always @(posedge clk)
        if (ps_we) begin
            bk_mode[ps_tag]  <= ps_mode;
            bk_k16[ps_tag]   <= ps_k16;
            bk_taddr[ps_tag] <= ps_taddr;
            bk_tlin[ps_tag]  <= ps_tlin;
            bk_ta[ps_tag]    <= ps_ta;
            bk_tb[ps_tag]    <= ps_tb;
            bk_tls[ps_tag]   <= ps_tls;
            bk_lslot[ps_tag] <= ps_lslot;
            bk_lw[ps_tag]    <= ps_lw;
            bk_lh[ps_tag]    <= ps_lh;
            bk_lsh[ps_tag]   <= ps_lsh;
            bk_lsmin[ps_tag] <= ps_lsmin;
            bk_ltmin[ps_tag] <= ps_ltmin;
            bk_phase[ps_tag] <= ps_phase;
        end

    // =================================================================================================
    // Общий «стоп»: какая ступень не может закончить точку в этом такте
    // =================================================================================================
    wire st_d, st_e, st_f;
    wire adv = !(st_d || st_e || st_f);
    assign in_ready = adv;

    // «Плавающий» z: 5 бит порядка + 11 бит мантиссы, больше — ближе (как bz_zenc в модели).
    function [15:0] zenc (input [31:0] zi);
        integer i, e;
        begin
            e = 0;
            for (i = 0; i < 31; i = i + 1)
                if (zi[i]) e = i;
            if ($signed(zi) <= 0)
                zenc = 16'd0;
            else if (e < 11)
                zenc = zi[15:0];
            else
                zenc = (16'(e - 10) << 11) | (16'(zi >> (e - 11)) & 16'h7FF);
        end
    endfunction

    // =================================================================================================
    // Ступень A — вход
    // =================================================================================================
    reg        pa_v, pa_e, pa_g;
    reg [15:0] pa_x, pa_y;
    reg [31:0] pa_zi, pa_s, pa_t, pa_l;
    always @(posedge clk)
        if (rst)
            pa_v <= 1'b0;
        else if (adv) begin
            pa_v  <= in_valid;
            pa_e  <= in_end;
            pa_g  <= in_tag;
            pa_x  <= in_x;
            pa_y  <= in_y;
            pa_zi <= in_zi;
            pa_s  <= in_s;
            pa_t  <= in_t;
            pa_l  <= in_l;
        end

    // волна: s прибавляет синус по t, t — по s; индекс — целая часть (биты 22:16) плюс фаза, по модулю 128.
    // Адрес — от точки на входе, на такт раньше ступени A: таблица читается через выходной регистр
    // (OREG, ниже) и отдаёт слово к ступени B сразу после фронта. (Со словом прямо из BSRAM путь
    // «таблица → сумма → зажим → адрес текселя» не укладывался в такт — сборка этапа B 23.11.)
    wire [6:0]  i_ti = in_t[22:16] + bk_phase[in_tag];
    wire [6:0]  i_si = in_s[22:16] + bk_phase[in_tag];
    wire [11:0] a_md = bk_mode[pa_g];
    wire [15:0] a_z16 = a_md[M_ZRAW] ? bk_k16[pa_g] : zenc (pa_zi);
    // номер точки в кадре: строка · шаг + колонка (z — по 2 байта на точку, цвет — по байту)
    wire [31:0] a_pidx = {16'd0, pa_y} * {16'd0, scr_s} + {16'd0, pa_x};
    // lightmap: u = (s − smin) >> сдвиг; целая часть (клетка) — биты 31:16, вес — биты 15:12
    wire signed [31:0] a_lu = $signed(pa_s - bk_lsmin[pa_g]) >>> bk_lsh[pa_g];
    wire signed [31:0] a_lv = $signed(pa_t - bk_ltmin[pa_g]) >>> bk_lsh[pa_g];
    // свет вершин: строка colormap = l >> 24, зажатая в 0…63
    wire signed [7:0] a_lr = pa_l[31:24];
    wire [5:0]  a_vr = (a_lr < 0) ? 6'd0 : (a_lr > 8'sd63) ? 6'd63 : a_lr[5:0];

    // таблица волны: две копии — два чтения за такт; слово — к ступени B (адрес — у входа, выше)
    wire [31:0] pb_tus, pb_tut;
    bz_ram #(.AW(7), .DW(32), .OREG(1'b1)) u_turb_s (.clk(clk), .we(turb_we), .wa(turb_addr), .wd(turb_data),
                                                     .re(adv), .ra(i_ti), .rd(pb_tus));
    bz_ram #(.AW(7), .DW(32), .OREG(1'b1)) u_turb_t (.clk(clk), .we(turb_we), .wa(turb_addr), .wd(turb_data),
                                                     .re(adv), .ra(i_si), .rd(pb_tut));

    // =================================================================================================
    // Ступень B — волна, координаты текселя, адреса клеток lightmap
    // =================================================================================================
    reg        pb_v, pb_e, pb_g;
    reg [31:0] pb_s, pb_t;
    reg [23:0] pb_pidx;
    reg [15:0] pb_z16;
    reg [15:0] pb_iu, pb_iv;
    reg [3:0]  pb_fu, pb_fv;
    reg [5:0]  pb_vr;
    // Параметры банка этой точки — выбраны ещё по pa_g и едут с ней (24.11.2026, тайминг ядра 0.02): путь
    // «номер банка pb_g (85 нагрузок) → параметры банка → волна → зажим → строка текселя → вход DSP t_lmu»
    // был худшим (12 уровней, −1,3 нс). Банк пишется до первой своей точки и не переписывается, пока
    // последняя не прошла (p_rel), — значения для точки те же, что и были.
    reg [11:0] pb_md;
    reg [15:0] pb_ta, pb_tb;
    reg        pb_tlin, pb_lslot;
    reg [6:0]  pb_lw, pb_lh;
    always @(posedge clk)
        if (rst)
            pb_v <= 1'b0;
        else if (adv) begin
            pb_v    <= pa_v;
            pb_e    <= pa_e;
            pb_g    <= pa_g;
            pb_s    <= pa_s;
            pb_t    <= pa_t;
            pb_pidx <= a_pidx[23:0];
            pb_z16  <= a_z16;
            pb_iu   <= a_lu[31:16];
            pb_fu   <= a_lu[15:12];
            pb_iv   <= a_lv[31:16];
            pb_fv   <= a_lv[15:12];
            pb_vr   <= a_vr;
            pb_md   <= bk_mode[pa_g];
            pb_ta   <= bk_ta[pa_g];
            pb_tb   <= bk_tb[pa_g];
            pb_tlin <= bk_tlin[pa_g];
            pb_lslot <= bk_lslot[pa_g];
            pb_lw   <= bk_lw[pa_g];
            pb_lh   <= bk_lh[pa_g];
        end

    wire [11:0] b_md = pb_md;
    wire [31:0] b_ws = b_md[M_WARP] ? pb_s + pb_tus : pb_s;
    wire [31:0] b_wt = b_md[M_WARP] ? pb_t + pb_tut : pb_t;
    wire signed [15:0] b_x = b_ws[31:16];   // целая часть s (сдвиг со знаком на 16)
    wire signed [15:0] b_y = b_wt[31:16];
    // TILED: повтор по маске ширины и высоты (степени двойки)
    wire [3:0]  b_wl = pb_ta[3:0];
    wire [3:0]  b_hl = pb_ta[7:4];
    wire [15:0] b_ux = b_x & ((16'd1 << b_wl) - 16'd1);
    wire [15:0] b_uy = b_y & ((16'd1 << b_hl) - 16'd1);
    // LINEAR: края зажаты в [0, ширина − 1] × [0, высота − 1]
    wire signed [16:0] b_lw1 = $signed({1'b0, pb_ta}) - 17'sd1;
    wire signed [16:0] b_lh1 = $signed({1'b0, pb_tb}) - 17'sd1;
    wire [15:0] b_cx = (b_x < 0) ? 16'd0 : ($signed({b_x[15], b_x}) > b_lw1) ? b_lw1[15:0] : b_x;
    wire [15:0] b_cy = (b_y < 0) ? 16'd0 : ($signed({b_y[15], b_y}) > b_lh1) ? b_lh1[15:0] : b_y;
    wire [15:0] b_tx = pb_tlin ? b_cx : b_ux;
    wire [15:0] b_ty = pb_tlin ? b_cy : b_uy;

    // lightmap: клетка (iu, iv) и соседняя (iu1, iv1), зажатые в lightmap; слева/сверху за краем — вес 0
    wire [5:0]  b_lwm = 6'(pb_lw - 7'd1);
    wire [5:0]  b_lhm = 6'(pb_lh - 7'd1);
    wire        b_un  = pb_iu[15];
    wire        b_vn  = pb_iv[15];
    wire        b_ub  = !b_un && (pb_iu[15:6] != 10'd0 || pb_iu[5:0] > b_lwm);
    wire        b_vb  = !b_vn && (pb_iv[15:6] != 10'd0 || pb_iv[5:0] > b_lhm);
    wire [5:0]  b_iuc = b_un ? 6'd0 : b_ub ? b_lwm : pb_iu[5:0];
    wire [5:0]  b_ivc = b_vn ? 6'd0 : b_vb ? b_lhm : pb_iv[5:0];
    wire [3:0]  b_fuc = b_un ? 4'd0 : pb_fu;
    wire [3:0]  b_fvc = b_vn ? 4'd0 : pb_fv;
    wire [5:0]  b_iu1 = (b_iuc == b_lwm) ? b_iuc : b_iuc + 6'd1;
    wire [5:0]  b_iv1 = (b_ivc == b_lhm) ? b_ivc : b_ivc + 6'd1;
    // Четыре клетки лежат в четырёх банках по чётности: банк {чётность строки, чётность колонки}. Две
    // нужные колонки (iu и iu + 1) разной чётности, строки — тоже, поэтому четыре клетки — в четырёх
    // разных банках и читаются за один такт. Банку с чётностью колонки p нужна та из двух колонок, у
    // которой эта чётность; адрес в банке — {слот, строка / 2, колонка / 2}.
    wire [5:0]  b_c0 = (b_iuc[0] == 1'b0) ? b_iuc : b_iu1;   // колонка для банков чётных колонок
    wire [5:0]  b_c1 = (b_iuc[0] == 1'b1) ? b_iuc : b_iu1;   // нечётных
    wire [5:0]  b_r0 = (b_ivc[0] == 1'b0) ? b_ivc : b_iv1;
    wire [5:0]  b_r1 = (b_ivc[0] == 1'b1) ? b_ivc : b_iv1;
    wire        b_sl = pb_lslot;

    // =================================================================================================
    // Ступень T — начало строки текселя, адреса z и цвета, чтение четырёх клеток lightmap
    // =================================================================================================
    // Своя ступень с 23.11: в сборке этапа B цепочки «волна → зажим → адрес текселя» (B) и «сдвиг ряда
    // плиток → сумма → сумма → кэш» (C) не уложились в такт 60,75 МГц. Теперь здесь — начало строки
    // текселя и всё, что от неё не зависит, а в C остаётся одно сложение «начало + сдвиг».
    reg        pt_v, pt_e, pt_g;
    reg [15:0] pt_tx, pt_ty;
    reg [20:0] pt_la;                           // клетки lightmap: {слот, r0/2, r1/2, c0/2, c1/2}
    reg        pt_pu0, pt_pu1, pt_pv0, pt_pv1;  // чётность колонок iu, iu1 и строк iv, iv1
    reg [3:0]  pt_fu, pt_fv;
    reg [23:0] pt_pidx;
    reg [15:0] pt_z16;
    reg [5:0]  pt_vr;
    always @(posedge clk)
        if (rst)
            pt_v <= 1'b0;
        else if (adv) begin
            pt_v    <= pb_v;
            pt_e    <= pb_e;
            pt_g    <= pb_g;
            pt_tx   <= b_tx;
            pt_ty   <= b_ty;
            pt_la   <= {b_sl, b_r0[5:1], b_r1[5:1], b_c0[5:1], b_c1[5:1]};
            pt_pu0  <= b_iuc[0];
            pt_pu1  <= b_iu1[0];
            pt_pv0  <= b_ivc[0];
            pt_pv1  <= b_iv1[0];
            pt_fu   <= b_fuc;
            pt_fv   <= b_fvc;
            pt_pidx <= pb_pidx;
            pt_z16  <= pb_z16;
            pt_vr   <= pb_vr;
        end

    // Адрес текселя (как texel() модели) = начало строки + сдвиг в строке; оба — здесь, сумма — в C.
    //   LINEAR: начало = адрес текстуры + y · шаг строки, сдвиг = x.
    //   TILED, плитки 8 × 8 (ширина и высота ≥ 8): адрес = адрес текстуры + плитка · 64 + (y & 7) · 8 +
    //     (x & 7), плитка = (y / 8) · (плиток в ряду) + x / 8. Раскладка: начало = адрес текстуры +
    //     (y / 8) · (плиток в ряду) · 64 + (y & 7) · 8, сдвиг = (x / 8) · 64 + (x & 7) — у сдвига это
    //     только перестановка битов x: {x / 8, 000, x & 7}.
    //   TILED мелкая (меньше 8): строки подряд, начало = адрес текстуры + y · ширина, сдвиг = x.
    // (Всё по модулю 2^23, как и раньше: от перестановки слагаемых сумма не меняется.)
    wire [3:0]  t_wl   = bk_ta[pt_g][3:0];
    wire [3:0]  t_hl   = bk_ta[pt_g][7:4];
    wire        t_lin  = bk_tlin[pt_g];
    wire        t_big  = !t_lin && t_wl >= 4'd3 && t_hl >= 4'd3;
    wire [31:0] t_lmu  = {16'd0, pt_ty} * {16'd0, bk_tls[pt_g]};        // LINEAR: строка · шаг
    wire [22:0] t_tb1  = 23'(pt_ty >> 3) << (t_wl - 4'd3);              // TILED: ряд плиток · плиток в ряду
    wire [22:0] t_row  = t_lin ? t_lmu[22:0] : t_big ? {t_tb1[16:0], pt_ty[2:0], 3'b000} : 23'(pt_ty) << t_wl;
    wire [22:0] t_base = bk_taddr[pt_g] + t_row;
    wire [18:0] t_txo  = t_big ? {pt_tx[15:3], 3'b000, pt_tx[2:0]} : {3'd0, pt_tx};
    // адреса z и цвета этой точки в SDRAM: кусок (пачка 32 байта) и дорожка в нём
    wire [22:0] t_za = zaddr + {pt_pidx[21:0], 1'b0};
    wire [22:0] t_ca = fb_draw + pt_pidx[22:0];

    // загрузка lightmap пишет банки (ниже); чтение — четыре клетки этой точки (адреса — из ступени B)
    wire        l_we;
    wire [1:0]  l_bank;
    wire [10:0] l_wa;
    wire [7:0]  l_byte;
    wire [7:0]  pc_lb [0:3];
    bz_ram #(.AW(11), .DW(8)) u_lm0 (.clk(clk), .we(l_we && l_bank == 2'd0), .wa(l_wa), .wd(l_byte),
                                     .re(adv), .ra({pt_la[20], pt_la[19:15], pt_la[9:5]}), .rd(pc_lb[0]));
    bz_ram #(.AW(11), .DW(8)) u_lm1 (.clk(clk), .we(l_we && l_bank == 2'd1), .wa(l_wa), .wd(l_byte),
                                     .re(adv), .ra({pt_la[20], pt_la[19:15], pt_la[4:0]}), .rd(pc_lb[1]));
    bz_ram #(.AW(11), .DW(8)) u_lm2 (.clk(clk), .we(l_we && l_bank == 2'd2), .wa(l_wa), .wd(l_byte),
                                     .re(adv), .ra({pt_la[20], pt_la[14:10], pt_la[9:5]}), .rd(pc_lb[2]));
    bz_ram #(.AW(11), .DW(8)) u_lm3 (.clk(clk), .we(l_we && l_bank == 2'd3), .wa(l_wa), .wd(l_byte),
                                     .re(adv), .ra({pt_la[20], pt_la[14:10], pt_la[4:0]}), .rd(pc_lb[3]));

    // =================================================================================================
    // Ступень C — адрес текселя, чтение кэша текстур, смешивание клеток lightmap по колонке
    // =================================================================================================
    reg        pc_v, pc_e, pc_g;
    reg [22:0] pc_base;                         // начало строки текселя (ступень T)
    reg [18:0] pc_txo;                          // сдвиг текселя в строке
    reg        pc_pu0, pc_pu1, pc_pv0, pc_pv1;  // чётность колонок iu, iu1 и строк iv, iv1
    reg [3:0]  pc_fu, pc_fv;
    reg [17:0] pc_zch, pc_cch;                  // кусок z и кусок цвета (адрес пачки)
    reg [3:0]  pc_zln;                          // дорожка z в куске (16 значений)
    reg [4:0]  pc_cln;                          // дорожка цвета (32 байта)
    reg [15:0] pc_z16;
    reg [5:0]  pc_vr;
    always @(posedge clk)
        if (rst)
            pc_v <= 1'b0;
        else if (adv) begin
            pc_v    <= pt_v;
            pc_e    <= pt_e;
            pc_g    <= pt_g;
            pc_base <= t_base;
            pc_txo  <= t_txo;
            pc_pu0  <= pt_pu0;
            pc_pu1  <= pt_pu1;
            pc_pv0  <= pt_pv0;
            pc_pv1  <= pt_pv1;
            pc_fu   <= pt_fu;
            pc_fv   <= pt_fv;
            pc_zch  <= t_za[22:5];
            pc_zln  <= t_za[4:1];
            pc_cch  <= t_ca[22:5];
            pc_cln  <= t_ca[4:0];
            pc_z16  <= pt_z16;
            pc_vr   <= pt_vr;
        end

    // адрес текселя: начало строки + сдвиг (оба посчитаны в T)
    wire [22:0] c_ta  = pc_base + 23'(pc_txo);

    // кэш текстур: 128 наборов × 2 пути × строка 32 байта. Набор — биты адреса [11:5], метка — [22:12].
    // Метки двух путей набора — в одном слове: {есть1, метка1, есть0, метка0}.
    //
    // 27.11.2026 (ядро 0.04): попадание считается в отдельной ступени H, в D оно уже в регистре.
    // Раньше слово меток с выхода BSRAM (~2,3 нс после фронта) сравнивалось в D и в том же такте
    // решало st_d → adv, а adv — разрешение сотен регистров (буфер bz_skid, z-кусок, умножители):
    // в сборке 0.04 так шли 268 путей clk_mem, до −1,7 нс. Цена ступени: промах кэша оставляет в H
    // точку, прочитавшую метки ДО догрузки строки. Её чтение повторяется (t_rpl — такт после записи
    // метки, адрес — точки в H), иначе соседняя точка той же строки промахнулась бы ещё раз и
    // загрузила строку во второй путь. Промах длиннее на такт.
    wire        tg_we;
    wire [6:0]  tg_wa;
    wire [23:0] tg_wd;
    wire [23:0] h_tw;
    wire        td_we0, td_we1;
    wire [9:0]  td_wa;
    wire [31:0] h_w0, h_w1;
    reg         t_rpl;                                             // повторить чтение для точки в H
    reg  [22:0] ph_ta;
    wire [11:2] r_ta  = t_rpl ? ph_ta[11:2] : c_ta[11:2];
    wire        r_re  = adv || t_rpl;
    bz_ram #(.AW(7), .DW(24)) u_tag (.clk(clk), .we(tg_we), .wa(tg_wa), .wd(tg_wd),
                                     .re(r_re), .ra(r_ta[11:5]), .rd(h_tw));
    bz_ram #(.AW(10), .DW(32)) u_td0 (.clk(clk), .we(td_we0), .wa(td_wa), .wd(m_rd_data),
                                      .re(r_re), .ra(r_ta[11:2]), .rd(h_w0));
    bz_ram #(.AW(10), .DW(32)) u_td1 (.clk(clk), .we(td_we1), .wa(td_wa), .wd(m_rd_data),
                                      .re(r_re), .ra(r_ta[11:2]), .rd(h_w1));

    // lightmap по колонке: r0 = L(iu, iv)·(16 − fu) + L(iu1, iv)·fu, r1 — то же в строке iv1
    wire [7:0]  c_l00 = pc_lb[{pc_pv0, pc_pu0}];
    wire [7:0]  c_l10 = pc_lb[{pc_pv0, pc_pu1}];
    wire [7:0]  c_l01 = pc_lb[{pc_pv1, pc_pu0}];
    wire [7:0]  c_l11 = pc_lb[{pc_pv1, pc_pu1}];
    wire [4:0]  c_gu  = 5'd16 - {1'b0, pc_fu};
    // (операнды расширены до 12 бит заранее: ширина умножения в Verilog — по самому широкому операнду)
    wire [11:0] c_r0  = {4'd0, c_l00} * {7'd0, c_gu} + {4'd0, c_l10} * {8'd0, pc_fu};
    wire [11:0] c_r1  = {4'd0, c_l01} * {7'd0, c_gu} + {4'd0, c_l11} * {8'd0, pc_fu};

    // =================================================================================================
    // Ступень H — метки и слова кэша текстур с выхода BSRAM: попадание; строка света
    // =================================================================================================
    reg        ph_v, ph_e, ph_g;
    reg [11:0] ph_r0, ph_r1;
    reg [3:0]  ph_fv;
    reg [17:0] ph_zch, ph_cch;
    reg [3:0]  ph_zln;
    reg [4:0]  ph_cln;
    reg [15:0] ph_z16;
    reg [5:0]  ph_vr;
    always @(posedge clk)
        if (rst)
            ph_v <= 1'b0;
        else if (adv) begin
            ph_v   <= pc_v;
            ph_e   <= pc_e;
            ph_g   <= pc_g;
            ph_ta  <= c_ta;
            ph_r0  <= c_r0;
            ph_r1  <= c_r1;
            ph_fv  <= pc_fv;
            ph_zch <= pc_zch;
            ph_cch <= pc_cch;
            ph_zln <= pc_zln;
            ph_cln <= pc_cln;
            ph_z16 <= pc_z16;
            ph_vr  <= pc_vr;
        end

    wire [11:0] h_md   = bk_mode[ph_g];
    wire [10:0] h_tg   = ph_ta[22:12];
    wire        h_h0   = h_tw[11] && h_tw[10:0] == h_tg;
    wire        h_h1   = h_tw[23] && h_tw[22:12] == h_tg;
    // строка света: (r0·(16 − fv) + r1·fv) >> 10 — 0…63
    wire [4:0]  h_gv   = 5'd16 - {1'b0, ph_fv};
    wire [16:0] h_val  = {5'd0, ph_r0} * {12'd0, h_gv} + {5'd0, ph_r1} * {13'd0, ph_fv};
    wire [5:0]  h_row  = (h_md[1:0] == 2'd0) ? h_val[15:10] : ph_vr;

    // =================================================================================================
    // Ступень D — промах кэша текстур: стоп и догрузка строки
    // =================================================================================================
    reg        pd_v, pd_e, pd_g;
    reg [22:0] pd_ta;
    reg [23:0] pd_tw;                          // метки набора (для выбора пути и записи новой)
    reg        pd_h0, pd_h1;                   // попадание в путь 0 / 1
    reg [31:0] pd_wd;                          // слово из попавшего пути
    reg [5:0]  pd_row;
    reg [17:0] pd_zch, pd_cch;
    reg [3:0]  pd_zln;
    reg [4:0]  pd_cln;
    reg [15:0] pd_z16;
    always @(posedge clk)
        if (rst)
            pd_v <= 1'b0;
        else if (adv) begin
            pd_v   <= ph_v;
            pd_e   <= ph_e;
            pd_g   <= ph_g;
            pd_ta  <= ph_ta;
            pd_tw  <= h_tw;
            pd_h0  <= h_h0;
            pd_h1  <= h_h1;
            pd_wd  <= h_h0 ? h_w0 : h_w1;
            pd_row <= h_row;
            pd_zch <= ph_zch;
            pd_cch <= ph_cch;
            pd_zln <= ph_zln;
            pd_cln <= ph_cln;
            pd_z16 <= ph_z16;
        end

    wire [11:0] d_md   = bk_mode[pd_g];
    wire        d_need = pd_v && !pd_e && !d_md[M_SOLID];          // точке нужен тексель
    wire [6:0]  d_set  = pd_ta[11:5];
    wire [10:0] d_tg   = pd_ta[22:12];
    reg         t_fwd;                                             // строка догружена: слово — из t_fw
    reg  [31:0] t_fw;
    wire [31:0] d_word = t_fwd ? t_fw : pd_wd;
    wire [7:0]  d_tex  = 8'(d_word >> {pd_ta[1:0], 3'b000});
    assign st_d = d_need && !pd_h0 && !pd_h1 && !t_fwd;
    wire [5:0]  d_row  = pd_row;

    // промах — строка из SDRAM (8 слов) в вытесняемый путь; нужное слово — сразу точке (t_fwd)
    localparam [1:0] T_IDLE = 0, T_REQ = 1, T_DATA = 2, T_DONE = 3;
    reg [1:0]  t_st;
    reg        t_way;
    reg [3:0]  t_n;
    reg        tc_lru [0:127];                 // какой путь набора вытеснять следующим
    reg        i_on;                           // идёт сброс кэша (inv): метки набора i_n — в ноль
    reg [6:0]  i_n;
    wire       t_last = t_st == T_DATA && tx_rd && t_n == 4'd7;
    assign tg_we  = i_on || t_last;
    assign tg_wa  = i_on ? i_n : d_set;
    assign tg_wd  = i_on ? 24'd0 : t_way ? {1'b1, d_tg, pd_tw[11:0]} : {pd_tw[23:12], 1'b1, d_tg};
    assign td_we0 = t_st == T_DATA && tx_rd && !t_way;
    assign td_we1 = t_st == T_DATA && tx_rd && t_way;
    assign td_wa  = {d_set, m_rd_idx[2:0]};
    always @(posedge clk) begin
        if (rst) begin
            t_st   <= T_IDLE;
            t_fwd  <= 1'b0;
            t_rpl  <= 1'b0;
            tx_req <= 1'b0;
            i_on   <= 1'b0;
        end else begin
            case (t_st)
            T_IDLE:
                if (st_d) begin
                    tx_req  <= 1'b1;
                    tx_addr <= {pd_ta[22:5], 3'd0};
                    // свободный путь, если есть; иначе — давно не нужный
                    t_way   <= !pd_tw[11] ? 1'b0 : !pd_tw[23] ? 1'b1 : tc_lru[d_set];
                    t_n     <= 4'd0;
                    t_st    <= T_REQ;
                end
            T_REQ:
                if (tx_ack) begin
                    tx_req <= 1'b0;
                    t_st   <= T_DATA;
                end
            T_DATA:
                if (tx_rd) begin
                    if (m_rd_idx[2:0] == pd_ta[4:2])
                        t_fw <= m_rd_data;
                    t_n <= t_n + 1'b1;
                    if (t_n == 4'd7) begin
                        tc_lru[d_set] <= ~t_way;
                        t_rpl <= 1'b1;                 // метки записаны: точке в H — прочесть заново
                        t_st  <= T_DONE;
                    end
                end
            default:                                   // T_DONE: такт повтора чтения, потом точка возьмёт слово и уйдёт
                if (t_rpl) begin
                    t_rpl <= 1'b0;                     // st_d ещё 1 (t_fwd = 0): adv в этом такте нет
                    t_fwd <= 1'b1;
                end else if (adv) begin
                    t_fwd <= 1'b0;
                    t_st  <= T_IDLE;
                end
            endcase
            // попадание: следующим вытеснять другой путь
            if (adv && d_need && !t_fwd && (pd_h0 || pd_h1))
                tc_lru[d_set] <= pd_h0;
            // сброс кэша: 128 наборов по одному за такт
            if (inv) begin
                i_on <= 1'b1;
                i_n  <= 7'd0;
            end else if (i_on) begin
                i_n <= i_n + 1'b1;
                if (i_n == 7'd127)
                    i_on <= 1'b0;
            end
        end
    end

    // =================================================================================================
    // Ступень E — кусок z: тест и запись; ключ; чтение colormap
    // =================================================================================================
    reg        pe_v, pe_e, pe_g;
    reg [7:0]  pe_tex;
    reg [5:0]  pe_row;
    reg [17:0] pe_zch, pe_cch;
    reg [3:0]  pe_zln;
    reg [4:0]  pe_cln;
    reg [15:0] pe_z16;
    always @(posedge clk)
        if (rst)
            pe_v <= 1'b0;
        else if (adv) begin
            pe_v   <= pd_v;
            pe_e   <= pd_e;
            pe_g   <= pd_g;
            pe_tex <= d_tex;
            pe_row <= d_row;
            pe_zch <= pd_zch;
            pe_cch <= pd_cch;
            pe_zln <= pd_zln;
            pe_cln <= pd_cln;
            pe_z16 <= pd_z16;
        end

    // colormap 64 × 256: четыре банка по младшим битам текселя, у каждого слово {строка, тексель[7:2]}
    wire [7:0] pf_cm [0:3];
    genvar gc;
    generate for (gc = 0; gc < 4; gc = gc + 1) begin : g_cmap
        bz_ram #(.AW(12), .DW(8)) u_cm (.clk(clk), .we(cmap_we), .wa(cmap_waddr), .wd(cmap_wdata[8 * gc +: 8]),
                                        .re(adv), .ra({pe_row, pe_tex[7:2]}), .rd(pf_cm[gc]));
    end endgenerate

    // кусок z: 16 значений, адрес пачки z_cur
    reg [17:0] z_cur;
    reg        z_have, z_cv, z_fl;
    reg [15:0] z_dm;
    (* syn_ramstyle = "registers" *) reg [15:0] z_d [0:15];
    // буфер записи вытесненного куска
    reg [17:0] zw_ch;
    (* syn_ramstyle = "registers" *) reg [15:0] zw_d [0:15];
    reg [15:0] zw_m;
    reg        zw_req, zw_busy;
    reg [3:0]  zw_n;
    // чтение куска
    reg        zr_req, zr_busy;
    reg [3:0]  zr_n;

    wire [11:0] e_md   = bk_mode[pe_g];
    wire        e_pix  = pe_v && !pe_e;
    wire        e_zt   = e_md[M_ZTEST];
    wire        e_zneed = e_pix && (e_zt || e_md[M_ZWRITE]);
    wire        e_zhit = z_have && z_cur == pe_zch;
    wire        e_lok  = z_cv || z_dm[pe_zln];                    // значение дорожки известно
    wire [15:0] e_zb   = z_d[pe_zln];
    wire        e_zpass = !e_zt || (e_md[M_ZGT] ? pe_z16 > e_zb : pe_z16 >= e_zb);
    wire        e_key  = e_md[M_KEY] && !e_md[M_SOLID] && pe_tex == 8'hFF;
    wire        e_kill = (e_zneed && !e_zpass) || e_key;
    wire        e_zw   = e_zneed && e_md[M_ZWRITE] && !e_kill;
    assign st_e = e_zneed && (!e_zhit || (e_zt && !e_lok));
    wire        z_dirty = z_have && z_dm != 16'd0;
    wire        z_sw    = (e_zneed && !e_zhit) || z_fl;            // нужен другой кусок (или сброс)

    assign zb_req   = zw_req || zr_req;
    assign zb_we    = zw_req;                                      // запись поставлена раньше чтения
    assign zb_addr  = zw_req ? {zw_ch, 3'd0} : {z_cur, 3'd0};
    assign zb_wdata = {zw_d[{m_wr_idx[2:0], 1'b1}], zw_d[{m_wr_idx[2:0], 1'b0}]};
    assign zb_wmask = {{2{zw_m[{m_wr_idx[2:0], 1'b1}]}}, {2{zw_m[{m_wr_idx[2:0], 1'b0}]}}};

    always @(posedge clk) begin
        if (rst) begin
            z_have <= 1'b0; z_cv <= 1'b0; z_fl <= 1'b0; z_dm <= 16'd0;
            zw_req <= 1'b0; zw_busy <= 1'b0; zr_req <= 1'b0; zr_busy <= 1'b0;
        end else begin
            if (flush)
                z_fl <= 1'b1;
            if (z_sw && !zr_busy && (!z_dirty || !zw_busy)) begin
                // смена куска: записанное — в буфер записи (мост заберёт), кусок — под новую пачку
                if (z_dirty) begin
                    for (ii = 0; ii < 16; ii = ii + 1)
                        zw_d[ii] <= z_d[ii];
                    zw_m    <= z_dm;
                    zw_ch   <= z_cur;
                    zw_req  <= 1'b1;
                    zw_busy <= 1'b1;
                    zw_n    <= 4'd0;
                end
                if (z_fl) begin
                    z_have <= 1'b0;
                    z_fl   <= 1'b0;
                end else begin
                    z_have <= 1'b1;
                    z_cur  <= pe_zch;
                end
                z_dm <= 16'd0;
                z_cv <= 1'b0;
            end else if (e_zneed && e_zhit && e_zt && !e_lok && !zr_busy) begin
                // z-тесту нужно старое значение — прочитать пачку
                zr_req  <= 1'b1;
                zr_busy <= 1'b1;
                zr_n    <= 4'd0;
            end
            if (zb_ack) begin
                if (zw_req) zw_req <= 1'b0;
                else        zr_req <= 1'b0;
            end
            if (zb_wr_take) begin
                zw_n <= zw_n + 1'b1;
                if (zw_n == 4'd7)
                    zw_busy <= 1'b0;
            end
            // слова чтения — только в дорожки, которые ещё не писались (записанное новее памяти)
            if (zb_rd) begin
                if (!z_dm[{m_rd_idx[2:0], 1'b0}]) z_d[{m_rd_idx[2:0], 1'b0}] <= m_rd_data[15:0];
                if (!z_dm[{m_rd_idx[2:0], 1'b1}]) z_d[{m_rd_idx[2:0], 1'b1}] <= m_rd_data[31:16];
                zr_n <= zr_n + 1'b1;
                if (zr_n == 4'd7) begin
                    zr_busy <= 1'b0;
                    z_cv    <= 1'b1;
                end
            end
            // точка уходит из E — её z в кусок
            if (adv && e_zw) begin
                z_d[pe_zln]  <= pe_z16;
                z_dm[pe_zln] <= 1'b1;
            end
        end
    end

    // =================================================================================================
    // Ступень F — цвет: colormap, прозрачность, кусок цвета
    // =================================================================================================
    reg        pf_v, pf_e, pf_g, pf_kill;
    reg [7:0]  pf_tex;
    reg [17:0] pf_cch;
    reg [4:0]  pf_cln;
    always @(posedge clk)
        if (rst)
            pf_v <= 1'b0;
        else if (adv) begin
            pf_v    <= pe_v;
            pf_e    <= pe_e;
            pf_g    <= pe_g;
            pf_kill <= e_kill;
            pf_tex  <= pe_tex;
            pf_cch  <= pe_cch;
            pf_cln  <= pe_cln;
        end

    // кусок цвета: 32 точки, адрес пачки c_cur
    reg [17:0] c_cur;
    reg        c_have, c_cv, c_fl;
    reg [31:0] c_dm;
    (* syn_ramstyle = "registers" *) reg [7:0]  c_d [0:31];
    reg [17:0] cw_ch;
    (* syn_ramstyle = "registers" *) reg [7:0]  cw_d [0:31];
    reg [31:0] cw_m;
    reg        cw_req, cw_busy;
    reg [3:0]  cw_n;
    reg        cr_req, cr_busy;
    reg [3:0]  cr_n;

    wire [11:0] f_md   = bk_mode[pf_g];
    wire [1:0]  f_tr   = f_md[8:7];
    wire [7:0]  f_cmv  = pf_cm[pf_tex[1:0]];
    wire [7:0]  f_src  = f_md[M_SOLID] ? bk_k16[pf_g][7:0] : (f_md[1] == 1'b0) ? f_cmv : pf_tex;
    wire        f_cneed = pf_v && !pf_e && !pf_kill && !f_md[M_NOCOL];
    wire        f_chit = c_have && c_cur == pf_cch;
    wire        f_lok  = c_cv || c_dm[pf_cln];
    wire [7:0]  f_dst  = c_d[pf_cln];
    // alphamap: 33 % — amap[src + dst·256], 66 % — amap[src·256 + dst]
    wire [15:0] f_off  = (f_tr == 2'd1) ? {f_dst, f_src} : {f_src, f_dst};
    wire [22:0] f_aa   = amap + {7'd0, f_off};

    // кэш alphamap: 64 строки по 32 байта, прямое отображение; строка — биты [10:5], метка — [22:11]
    localparam [1:0] A_IDLE = 0, A_LOOK = 1, A_FILL = 2, A_RDY = 3;
    reg [1:0]  a_st;
    reg [22:0] a_aa;
    reg [3:0]  a_n;
    reg [7:0]  a_mix;
    reg [63:0] am_v;
    wire [10:0] a_ra = (a_st == A_IDLE) ? f_aa[10:0] : a_aa[10:0];
    wire        a_we = a_st == A_FILL && am_rd;
    wire        a_tlast = a_we && a_n == 4'd7;
    wire [31:0] a_q;
    wire [11:0] a_tq;
    bz_ram #(.AW(9), .DW(32)) u_amd (.clk(clk), .we(a_we), .wa({a_aa[10:5], m_rd_idx[2:0]}), .wd(m_rd_data),
                                     .re(1'b1), .ra(a_ra[10:2]), .rd(a_q));
    bz_ram #(.AW(6), .DW(12)) u_amt (.clk(clk), .we(a_tlast), .wa(a_aa[10:5]), .wd(a_aa[22:11]),
                                     .re(1'b1), .ra(a_ra[10:5]), .rd(a_tq));
    wire        a_hit  = a_st == A_LOOK && am_v[a_aa[10:5]] && a_tq == a_aa[22:11];
    wire [7:0]  a_byte = 8'(a_q >> {a_aa[1:0], 3'b000});
    wire [7:0]  f_mix  = (a_st == A_RDY) ? a_mix : a_byte;
    wire        f_aok  = a_hit || a_st == A_RDY;
    wire [7:0]  f_out  = (f_tr != 2'd0) ? f_mix : f_src;
    assign st_f = f_cneed && (!f_chit || (f_tr != 2'd0 && (!f_lok || !f_aok)));
    // Запись цвета точки — один раз, как только F сама готова (st_f = 0), а не в такт общего adv
    // (24.11.2026, ядро 0.03). Раньше разрешение 32 байт куска шло от adv, а в adv — сравнение меток кэша
    // текстур ступени D (выход BSRAM → сравнение → st_d → adv → c_d: 13 уровней, −0,2 нс при 60,75 МГц).
    // Значение то же: пока точка стоит, всё, от чего зависит f_out, неподвижно (pf_* держит adv, colormap
    // читается по adv, alphamap — по a_aa, кусок не сменится: c_sw нужен только промаху этой же точки).
    // f_done — точка уже записана и ждёт adv; при adv сбрасывается для следующей точки.
    reg         f_done;
    wire        f_wr = f_cneed && !st_f && !f_done;
    wire        c_dirty = c_have && c_dm != 32'd0;
    wire        c_sw    = (f_cneed && !f_chit) || c_fl;

    assign co_req   = cw_req || cr_req;
    assign co_we    = cw_req;
    assign co_addr  = cw_req ? {cw_ch, 3'd0} : {c_cur, 3'd0};
    assign co_wdata = {cw_d[{m_wr_idx[2:0], 2'd3}], cw_d[{m_wr_idx[2:0], 2'd2}],
                       cw_d[{m_wr_idx[2:0], 2'd1}], cw_d[{m_wr_idx[2:0], 2'd0}]};
    assign co_wmask = cw_m[{m_wr_idx[2:0], 2'd0} +: 4];

    always @(posedge clk) begin
        rel <= 1'b0;
        if (rst) begin
            c_have <= 1'b0; c_cv <= 1'b0; c_fl <= 1'b0; c_dm <= 32'd0;
            cw_req <= 1'b0; cw_busy <= 1'b0; cr_req <= 1'b0; cr_busy <= 1'b0;
            a_st <= A_IDLE; am_req <= 1'b0; am_v <= 64'd0; f_done <= 1'b0;
        end else begin
            if (flush)
                c_fl <= 1'b1;
            if (c_sw && !cr_busy && (!c_dirty || !cw_busy)) begin
                if (c_dirty) begin
                    for (ii = 0; ii < 32; ii = ii + 1)
                        cw_d[ii] <= c_d[ii];
                    cw_m    <= c_dm;
                    cw_ch   <= c_cur;
                    cw_req  <= 1'b1;
                    cw_busy <= 1'b1;
                    cw_n    <= 4'd0;
                end
                if (c_fl) begin
                    c_have <= 1'b0;
                    c_fl   <= 1'b0;
                end else begin
                    c_have <= 1'b1;
                    c_cur  <= pf_cch;
                end
                c_dm <= 32'd0;
                c_cv <= 1'b0;
            end else if (f_cneed && f_chit && f_tr != 2'd0 && !f_lok && !cr_busy) begin
                // прозрачной точке нужен цвет под ней — прочитать пачку кадра
                cr_req  <= 1'b1;
                cr_busy <= 1'b1;
                cr_n    <= 4'd0;
            end
            if (co_ack) begin
                if (cw_req) cw_req <= 1'b0;
                else        cr_req <= 1'b0;
            end
            if (co_wr_take) begin
                cw_n <= cw_n + 1'b1;
                if (cw_n == 4'd7)
                    cw_busy <= 1'b0;
            end
            if (co_rd) begin
                for (ii = 0; ii < 4; ii = ii + 1)
                    if (!c_dm[{m_rd_idx[2:0], 2'(ii)}])
                        c_d[{m_rd_idx[2:0], 2'(ii)}] <= m_rd_data[8 * ii +: 8];
                cr_n <= cr_n + 1'b1;
                if (cr_n == 4'd7) begin
                    cr_busy <= 1'b0;
                    c_cv    <= 1'b1;
                end
            end

            // прозрачность: такт 1 — адрес в alphamap (по цвету под точкой), такт 2 — попадание
            case (a_st)
            A_IDLE:
                if (f_cneed && f_chit && f_tr != 2'd0 && f_lok) begin
                    a_aa <= f_aa;
                    a_st <= A_LOOK;
                end
            A_LOOK:
                if (!a_hit) begin
                    am_req  <= 1'b1;
                    am_addr <= {a_aa[22:5], 3'd0};
                    a_n     <= 4'd0;
                    a_st    <= A_FILL;
                end
            A_FILL: begin
                if (am_ack)
                    am_req <= 1'b0;
                if (am_rd) begin
                    if (m_rd_idx[2:0] == a_aa[4:2])
                        a_mix <= 8'(m_rd_data >> {a_aa[1:0], 3'b000});
                    a_n <= a_n + 1'b1;
                    if (a_n == 4'd7) begin
                        am_v[a_aa[10:5]] <= 1'b1;
                        a_st <= A_RDY;
                    end
                end
            end
            default: ;                               // A_RDY: точка возьмёт a_mix
            endcase
            if (inv)
                am_v <= 64'd0;

            // цвет точки — в кусок (один раз, см. f_wr); точка уходит из F — метка конца: банк свободен
            if (f_wr) begin
                c_d[pf_cln]  <= f_out;
                c_dm[pf_cln] <= 1'b1;
            end
            if (adv)
                f_done <= 1'b0;
            else if (f_wr)
                f_done <= 1'b1;
            if (adv) begin
                if (a_st == A_LOOK || a_st == A_RDY)
                    a_st <= A_IDLE;
                if (pf_v && pf_e) begin
                    rel     <= 1'b1;
                    rel_tag <= pf_g;
                end
            end
        end
    end

    // =================================================================================================
    // Загрузка lightmap: пачки из SDRAM, байт за такт — в банк по чётности строки и колонки
    // =================================================================================================
    localparam [1:0] L_IDLE = 0, L_REQ = 1, L_DATA = 2, L_SCAT = 3;
    reg [1:0]  l_st;
    reg        l_slot;
    reg [17:0] l_line;                         // пачка, которую читаем
    reg [4:0]  l_p;                            // байт в ней
    reg [5:0]  l_u, l_v;                       // клетка, которую пишем
    reg [6:0]  l_w, l_h;
    (* syn_ramstyle = "distributed_ram" *) reg [31:0] l_buf [0:7];   // пачка: одно чтение за такт — память
    reg [3:0]  l_n;
    wire       l_lastb = {1'b0, l_u} == l_w - 7'd1 && {1'b0, l_v} == l_h - 7'd1;
    assign lm_busy = l_st != L_IDLE;
    assign l_we    = l_st == L_SCAT;
    assign l_bank  = {l_v[0], l_u[0]};
    assign l_wa    = {l_slot, l_v[5:1], l_u[5:1]};
    assign l_byte  = 8'(l_buf[l_p[4:2]] >> {l_p[1:0], 3'b000});
    always @(posedge clk) begin
        if (rst) begin
            l_st   <= L_IDLE;
            lr_req <= 1'b0;
        end else
            case (l_st)
            L_IDLE:
                if (lm_go) begin
                    l_slot  <= lm_slot;
                    l_line  <= lm_addr[22:5];
                    l_p     <= lm_addr[4:0];
                    l_u     <= 6'd0;
                    l_v     <= 6'd0;
                    l_w     <= lm_w;
                    l_h     <= lm_h;
                    lr_req  <= 1'b1;
                    lr_addr <= {lm_addr[22:5], 3'd0};
                    l_st    <= L_REQ;
                end
            L_REQ:
                if (lr_ack) begin
                    lr_req <= 1'b0;
                    l_n    <= 4'd0;
                    l_st   <= L_DATA;
                end
            L_DATA:
                if (lr_rd) begin
                    l_buf[m_rd_idx[2:0]] <= m_rd_data;
                    l_n <= l_n + 1'b1;
                    if (l_n == 4'd7)
                        l_st <= L_SCAT;
                end
            default: begin                       // L_SCAT: байт l_p ушёл в банк — следующий
                if ({1'b0, l_u} == l_w - 7'd1) begin
                    l_u <= 6'd0;
                    l_v <= l_v + 1'b1;
                end else
                    l_u <= l_u + 1'b1;
                l_p <= l_p + 1'b1;
                if (l_lastb)
                    l_st <= L_IDLE;
                else if (l_p == 5'd31) begin
                    l_line  <= l_line + 1'b1;
                    lr_req  <= 1'b1;
                    lr_addr <= {l_line + 18'd1, 3'd0};
                    l_st    <= L_REQ;
                end
            end
            endcase
    end

    assign busy = pa_v || pb_v || pt_v || pc_v || ph_v || pd_v || pe_v || pf_v || rel || z_fl || c_fl || zw_busy || cw_busy ||
                  zr_busy || cr_busy || t_st != T_IDLE || a_st != A_IDLE || i_on || l_st != L_IDLE;
    assign dbg = {zw_busy, cw_busy, a_st != A_IDLE, st_f, st_e, st_d, in_valid, pf_v && !pf_e};
endmodule
