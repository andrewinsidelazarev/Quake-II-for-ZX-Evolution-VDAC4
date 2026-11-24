// =====================================================================================================
// bz_core.sv — растеризатор «бомж-3D», этап A: автомат, который рисует точку за точкой, бит в бит
// как C-модель gpu/model/bomzh.c.
// =====================================================================================================
//
// ЧТО ДЕЛАЕТ РАСТЕРИЗАТОР
//   «Растеризовать» — превратить фигуру, заданную вершинами, в набор точек (пикселей) экрана. ESP уже
//   посчитал, где на экране вершины стены и как по ней «натянута» текстура; растеризатор перебирает
//   строки экрана, в каждой строке — точки внутри фигуры, и для каждой точки:
//     1) считает её глубину и сравнивает с z-буфером (не закрыта ли точка чем-то ближним);
//     2) находит точку текстуры (тексель), которая сюда ложится;
//     3) освещает его (свет из lightmap или из вершин модели) через таблицу colormap Quake;
//     4) при прозрачности смешивает с тем, что уже лежит в кадре (таблица alphamap Quake);
//     5) пишет цвет в кадр и глубину в z-буфер.
//   Входные команды — gpu/SPEC.md (POLY, POINT, PIC, FILL, FRAME и др.), они идут из очереди bz_fifo.
//
// ЧИСЛА С ФИКСИРОВАННОЙ ТОЧКОЙ (Q-формат)
//   Плавающей точки в ПЛИС нет — дробные числа хранятся целыми, «с запятой в условленном месте».
//   Qm.n — m бит целой части и n бит дробной: число x хранится как x · 2^n. Пример: Q16.16, x = 2,5 →
//   2,5 · 65536 = 163840. Сложение — обычное сложение целых. Умножение Qa.b × Qc.d даёт Q(a+c).(b+d), и
//   чтобы вернуться к нужному формату, лишние дробные биты отбрасывают сдвигом вправо.
//   Здесь: координаты вершин — Q12.4 (1/16 точки), координаты текстуры s, t — Q16.16, zi = 1/z — Q2.30.
//
// ГРАДИЕНТ: как получить значение в любой точке многоугольника
//   Многоугольник плоский, поэтому многие величины на экране меняются линейно: значение в точке (x, y)
//   равно C + A·x + B·y. Тройку {C, A, B} (значение в углу экрана и прибавки на шаг по x и по y) ESP
//   присылает в команде POLY. Тогда соседняя точка справа — просто «+A», без умножений. Арифметика —
//   по модулю 2^32 (переполнение «заворачивается»), так же в модели, поэтому совпадение точное.
//
// ПЕРСПЕКТИВА
//   Текстура на наклонной стене линейна не по экрану, а по 1/z. Поэтому линейно идут s/z, t/z и 1/z, а
//   настоящие s и t получаются делением: s = (s/z) / (1/z). Делить в каждой точке дорого, поэтому — как
//   в программном Quake — делим только на концах куска строки из 16 точек, а внутри куска s и t идут
//   по прямой (линейно). Деление — точное: w = floor(2^46 / zi), потом s = (su · w) >> 20.
//
// Z-БУФЕР
//   Для каждой точки кадра хранится глубина последнего нарисованного в ней (16 бит). Новая точка
//   рисуется, только если она ближе. Храним не z, а «плавающее» 1/z (5 бит порядка + 11 бит мантиссы,
//   функция zenc): так точность одинакова и вблизи, и вдали.
//
// LIGHTMAP И COLORMAP
//   Освещённость стены Quake хранит сеткой «клеток» по 16×16 текселей (lightmap). В каждой точке берём
//   4 соседние клетки и смешиваем их по дробной части координаты (билинейная интерполяция) — получаем
//   номер строки 0…63 таблицы colormap. Colormap (64 строки × 256 цветов) говорит, каким индексом
//   палитры станет цвет текселя при такой освещённости. Так программный Quake 2 и освещал стены.
//
// ПОЧЕМУ ЭТО «ЭТАП A»
//   Здесь каждая точка проходит автомат шаг за шагом (~25 тактов на точку), память — простой порт
//   «запрос/ответ» по 32-битным словам. Цель — точность, а не скорость: стенд gpu/rtl/tb гонит поток
//   команд, записанный из настоящего Quake 2 (ref_bz, cvar bz_trace), и после каждого кадра сравнивает
//   всю память RTL с памятью C-модели байт в байт. Быстрое ядро (этап B, gpu/rtl/ARCH.md) — конвейер
//   «точка за такт» с теми же формулами; этот файл — образец, с которым оно будет сверяться.
//
// КАК ЧИТАТЬ ФАЙЛ
//   1) функции арифметики (grad, zenc, persp_mul, step_of, tex_addr) — это и есть SPEC.md в железе;
//   2) автомат: S_IDLE/S_GET принимают слова команды, S_EXEC разбирает её, дальше — свои состояния у
//      каждой команды: S_P_* — многоугольник (строка → кусок → точка), S_PT_* — частица, S_PIC_*,
//      S_FILL_*; S_MEM — общее ожидание ответа памяти (задачи rd, wr8, wr16 его вызывают).
//
// КЛЮЧ BZ_NO3D
//   Без POLY и POINT (их слова принимаются и пропускаются) — для пробной сборки оболочки в Gowin: в этом
//   автомате много умножителей 32×32 (так проще читать), в железо пойдёт конвейер этапа B.
//
// ПОРТ ПАМЯТИ
//   mem_req держится до mem_ack (ответ — один такт); на mem_ack для чтения действует mem_rdata. Адрес
//   байтовый (8 МБ = 2^23), номер 32-битного слова — mem_addr[22:2], какие байты писать — mem_be.
module bz_core (
    input  wire        clk,
    input  wire        rst,
    // поток команд
    input  wire [31:0] cmd_data,
    input  wire        cmd_valid,
    output wire        cmd_ready,
    // память
    output reg         mem_req,
    output reg         mem_we,
    output reg  [22:0] mem_addr,
    output reg  [31:0] mem_wdata,
    output reg  [3:0]  mem_be,
    input  wire        mem_ack,
    input  wire [31:0] mem_rdata,
    // вывод кадра
    output reg  [22:0] shown_fb,        // адрес показанного кадра (после FRAME)
    output reg  [31:0] shown_blend,     // вспышка: R, G, B, доля
    output reg         frame_pulse,     // такт на каждом FRAME
    input  wire        swap_ack,        // вывод взял новый кадр: можно рисовать в старый
    output reg         bad_cmd,         // встретилось неизвестное слово (липкий)
    // палитра вывода (SETUP): байт за такт в RAM блока вывода (bz_scan), индекс 0…767 = цвет·3 + канал
    output reg         pal_we,
    output reg  [9:0]  pal_addr,
    output reg  [7:0]  pal_data,
    output wire        idle
);
    // ------------------------------------------------------------------ коды команд и режимы
    localparam [7:0] OP_TEX = 8'h01, OP_LMAP = 8'h02, OP_POLY = 8'h03, OP_POINT = 8'h04,
                     OP_PIC = 8'h05, OP_FILL = 8'h06, OP_FRAME = 8'h07, OP_TIME = 8'h08,
                     OP_WRITE = 8'h09, OP_SETUP = 8'h0A, OP_SCISSOR = 8'h0B,
                     OP_VBUF = 8'h0C, OP_STBUF = 8'h0D, OP_TRIS = 8'h0E;

    // ------------------------------------------------------------------ состояния автомата
    localparam [6:0]
        S_IDLE = 0, S_GET = 1, S_EXEC = 2, S_MEM = 3, S_WSTREAM = 4, S_FWAIT = 37,
        S_VB = 60, S_STB = 61, S_TRI0 = 62, S_TRI1 = 63, S_TS0 = 64, S_TS1 = 65, S_TS2 = 66,
        S_TS2B = 67, S_TS3 = 68, S_TS3W = 69, S_TS4 = 70, S_TS4W = 71, S_TS5 = 72,
        S_SETUP_CM = 5, S_SETUP_PAL = 6, S_SETUP_TURB = 7, S_SETUP_PALB = 36,
        S_P_TB = 8, S_P_INIT = 9, S_P_ROW = 10, S_P_ADV = 11, S_P_EDGE_A = 12, S_P_EDGE_A_W = 13,
        S_P_EDGE_B = 14, S_P_EDGE_B_W = 15, S_P_SPAN = 16, S_P_SEG = 17, S_P_DIV0 = 18, S_P_DIV0_W = 19,
        S_P_DIV1 = 20, S_P_DIV1_W = 21, S_P_PIX = 22, S_P_ZCHK = 23, S_P_TEX = 24, S_P_TEXD = 25,
        S_P_LM0 = 26, S_P_LM1 = 27, S_P_LM2 = 28, S_P_LM3 = 29, S_P_COL = 30, S_P_TR0 = 31,
        S_P_TR1 = 32, S_P_WR = 33, S_P_ZW = 34, S_P_NEXT = 35,
        S_PT_ROW = 40, S_PT_PIX = 41, S_PT_ZCHK = 42, S_PT_TR0 = 43, S_PT_TR1 = 44, S_PT_WR = 45,
        S_PT_ZW = 46, S_PT_NEXT = 47,
        S_PIC_ROW = 50, S_PIC_PIX = 51, S_PIC_TEX = 52, S_PIC_NEXT = 53, S_PIC_ROW2 = 54,
        S_PIC_ADDR = 58,
        S_FILL_ROW = 55, S_FILL_PIX = 56, S_FILL_NEXT = 57;

    reg [6:0]  state, mret;
    reg [31:0] cw [0:31];               // слова текущей команды
    reg [5:0]  nget, need;
    reg [23:0] wcount;                  // WRITE: сколько слов осталось
    reg [31:0] waddr;
    reg [31:0] mrd;                     // прочитанное слово

    // пачки моделей идут потоком, как WRITE: слово берётся, только пока его ждут
    reg [9:0]  vb_n, st_n;              // VBUF/STBUF: сколько записей осталось принять
    reg [8:0]  vb_i, st_i;              // куда пишется следующая
    reg [1:0]  vb_k;                    // слово вершины: 0 — {x, y}, 1 — zi, 2 — свет
    reg [7:0]  tri_n;                   // TRIS: треугольников осталось
    reg        in_tris;                 // POLY рисуется из TRIS: после него — за следующим треугольником
    assign cmd_ready = (state == S_IDLE) || (state == S_GET) || (state == S_WSTREAM && wcount != 0) ||
                       (state == S_VB && vb_n != 0) || (state == S_STB && st_n != 0) ||
                       (state == S_TRI0 && tri_n != 0) || (state == S_TRI1);
    assign idle = (state == S_IDLE) && !cmd_valid;

    // ------------------------------------------------------------------ настройки (SETUP и др.)
    reg [31:0] fb0, fb1, zaddr, amap;
    reg [15:0] scr_w, scr_h;
    wire [15:0] scr_s = (scr_w + 16'd15) & ~16'd15;   // шаг строки: W вверх до 16
    reg        draw;
    reg [7:0]  colormap [0:16383];      // в железе — BSRAM (8 блоков)
    reg [1:0]  pal_sub;                 // байт слова палитры
    reg [31:0] turb [0:127];
    reg [6:0]  phase;
    reg [31:0] src_addr;                // SETUP: откуда копируется таблица
    reg [13:0] cpy_i;

    reg [31:0] t_addr;  reg t_lin;  reg [3:0] t_wl, t_hl;  reg [15:0] t_lw, t_lh, t_ls;
    reg [31:0] l_addr;  reg [15:0] l_stride;  reg [7:0] l_rows;  reg [3:0] l_shift;
    reg [31:0] l_smin, l_tmin;
    reg signed [31:0] sx0, sy0, sx1, sy1;

    wire [31:0] fb_draw = draw ? fb1 : fb0;

    // ------------------------------------------------------------------ арифметика (как в C-модели)
    function [31:0] grad (input [31:0] c, input [31:0] a, input [31:0] b, input [31:0] x, input [31:0] y);
        grad = c + a * x + b * y;                   // по модулю 2^32
    endfunction

    // «Плавающий» z: 5 бит порядка + 11 бит мантиссы, больше — ближе.
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

    // Перспектива: (su · w) >> 20, младшие 32 бита (w ≤ 2^31 без знака).
    function [31:0] persp_mul (input [31:0] su, input [31:0] w);
        reg signed [64:0] p;
        begin
            p = $signed(su) * $signed({1'b0, w});
            persp_mul = p[51:20];
        end
    endfunction

    // floor(65536 / n), n = 1…16
    function [16:0] recip (input [4:0] n);
        case (n)
            1: recip = 65536;  2: recip = 32768;  3: recip = 21845;  4: recip = 16384;
            5: recip = 13107;  6: recip = 10922;  7: recip = 9362;   8: recip = 8192;
            9: recip = 7281;   10: recip = 6553;  11: recip = 5957;  12: recip = 5461;
            13: recip = 5041;  14: recip = 4681;  15: recip = 4369;  default: recip = 4096;
        endcase
    endfunction

    function [31:0] step_of (input [31:0] a0, input [31:0] a1, input [4:0] n);
        reg signed [33:0] d;
        reg signed [51:0] p;
        begin
            d = $signed({a1[31], a1}) - $signed({a0[31], a0});
            p = d * $signed({1'b0, recip (n)});
            step_of = p[47:16];
        end
    endfunction

    // ------------------------------------------------------------------ делитель
    reg         div_start;
    reg  [47:0] div_num;
    reg  [31:0] div_den;
    wire        div_busy, div_done;
    wire [47:0] div_q;
    reg         div_neg;                // край ребра: делимое было отрицательным

    bz_div #(.W(48), .WD(32)) u_div (
        .clk(clk), .rst(rst), .start(div_start), .num(div_num), .den(div_den),
        .busy(div_busy), .done(div_done), .quot(div_q)
    );

    // ------------------------------------------------------------------ многоугольник
    reg [4:0]  pn;                      // вершин
    reg [15:0] pmode;
    reg signed [15:0] vx [0:15];
    reg signed [15:0] vy [0:15];
    reg [31:0] gz [0:2], gu [0:2], gv [0:2], gl [0:2];
    reg [3:0]  top, bot, a0, a1, b0, b1;
    reg [4:0]  tbi;                     // 5 бит: при n = 16 счёт доходит до 16

    // ---------------------------------------------------- треугольники моделей (SPEC.md 0.3)
    // Буферы вершин и точек шкурки: в ПЛИС — BSRAM на 512 записей. Треугольник — номера в них.
    reg [15:0] tri_mode;
    reg [31:0] tw0, tw1;                // слова треугольника: номера вершин и номера st
`ifndef BZ_NO3D
    reg [15:0] vb_u [0:511], vb_v [0:511];
    reg [31:0] vb_zi [0:511], vb_l [0:511];
    reg [15:0] st_s [0:511], st_t [0:511];
`endif
    reg signed [31:0] tu [0:2], tv [0:2];        // вершины треугольника: целые точки экрана
    reg [31:0] tf [0:11];               // величины вершин: [3·k + i], k — zi, s, t, l
    reg signed [31:0] du1, dv1, du2, dv2;
    reg signed [63:0] td;               // d = du1·dv2 − du2·dv1 — удвоенная площадь со знаком
    reg [1:0]  ka;                      // какая величина сейчас (0 zi, 1 s, 2 t, 3 l)
    reg [31:0] ga;                      // её A, пока считается B
    reg        tneg;                    // делимое было отрицательным (floor)

    // разности величины k у вершин 1 и 0, 2 и 0 — 64 бита со знаком (разность двух int32 — до 33 бит)
    wire signed [63:0] df1 = $signed({{32{tf[3 * ka + 1][31]}}, tf[3 * ka + 1]}) - $signed({{32{tf[3 * ka][31]}}, tf[3 * ka]});
    wire signed [63:0] df2 = $signed({{32{tf[3 * ka + 2][31]}}, tf[3 * ka + 2]}) - $signed({{32{tf[3 * ka][31]}}, tf[3 * ka]});
    wire signed [63:0] na  = df1 * dv2 - df2 * dv1;     // числитель A
    wire signed [63:0] nb  = du1 * df2 - du2 * df1;     // числитель B
    // floor(n / d) при d > 0: n ≥ 0 — обычное деление; n < 0 — −ceil(|n| / d) = −((|n| + d − 1) / d)
    wire [47:0] na_div = (na < 0) ? 48'(-na + td - 1) : 48'(na);
    wire [47:0] nb_div = (nb < 0) ? 48'(-nb + td - 1) : 48'(nb);
    wire [31:0] q_sgn  = tneg ? 32'(-div_q) : div_q[31:0];      // частное со знаком, младшие 32 бита
    reg signed [31:0] py, py1, xa, xb, xr, px0;
    reg [4:0]  nseg, k;
    reg        first;
    reg [31:0] s0, t0, ds, dt, s1;      // s1 — временное до шага
    reg [31:0] zi_c, s_c, t_c, c_col;
    reg [15:0] z16_c;
    reg [7:0]  tx_c;
    reg [5:0]  row_c;
    reg [7:0]  lm00, lm10, lm01;
    reg [31:0] lm_r0, lm_r1, lm_iu, lm_iu1;
    reg [3:0]  lm_fu, lm_fv;

    wire        m_persp = pmode[0];
    wire [1:0]  m_light = pmode[2:1];
    wire        m_sky   = pmode[5];
    wire        m_ztest = pmode[5] | pmode[3];
    wire        m_zgt   = !pmode[5] & pmode[10];
    wire        m_zwr   = !pmode[5] & pmode[4];
    wire        m_warp  = pmode[6];
    wire [1:0]  m_trans = pmode[8:7];
    wire        m_key   = pmode[9];

    wire signed [31:0] yc  = (py <<< 4) + 32'sd8;
    wire [3:0] a1_next = (a1 == pn - 1) ? 4'd0 : a1 + 4'd1;
    wire [3:0] b1_next = (b1 == 4'd0) ? pn - 5'd1 : b1 - 4'd1;

    // Числитель и знаменатель края ребра p→q в строке yc (gpu/SPEC.md, «Растеризация»).
    function [80:0] edge_nd (input signed [15:0] px, input signed [15:0] ppy, input signed [15:0] qx,
                             input signed [15:0] qy, input signed [31:0] ycc);
        reg signed [39:0] dy, dx, n;
        reg [39:0] an;
        reg [31:0] d;
        begin
            dy = qy - ppy;
            dx = qx - px;
            n  = (px - 40'sd8) * dy + (ycc - ppy) * dx;
            d  = 32'(dy) << 4;
            an = (n < 0) ? 40'(-n) : 40'(n + d - 1);     // ceil: (n + d − 1) / d; отрицательное — −floor(−n / d)
            edge_nd = {n < 0, an, d, 8'd0};
        end
    endfunction

    wire [80:0] nd_a = edge_nd (vx[a0], vy[a0], vx[a1], vy[a1], yc);
    wire [80:0] nd_b = edge_nd (vx[b0], vy[b0], vx[b1], vy[b1], yc);
    wire signed [31:0] div_xs = div_neg ? -$signed(div_q[31:0]) : $signed(div_q[31:0]);

    // текущая точка
    wire signed [31:0] pxx  = px0 + {27'd0, k};
    wire [31:0] row_off     = py * {16'd0, scr_s};
    wire [31:0] zi_now      = grad (gz[0], gz[1], gz[2], pxx, py);
    wire [15:0] z16_now     = m_sky ? 16'd0 : zenc (zi_now);
    wire [31:0] za_now      = zaddr + ((row_off + pxx) << 1);
    wire [31:0] fa_now      = fb_draw + row_off + pxx;
    wire [31:0] s_now       = m_persp ? s0 + {27'd0, k} * ds : grad (gu[0], gu[1], gu[2], pxx, py);
    wire [31:0] t_now       = m_persp ? t0 + {27'd0, k} * dt : grad (gv[0], gv[1], gv[2], pxx, py);
    wire [31:0] l_now       = grad (gl[0], gl[1], gl[2], pxx, py);

    // текущая zi на конце куска (для деления)
    wire [31:0] zi_end0 = grad (gz[0], gz[1], gz[2], px0, py);
    wire [31:0] zi_end1 = grad (gz[0], gz[1], gz[2], px0 + {27'd0, nseg}, py);
    wire [31:0] zc0 = ($signed(zi_end0) < 32'sd32768) ? 32'd32768 : zi_end0;
    wire [31:0] zc1 = ($signed(zi_end1) < 32'sd32768) ? 32'd32768 : zi_end1;

    // адрес текселя по s, t (Q16.16)
    function [31:0] tex_addr (input [31:0] s, input [31:0] t);
        reg signed [31:0] x, y;
        reg [31:0] ux, uy;
        begin
            x = $signed(s) >>> 16;
            y = $signed(t) >>> 16;
            if (t_lin) begin
                if (x < 0) x = 0;
                if (x > $signed({16'd0, t_lw}) - 1) x = $signed({16'd0, t_lw}) - 1;
                if (y < 0) y = 0;
                if (y > $signed({16'd0, t_lh}) - 1) y = $signed({16'd0, t_lh}) - 1;
                tex_addr = t_addr + 32'(y) * {16'd0, t_ls} + 32'(x);
            end else begin
                ux = 32'(x) & ((32'd1 << t_wl) - 1);
                uy = 32'(y) & ((32'd1 << t_hl) - 1);
                if (t_wl >= 3 && t_hl >= 3)
                    tex_addr = t_addr + ((((uy >> 3) << (t_wl - 3)) + (ux >> 3)) << 6) + ((uy & 7) << 3) + (ux & 7);
                else
                    tex_addr = t_addr + (uy << t_wl) + ux;
            end
        end
    endfunction

    // волна
    wire [31:0] w_s = s_c + turb[7'(($signed(t_c) >>> 16) + {25'd0, phase})];
    wire [31:0] w_t = t_c + turb[7'(($signed(s_c) >>> 16) + {25'd0, phase})];

    // lightmap: клетки и веса
    wire signed [31:0] lm_u  = $signed(s_c - l_smin) >>> l_shift;
    wire signed [31:0] lm_v  = $signed(t_c - l_tmin) >>> l_shift;
    wire signed [31:0] lm_iu0 = lm_u >>> 16;
    wire signed [31:0] lm_iv0 = lm_v >>> 16;
    wire [31:0] lm_iuc = (lm_iu0 < 0) ? 32'd0 : (lm_iu0 > $signed({16'd0, l_stride}) - 1) ? {16'd0, l_stride} - 1 : lm_iu0;
    wire [31:0] lm_ivc = (lm_iv0 < 0) ? 32'd0 : (lm_iv0 > $signed({24'd0, l_rows}) - 1) ? {24'd0, l_rows} - 1 : lm_iv0;
    wire [3:0]  lm_fu0 = (lm_iu0 < 0) ? 4'd0 : lm_u[15:12];
    wire [3:0]  lm_fv0 = (lm_iv0 < 0) ? 4'd0 : lm_v[15:12];
    wire [31:0] lm_iu1c = (lm_iuc + 1 > {16'd0, l_stride} - 1) ? {16'd0, l_stride} - 1 : lm_iuc + 1;
    wire [31:0] lm_iv1c = (lm_ivc + 1 > {24'd0, l_rows} - 1) ? {24'd0, l_rows} - 1 : lm_ivc + 1;

    wire [7:0] rbyte = 8'(mrd >> {mem_addr[1:0], 3'b000});
    wire [15:0] rhalf = mem_addr[1] ? mrd[31:16] : mrd[15:0];

    wire [31:0] lm_val = ({24'd0, lm00} * (16 - lm_fu) + {24'd0, lm10} * {28'd0, lm_fu}) * (16 - lm_fv) +
                         ({24'd0, lm01} * (16 - lm_fu) + {24'd0, rbyte} * {28'd0, lm_fu}) * {28'd0, lm_fv};

    wire signed [31:0] l_row = $signed(l_now) >>> 24;
    wire [5:0] vrow = (l_row < 0) ? 6'd0 : (l_row > 63) ? 6'd63 : l_row[5:0];

    function [31:0] mix_addr (input [1:0] tr, input [7:0] src, input [7:0] dst);
        mix_addr = (tr == 2'd1) ? amap + {24'd0, src} + {16'd0, dst, 8'd0} : amap + {16'd0, src, 8'd0} + {24'd0, dst};
    endfunction

    // ------------------------------------------------------------------ частица, картинка, заливка
    reg signed [31:0] qx, qy, qi, qj, qw, qh, qx1;
    reg [31:0] q_addr, q_stride, q_ss, q_ts, q_val;
    reg [7:0]  q_col;
    reg [1:0]  q_trans;
    reg        q_key, q_tgt, q_fade;
    reg [15:0] q_z16;

    wire signed [31:0] qpx = qx + qi;
    wire signed [31:0] qpy = qy + qj;
    wire [31:0] q_row  = qpy * {16'd0, scr_s};
    wire [31:0] q_fa   = fb_draw + q_row + qpx;
    wire [31:0] q_za   = zaddr + ((q_row + qpx) << 1);
    wire [63:0] q_trow = {32'd0, qj} * {32'd0, q_ts};
    wire [63:0] q_tcol = {32'd0, qi} * {32'd0, q_ss};
    // Адрес текселя картинки = адрес + строка_источника·шаг + столбец_источника. Два умножения подряд
    // за один такт 60,75 МГц не успевали (первая сборка Gowin, −0,2 нс), поэтому по ступеням:
    // q_rowoff — номер строки источника (раз на строку), q_rowbase — начало этой строки в памяти
    // (раз на строку), q_scol — столбец (на каждую точку); на каждом такте — не больше одного умножения.
    reg  [31:0] q_rowoff, q_rowbase, q_scol;

    // ------------------------------------------------------------------ автомат
    task automatic mreq (input we, input [31:0] a, input [31:0] wd, input [3:0] be, input [6:0] ret);
        begin
            mem_req   <= 1'b1;
            mem_we    <= we;
            mem_addr  <= a[22:0];
            mem_wdata <= wd;
            mem_be    <= be;
            mret      <= ret;
            state     <= S_MEM;
        end
    endtask

    task automatic rd (input [31:0] a, input [6:0] ret);
        mreq (1'b0, a, 32'd0, 4'd0, ret);
    endtask

    task automatic wr8 (input [31:0] a, input [7:0] v, input [6:0] ret);
        mreq (1'b1, a, {4{v}}, 4'b0001 << a[1:0], ret);
    endtask

    task automatic wr16 (input [31:0] a, input [15:0] v, input [6:0] ret);
        mreq (1'b1, a, {2{v}}, a[1] ? 4'b1100 : 4'b0011, ret);
    endtask

    wire [7:0] op = cw[0][31:24];

    integer ii;

    always @(posedge clk) begin
        div_start   <= 1'b0;
        frame_pulse <= 1'b0;
        pal_we      <= 1'b0;
        if (rst) begin
            state   <= S_IDLE;
            mem_req <= 1'b0;
            draw    <= 1'b0;
            bad_cmd <= 1'b0;
            nget    <= 0;
            shown_fb <= 0;
            shown_blend <= 0;
            phase   <= 0;
            sx0 <= 0; sy0 <= 0; sx1 <= 0; sy1 <= 0;
            vb_n <= 0; st_n <= 0; tri_n <= 0; in_tris <= 1'b0;
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
                        OP_WRITE:   need <= 2;
                        OP_SETUP:   need <= 9;
                        OP_SCISSOR: need <= 3;
                        default:    need <= 0;
                    endcase
                    case (cmd_data[31:24])
                        OP_TEX, OP_LMAP, OP_POLY, OP_POINT, OP_PIC, OP_FILL, OP_FRAME, OP_TIME,
                        OP_WRITE, OP_SETUP, OP_SCISSOR: state <= S_GET;
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
                if (cmd_valid) begin
                    cw[nget[4:0]] <= cmd_data;
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
                    t_addr <= cw[1];
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
                    l_addr   <= cw[1];
                    l_stride <= (cw[2][15:0] == 0) ? 16'd1 : cw[2][15:0];
                    l_rows   <= (cw[2][23:16] == 0) ? 8'd1 : cw[2][23:16];
                    l_shift  <= cw[2][27:24];
                    l_smin   <= cw[3];
                    l_tmin   <= cw[4];
                end
                OP_TIME:  phase <= cw[1][6:0];
                OP_FRAME: begin
                    shown_blend <= cw[1];
                    shown_fb    <= fb_draw[22:0];
                    draw        <= ~draw;
                    frame_pulse <= 1'b1;
                    state       <= S_FWAIT;      // в прежний показанный кадр — только после смены
                end
                OP_SCISSOR: begin
                    sx0 <= ($signed(cw[1][15:0]) < 0) ? 0 : 32'($signed(cw[1][15:0]));
                    sy0 <= ($signed(cw[1][31:16]) < 0) ? 0 : 32'($signed(cw[1][31:16]));
                    sx1 <= ($signed(cw[2][15:0]) > $signed({16'd0, scr_w})) ? {16'd0, scr_w} : 32'($signed(cw[2][15:0]));
                    sy1 <= ($signed(cw[2][31:16]) > $signed({16'd0, scr_h})) ? {16'd0, scr_h} : 32'($signed(cw[2][31:16]));
                end
                OP_WRITE: begin
                    wcount <= cw[0][23:0];
                    waddr  <= cw[1];
                    state  <= S_WSTREAM;
                end
                OP_SETUP: begin
                    fb0   <= cw[1];
                    fb1   <= cw[2];
                    zaddr <= cw[3];
                    scr_w <= cw[4][15:0];
                    scr_h <= cw[4][31:16];
                    amap  <= cw[6];
                    draw  <= 1'b0;
                    sx0 <= 0; sy0 <= 0;
                    sx1 <= {16'd0, cw[4][15:0]};
                    sy1 <= {16'd0, cw[4][31:16]};
                    src_addr <= cw[5];
                    cpy_i <= 0;
                    state <= S_SETUP_CM;
                end
`ifndef BZ_NO3D   // без 3D: POLY и POINT — мимо (S_IDLE)
                OP_POLY: begin
                    pn    <= cw[0][4:0];
                    pmode <= cw[0][23:8];
                    for (ii = 0; ii < 16; ii = ii + 1) begin
                        vx[ii] <= cw[1 + ii][15:0];
                        vy[ii] <= cw[1 + ii][31:16];
                    end
                    for (ii = 0; ii < 3; ii = ii + 1) begin
                        gz[ii] <= cw[1 + cw[0][4:0] + ii];
                        gu[ii] <= cw[1 + cw[0][4:0] + 3 + ii];
                        gv[ii] <= cw[1 + cw[0][4:0] + 6 + ii];
                        gl[ii] <= cw[1 + cw[0][4:0] + 9 + ii];
                    end
                    top <= 0;
                    bot <= 0;
                    tbi <= 1;
                    state <= S_P_TB;
                end
                OP_POINT: begin
                    q_col   <= cw[0][7:0];
                    qw      <= {28'd0, cw[0][11:8]};
                    q_trans <= cw[0][13:12];
                    qx      <= 32'($signed(cw[1][15:0]));
                    qy      <= 32'($signed(cw[1][31:16]));
                    q_z16   <= zenc (cw[2]);
                    qj      <= 0;
                    state   <= S_PT_ROW;
                end
`endif
                OP_PIC: begin
                    q_key    <= cw[0][0];
                    qx       <= 32'($signed(cw[1][15:0]));
                    qy       <= 32'($signed(cw[1][31:16]));
                    qw       <= {16'd0, cw[2][15:0]};
                    qh       <= {16'd0, cw[2][31:16]};
                    q_addr   <= cw[3];
                    q_stride <= cw[4];
                    q_ss     <= cw[5];
                    q_ts     <= cw[6];
                    qj       <= 0;
                    state    <= S_PIC_ROW;
                end
                OP_FILL: begin
                    q_tgt  <= cw[0][0];
                    q_fade <= cw[0][1];
                    q_val  <= cw[3];
                    // обрезка по экрану сразу: qx/qy — левый верх, qx1/qh — правый низ (не включая)
                    qx  <= ($signed(cw[1][15:0]) < 0) ? 0 : 32'($signed(cw[1][15:0]));
                    qy  <= ($signed(cw[1][31:16]) < 0) ? 0 : 32'($signed(cw[1][31:16]));
                    qx1 <= ($signed(cw[1][15:0]) + $signed({16'd0, cw[2][15:0]}) > $signed({16'd0, scr_w})) ?
                           {16'd0, scr_w} : $signed(cw[1][15:0]) + $signed({16'd0, cw[2][15:0]});
                    qh  <= ($signed(cw[1][31:16]) + $signed({16'd0, cw[2][31:16]}) > $signed({16'd0, scr_h})) ?
                           {16'd0, scr_h} : $signed(cw[1][31:16]) + $signed({16'd0, cw[2][31:16]});
                    qj  <= 0;
                    state <= S_FILL_ROW;
                end
                default: ;
                endcase
            end

            // ---------------------------------------------------- VBUF, STBUF: вершины и шкурка моделей
            S_VB:
                if (vb_n == 0)
                    state <= S_IDLE;
                else if (cmd_valid) begin
`ifndef BZ_NO3D
                    case (vb_k)
                        2'd0: begin vb_u[vb_i] <= cmd_data[15:0]; vb_v[vb_i] <= cmd_data[31:16]; end
                        2'd1: vb_zi[vb_i] <= cmd_data;
                        default: vb_l[vb_i] <= cmd_data;
                    endcase
`endif
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
`ifndef BZ_NO3D
                    st_s[st_i] <= cmd_data[15:0];
                    st_t[st_i] <= cmd_data[31:16];
`endif
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
                    state <= S_TS0;
                end
`ifdef BZ_NO3D
            S_TS0: state <= S_TRI0;       // без 3D треугольник только принимается
`else
            // Постановка треугольника (SPEC.md, «Треугольники моделей»): величина f меняется по экрану
            // линейно, f(x, y) = C + A·x + B·y; три вершины дают систему из трёх уравнений, её решение —
            // A = (df1·dv2 − df2·dv1) / d, B = (du1·df2 − du2·df1) / d, C = f0 − A·u0 − B·v0, где d —
            // удвоенная площадь треугольника. Делим точно, с округлением вниз — как C-модель.
            S_TS0: begin : tri_load
                integer i;
                for (i = 0; i < 3; i = i + 1) begin
                    tu[i] <= {{16{vb_u[tw0[9 * i +: 9]][15]}}, vb_u[tw0[9 * i +: 9]]};
                    tv[i] <= {{16{vb_v[tw0[9 * i +: 9]][15]}}, vb_v[tw0[9 * i +: 9]]};
                    tf[i]     <= vb_zi[tw0[9 * i +: 9]];
                    tf[3 + i] <= {st_s[tw1[9 * i +: 9]], 16'd0};
                    tf[6 + i] <= {st_t[tw1[9 * i +: 9]], 16'd0};
                    tf[9 + i] <= vb_l[tw0[9 * i +: 9]] << 16;
                end
                state <= S_TS1;
            end
            S_TS1: begin
                du1 <= tu[1] - tu[0]; dv1 <= tv[1] - tv[0];
                du2 <= tu[2] - tu[0]; dv2 <= tv[2] - tv[0];
                state <= S_TS2;
            end
            S_TS2: begin
                td    <= 64'(du1) * 64'(dv2) - 64'(du2) * 64'(dv1);
                state <= S_TS2B;
            end
            S_TS2B:
                if (td <= 0)
                    state <= S_TRI0;      // спиной к нам или вырожден — не рисуется
                else begin
                    ka    <= 2'd0;
                    state <= S_TS3;
                end
            S_TS3: begin                  // A величины ka
                div_num   <= na_div;
                div_den   <= td[31:0];
                tneg      <= na < 0;
                div_start <= 1'b1;
                state     <= S_TS3W;
            end
            S_TS3W:
                if (div_done) begin
                    ga    <= q_sgn;
                    state <= S_TS4;
                end
            S_TS4: begin                  // B величины ka
                div_num   <= nb_div;
                div_den   <= td[31:0];
                tneg      <= nb < 0;
                div_start <= 1'b1;
                state     <= S_TS4W;
            end
            S_TS4W:
                if (div_done) begin : tri_grad
                    reg [31:0] c;
                    c = tf[3 * ka] - ga * tu[0] - q_sgn * tv[0];          // по модулю 2^32
                    case (ka)
                        2'd0: begin gz[0] <= c; gz[1] <= ga; gz[2] <= q_sgn; end
                        2'd1: begin gu[0] <= c; gu[1] <= ga; gu[2] <= q_sgn; end
                        2'd2: begin gv[0] <= c; gv[1] <= ga; gv[2] <= q_sgn; end
                        default: begin gl[0] <= c; gl[1] <= ga; gl[2] <= q_sgn; end
                    endcase
                    if (ka == 2'd3)
                        state <= S_TS5;
                    else begin
                        ka    <= ka + 1'b1;
                        state <= S_TS3;
                    end
                end
            S_TS5: begin : tri_poly       // дальше — как многоугольник из трёх вершин
                integer i;
                for (i = 0; i < 3; i = i + 1) begin
                    vx[i] <= 16'(tu[i] * 16 + 8);         // целая точка Quake — центр точки адаптера
                    vy[i] <= 16'(tv[i] * 16 + 8);
                end
                pn    <= 5'd3;
                pmode <= tri_mode;
                top   <= 4'd0;
                bot   <= 4'd0;
                tbi   <= 5'd1;
                state <= S_P_TB;
            end
`endif

            // ---------------------------------------------------- FRAME: ждать, пока вывод сменит кадр
            S_FWAIT:
                if (swap_ack)
                    state <= S_IDLE;

            // ---------------------------------------------------- WRITE: данные потока — в память
            S_WSTREAM:
                if (wcount == 0)
                    state <= S_IDLE;
                else if (cmd_valid) begin
                    wcount <= wcount - 1'b1;
                    waddr  <= waddr + 4;
                    mreq (1'b1, waddr, cmd_data, 4'b1111, S_WSTREAM);
                end

            // ---------------------------------------------------- SETUP: таблицы в «BSRAM»
            S_SETUP_CM: begin
                if (cpy_i != 0) begin
                    colormap[{cpy_i[11:0] - 12'd1, 2'b00}]     <= mrd[7:0];
                    colormap[{cpy_i[11:0] - 12'd1, 2'b01}]     <= mrd[15:8];
                    colormap[{cpy_i[11:0] - 12'd1, 2'b10}]     <= mrd[23:16];
                    colormap[{cpy_i[11:0] - 12'd1, 2'b11}]     <= mrd[31:24];
                end
                if (cpy_i == 14'd4096) begin
                    cpy_i <= 0;
                    src_addr <= cw[7];
                    state <= S_SETUP_PAL;
                end else begin
                    cpy_i <= cpy_i + 1'b1;
                    rd (src_addr + {16'd0, cpy_i, 2'b00}, S_SETUP_CM);
                end
            end
            // палитра — не себе, а блоку вывода: слово читается, его 4 байта уходят по одному за такт
            S_SETUP_PAL:
                if (cpy_i == 14'd192) begin
                    cpy_i <= 0;
                    src_addr <= cw[8];
                    state <= S_SETUP_TURB;
                end else begin
                    pal_sub <= 2'd0;
                    rd (src_addr + {16'd0, cpy_i, 2'b00}, S_SETUP_PALB);
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
                if (cpy_i != 0)
                    turb[7'(cpy_i - 1)] <= mrd;
                if (cpy_i == 14'd128)
                    state <= S_IDLE;
                else begin
                    cpy_i <= cpy_i + 1'b1;
                    rd (src_addr + {16'd0, cpy_i, 2'b00}, S_SETUP_TURB);
                end
            end

`ifndef BZ_NO3D   // автоматы POLY и POINT
            // ---------------------------------------------------- POLY: верх и низ
            // Многоугольник рисуется строками сверху вниз. Сначала ищем верхнюю вершину (top — с
            // наименьшим y) и нижнюю (bot — с наибольшим): по одной за такт, сравнением с лучшей
            // найденной. От top вниз идут две «цепочки» рёбер — по номерам вершин вперёд (A) и назад
            // (B); у выпуклого многоугольника в каждой строке одно ребро цепочки A даёт один край
            // отрезка строки, одно ребро цепочки B — другой.
            S_P_TB: begin
                if (tbi == pn) begin
                    state <= S_P_INIT;
                end else begin
                    if (vy[tbi[3:0]] < vy[top]) top <= tbi[3:0];
                    if (vy[tbi[3:0]] > vy[bot]) bot <= tbi[3:0];
                    tbi <= tbi + 1'b1;
                end
            end
            S_P_INIT: begin
                begin : init_rows
                    reg signed [31:0] y0, y1;
                    y0 = ($signed({{16{vy[top][15]}}, vy[top]}) + 32'sd7) >>> 4;
                    y1 = ($signed({{16{vy[bot][15]}}, vy[bot]}) + 32'sd7) >>> 4;
                    py  <= (y0 < sy0) ? sy0 : y0;
                    py1 <= (y1 > sy1) ? sy1 : y1;
                end
                a0 <= top;
                b0 <= top;
                a1 <= (top == pn - 1) ? 4'd0 : top + 4'd1;
                b1 <= (top == 4'd0) ? pn - 5'd1 : top - 4'd1;
                state <= S_P_ROW;
            end
            S_P_ROW:
                if (py >= py1)
                    state <= in_tris ? S_TRI0 : S_IDLE;   // TRIS: следующий треугольник
                else
                    state <= S_P_ADV;
            // Продвинуть обе цепочки к рёбрам, которые пересекают центр этой строки (yc = 16y + 8 в
            // единицах 1/16 точки): ребро, закончившееся выше центра строки, сменяется следующим.
            // Если в какой-то цепочке ребра на этой строке нет (бывает только у вырожденных фигур),
            // строка пропускается. Потом — край каждого ребра делением (S_P_EDGE_A/B): в какой точке
            // строки начинается фигура: ceil(((p.x − 8)·dy + (yc − p.y)·dx) / (16·dy)).
            S_P_ADV:
                if (a1 != bot && $signed({{16{vy[a1][15]}}, vy[a1]}) <= yc) begin
                    a0 <= a1;
                    a1 <= a1_next;
                end else if (b1 != bot && $signed({{16{vy[b1][15]}}, vy[b1]}) <= yc) begin
                    b0 <= b1;
                    b1 <= b1_next;
                end else if (!($signed({{16{vy[a0][15]}}, vy[a0]}) <= yc && yc < $signed({{16{vy[a1][15]}}, vy[a1]})) ||
                             !($signed({{16{vy[b0][15]}}, vy[b0]}) <= yc && yc < $signed({{16{vy[b1][15]}}, vy[b1]}))) begin
                    py    <= py + 1;            // строки нет
                    state <= S_P_ROW;
                end else
                    state <= S_P_EDGE_A;
            S_P_EDGE_A: begin
                div_num   <= {8'd0, nd_a[79:40]};
                div_den   <= nd_a[39:8];
                div_neg   <= nd_a[80];
                div_start <= 1'b1;
                state     <= S_P_EDGE_A_W;
            end
            S_P_EDGE_A_W:
                if (div_done) begin
                    xa    <= div_xs;
                    state <= S_P_EDGE_B;
                end
            S_P_EDGE_B: begin
                div_num   <= {8'd0, nd_b[79:40]};
                div_den   <= nd_b[39:8];
                div_neg   <= nd_b[80];
                div_start <= 1'b1;
                state     <= S_P_EDGE_B_W;
            end
            S_P_EDGE_B_W:
                if (div_done) begin
                    xb    <= div_xs;
                    state <= S_P_SPAN;
                end
            S_P_SPAN: begin
                begin : span
                    reg signed [31:0] l, r;
                    r = (xa < xb) ? xb : xa;
                    l = (xa < xb) ? xa : xb;
                    if (l < sx0) l = sx0;
                    if (r > sx1) r = sx1;
                    px0 <= l;
                    xr  <= r;
                end
                first <= 1'b1;
                state <= S_P_SEG;
            end
            // ---------------------------------------------------- кусок строки
            // Отрезок строки [xl, xr) режется на куски, выровненные по колонкам экрана в 16 точек: кусок
            // кончается на ближайшей границе, кратной 16 (или на конце отрезка). Для перспективы на
            // обоих концах куска считаются настоящие s и t: деление 2^46 / zi (S_P_DIV0, S_P_DIV1) и
            // умножение (persp_mul). Внутри куска s и t идут по прямой с шагом ds = (s1 − s0) / n
            // (через таблицу recip, без деления). Так делал и программный Quake: деление дорогое, а на
            // 16 точках искажение прямой незаметно.
            S_P_SEG:
                if (px0 >= xr) begin
                    py    <= py + 1;
                    state <= S_P_ROW;
                end else begin
                    nseg  <= 5'((((px0 | 32'sd15) + 32'sd1) < xr ? ((px0 | 32'sd15) + 32'sd1) : xr) - px0);
                    k     <= 0;
                    state <= m_persp ? S_P_DIV0 : S_P_PIX;
                end
            S_P_DIV0: begin
                div_num   <= 48'h4000_0000_0000;        // 2^46
                div_den   <= zc0;
                div_start <= 1'b1;
                state     <= S_P_DIV0_W;
            end
            S_P_DIV0_W:
                if (div_done) begin
                    s0    <= persp_mul (grad (gu[0], gu[1], gu[2], px0, py), div_q[31:0]);
                    t0    <= persp_mul (grad (gv[0], gv[1], gv[2], px0, py), div_q[31:0]);
                    state <= S_P_DIV1;
                end
            S_P_DIV1: begin
                div_num   <= 48'h4000_0000_0000;
                div_den   <= zc1;
                div_start <= 1'b1;
                state     <= S_P_DIV1_W;
            end
            S_P_DIV1_W:
                if (div_done) begin
                    ds    <= step_of (s0, persp_mul (grad (gu[0], gu[1], gu[2], px0 + {27'd0, nseg}, py), div_q[31:0]), nseg);
                    dt    <= step_of (t0, persp_mul (grad (gv[0], gv[1], gv[2], px0 + {27'd0, nseg}, py), div_q[31:0]), nseg);
                    state <= S_P_PIX;
                end
            // ---------------------------------------------------- точка
            // Одна точка проходит шаги: z-тест (чтение z из памяти, S_P_ZCHK) → тексель (S_P_TEX) →
            // свет: lightmap — 4 клетки (S_P_LM0…3), вершины — сразу строка colormap → цвет через
            // colormap (S_P_COL) → смешивание с кадром при прозрачности (S_P_TR0/1) → запись цвета
            // (S_P_WR) и z (S_P_ZW). Не прошла z-тест или тексель «прозрачный» (255 при KEY) — сразу к
            // следующей точке (S_P_NEXT). В быстром ядре эти шаги станут ступенями конвейера, и в каждой
            // будет своя точка, по точке за такт.
            S_P_PIX:
                if (k == nseg) begin
                    px0   <= px0 + {27'd0, nseg};
                    first <= 1'b0;
                    state <= S_P_SEG;
                end else begin
                    zi_c  <= zi_now;
                    z16_c <= z16_now;
                    s_c   <= s_now;
                    t_c   <= t_now;
                    if (m_ztest)
                        rd (za_now, S_P_ZCHK);
                    else
                        state <= S_P_TEX;
                end
            S_P_ZCHK:
                if (m_zgt ? !(z16_c > rhalf) : !(z16_c >= rhalf))
                    state <= S_P_NEXT;
                else
                    state <= S_P_TEX;
            S_P_TEX:
                rd (tex_addr (m_warp ? w_s : s_c, m_warp ? w_t : t_c), S_P_TEXD);
            S_P_TEXD: begin
                tx_c <= rbyte;
                if (m_key && rbyte == 8'hFF)
                    state <= S_P_NEXT;
                else if (m_light == 2'd0) begin
                    lm_fu  <= lm_fu0;
                    lm_fv  <= lm_fv0;
                    lm_iu  <= lm_iuc;
                    lm_iu1 <= lm_iu1c;
                    lm_r0  <= l_addr + lm_ivc * {16'd0, l_stride};
                    lm_r1  <= l_addr + lm_iv1c * {16'd0, l_stride};
                    rd (l_addr + lm_ivc * {16'd0, l_stride} + lm_iuc, S_P_LM0);
                end else if (m_light == 2'd1) begin
                    c_col <= {24'd0, colormap[{vrow, rbyte}]};
                    state <= S_P_COL;
                end else begin
                    c_col <= {24'd0, rbyte};
                    state <= S_P_COL;
                end
            end
            S_P_LM0: begin lm00 <= rbyte; rd (lm_r0 + lm_iu1, S_P_LM1); end
            S_P_LM1: begin lm10 <= rbyte; rd (lm_r1 + lm_iu,  S_P_LM2); end
            S_P_LM2: begin lm01 <= rbyte; rd (lm_r1 + lm_iu1, S_P_LM3); end
            S_P_LM3: begin
                c_col <= {24'd0, colormap[{lm_val[15:10], tx_c}]};
                state <= S_P_COL;
            end
            S_P_COL:
                if (m_trans != 2'd0)
                    rd (fa_now, S_P_TR0);
                else
                    state <= S_P_WR;
            S_P_TR0: rd (mix_addr (m_trans, c_col[7:0], rbyte), S_P_TR1);
            S_P_TR1: begin c_col <= {24'd0, rbyte}; state <= S_P_WR; end
            S_P_WR:  wr8 (fa_now, c_col[7:0], m_zwr ? S_P_ZW : S_P_NEXT);
            S_P_ZW:  wr16 (za_now, z16_c, S_P_NEXT);
            S_P_NEXT: begin
                k     <= k + 1'b1;
                state <= S_P_PIX;
            end

            // ---------------------------------------------------- POINT
            S_PT_ROW:
                if (qj == qw)
                    state <= S_IDLE;
                else if (qpy < sy0 || qpy >= sy1)
                    qj <= qj + 1;
                else begin
                    qi    <= 0;
                    state <= S_PT_PIX;
                end
            S_PT_PIX:
                if (qi == qw) begin
                    qj    <= qj + 1;
                    state <= S_PT_ROW;
                end else if (qpx < sx0 || qpx >= sx1)
                    qi <= qi + 1;
                else
                    rd (q_za, S_PT_ZCHK);
            S_PT_ZCHK:
                if (q_z16 < rhalf)
                    state <= S_PT_NEXT;
                else if (q_trans != 2'd0)
                    rd (q_fa, S_PT_TR0);
                else begin
                    c_col <= {24'd0, q_col};
                    state <= S_PT_WR;
                end
            S_PT_TR0: rd (mix_addr ({q_trans[1], q_trans[0]}, q_col, rbyte), S_PT_TR1);
            S_PT_TR1: begin c_col <= {24'd0, rbyte}; state <= S_PT_WR; end
            S_PT_WR:  wr8 (q_fa, c_col[7:0], S_PT_ZW);
            S_PT_ZW:  wr16 (q_za, q_z16, S_PT_NEXT);
            S_PT_NEXT: begin
                qi    <= qi + 1;
                state <= S_PT_PIX;
            end

`endif
            // ---------------------------------------------------- PIC
            S_PIC_ROW:
                if (qj == qh)
                    state <= S_IDLE;
                else if (qpy < 0 || qpy >= $signed({16'd0, scr_h}))
                    qj <= qj + 1;
                else begin
                    qi       <= (qx < 0) ? -qx : 0;
                    qx1      <= ((qx + qw > $signed({16'd0, scr_w})) ? $signed({16'd0, scr_w}) : qx + qw) - qx;
                    q_rowoff <= q_trow[47:16];              // (j · шаг_t) >> 16 — строка источника
                    state    <= S_PIC_ROW2;
                end
            S_PIC_ROW2: begin
                q_rowbase <= q_addr + q_rowoff * q_stride;   // начало этой строки в памяти
                state     <= S_PIC_PIX;
            end
            S_PIC_PIX:
                if (qi >= qx1) begin
                    qj    <= qj + 1;
                    state <= S_PIC_ROW;
                end else begin
                    q_scol <= q_tcol[47:16];                // (i · шаг_s) >> 16 — столбец источника
                    state <= S_PIC_ADDR;
                end
            S_PIC_ADDR:
                rd (q_rowbase + q_scol, S_PIC_TEX);
            S_PIC_TEX:
                if (q_key && rbyte == 8'hFF)
                    state <= S_PIC_NEXT;
                else
                    wr8 (q_fa, rbyte, S_PIC_NEXT);
            S_PIC_NEXT: begin
                qi    <= qi + 1;
                state <= S_PIC_PIX;
            end

            // ---------------------------------------------------- FILL (qx…qx1 × qy…qh уже обрезаны)
            S_FILL_ROW:
                if (qy + qj >= qh)
                    state <= S_IDLE;
                else begin
                    qi    <= 0;
                    state <= S_FILL_PIX;
                end
            S_FILL_PIX:
                if (qx + qi >= qx1) begin
                    qj    <= qj + 1;
                    state <= S_FILL_ROW;
                end else if (q_tgt)
                    wr16 (zaddr + (((qy + qj) * {16'd0, scr_s} + qx + qi) << 1), q_val[15:0], S_FILL_NEXT);
                else if (!q_fade)
                    wr8 (fb_draw + (qy + qj) * {16'd0, scr_s} + qx + qi, q_val[7:0], S_FILL_NEXT);
                else if (((qx + qi) & 3) != (((qy + qj) & 1) << 1))
                    wr8 (fb_draw + (qy + qj) * {16'd0, scr_s} + qx + qi, 8'd0, S_FILL_NEXT);
                else
                    state <= S_FILL_NEXT;
            S_FILL_NEXT: begin
                qi    <= qi + 1;
                state <= S_FILL_PIX;
            end

            default: state <= S_IDLE;
            endcase
        end
    end
endmodule
