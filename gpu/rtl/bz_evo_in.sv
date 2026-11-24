// =====================================================================================================
// bz_evo_in.sv — второй вход ядра: SPI Z-контроллера Evo (вывод FT812, 14 или 28 МГц) → прямо в SDRAM.
// =====================================================================================================
//
// ЗАЧЕМ ВТОРОЙ ВХОД
//   По LINK (ESP) идут команды каждого кадра. Объёмные данные уровня — текстуры, шкурки, небо, картинки,
//   3–4,5 МБ на карту — удобнее отдать с Evo: Z80 читает pak со своей SD-карты и шлёт байты сюда, а LINK
//   остаётся свободным под кадр. Что грузить, решает ESP и говорит Z80 обычным обменом ESP↔Z80.
//
// SPI ОДНИМ АБЗАЦЕМ
//   SPI — последовательная шина: ведущий (здесь Z-контроллер Evo) опускает CS в 0 («говорю с тобой»),
//   на каждом такте SCK выдаёт по биту на MOSI (старший бит байта первым) и снимает бит с MISO.
//   Режим 0 — бит читается по фронту SCK. Приёмник ft_spi тактируется самим SCK: 28 МГц пересчитать
//   тактом ядра (60 МГц) надёжно нельзя — всего два отсчёта на бит.
//
// ПРОТОКОЛ — КАК ЗАПИСЬ В ПАМЯТЬ FT812
//   Z80-программы VDAC4 его уже умеют (это порт FT812). Посылка:
//     3 байта заголовка: два старших бита 10 — «запись», остальные 22 бита — адрес;
//     дальше байты данных подряд, адрес растёт на 1 с каждым байтом.
//   Адрес SDRAM = {страница, 22 бита адреса}: страница (0 — нижние 4 МБ, 1 — верхние) задаётся по LINK.
//
// ИЗ БАЙТОВ — В ПАЧКИ
//   Байты копятся в «пачку» 32 байта (8 слов) с маской «какие байты пришли». Пачка уходит в SDRAM
//   одной операцией моста, когда адрес перешёл через границу 32 байт или посылка кончилась (CS = 1).
//   Пока мост пишет одну пачку («pend»), копится следующая («buf») — так байты не теряются.
//
// ПЕРЕДАЧА ИЗ ДОМЕНА SCK В ДОМЕН ЯДРА
//   Каждый принятый байт ft_spi отмечает переключением сигнала tg (0→1 или 1→0) и держит его поля не
//   меньше 8 тактов SCK. Здесь tg проходит через 3 триггера (синхронизатор: защита от метастабильности)
//   и сравнивается со своей прежней копией: изменился — значит, пришло событие, его поля уже устойчивы.
//
// ПРИЗНАК СБРОСА EVO
//   BIOS Evo при сбросе шлёт по этому SPI команду FT812 RST_PULSE (байты 68 00 00) — по ней ядро
//   возвращается в слот 0 (bz_top). Чтение по этому SPI ядро не поддерживает (MISO = 0).
module bz_evo_in (
    input  wire        clk,
    input  wire        rst_n,
    // SPI Evo
    input  wire        sck,
    input  wire        cs_n,
    input  wire        mosi,
    output wire        miso,
    // настройки и признаки
    input  wire        page,          // старший бит адреса SDRAM
    output reg         evo_rst,       // такт: принята RST_PULSE (68 00 00)
    output reg         active,        // такт: принят байт (для правила «Evo молчит»)
    output reg  [31:0] bytes_in,      // счётчик принятых байт данных
    // клиент моста
    output reg         b_req,
    output wire        b_we,
    output reg  [20:0] b_addr,
    input  wire        b_ack,
    input  wire        b_wr_take,
    input  wire [5:0]  b_wr_idx,
    output wire [31:0] b_wdata,
    output wire [3:0]  b_wmask
);
    localparam [1:0] EV_START = 2'd0, EV_WRITE = 2'd1;

    wire       tg;
    wire [1:0] ev_kind;
    wire [7:0] ev_h0, ev_h1, ev_b;

    ft_spi rx (
        .sck(sck), .cs_n(cs_n), .mosi(mosi), .miso(miso),
        .tg(tg), .ev_kind(ev_kind), .ev_h0(ev_h0), .ev_h1(ev_h1), .ev_b(ev_b), .rd_byte(8'd0)
    );

    // событие: переключение tg; поля держатся не меньше 8 тактов SCK (≥ 280 нс) — 3 триггера
    // синхронизатора (50 нс) и такт разбора укладываются
    (* ASYNC_REG = "TRUE" *) reg [2:0] tg_s;
    (* ASYNC_REG = "TRUE" *) reg [2:0] cs_s;
    wire ev = tg_s[2] ^ tg_s[1];

    reg        wr_on;                  // посылка — запись
    reg [22:0] wa;                     // адрес следующего байта
    // (в триггерах, не в распределённой памяти: пачку копируют целиком за такт — память копировалась бы
    // на каждое из 8 слов, 64 блока SSRAM в сборке этапа B)
    (* syn_ramstyle = "registers" *) reg [31:0] buf_w [0:7];            // собираемая пачка
    (* syn_ramstyle = "registers" *) reg [3:0]  buf_m [0:7];
    reg [17:0] buf_line;               // её номер (адрес [22:5])
    reg        buf_any;
    (* syn_ramstyle = "registers" *) reg [31:0] pend_w [0:7];           // отданная мосту пачка
    (* syn_ramstyle = "registers" *) reg [3:0]  pend_m [0:7];
    reg        pend_busy, pend_taking;
    reg [3:0]  take_cnt;

    assign b_we    = 1'b1;
    assign b_wdata = pend_w[b_wr_idx[2:0]];
    assign b_wmask = pend_m[b_wr_idx[2:0]];

    wire flush_now = buf_any && !pend_busy;
    integer i;

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tg_s <= 3'd0;
            cs_s <= 3'b111;
            wr_on <= 1'b0;
            buf_any <= 1'b0;
            pend_busy <= 1'b0;
            pend_taking <= 1'b0;
            b_req <= 1'b0;
            evo_rst <= 1'b0;
            active <= 1'b0;
            bytes_in <= 32'd0;
            for (i = 0; i < 8; i = i + 1)
                buf_m[i] <= 4'd0;
        end else begin
            tg_s    <= {tg_s[1:0], tg};
            cs_s    <= {cs_s[1:0], cs_n};
            evo_rst <= 1'b0;
            active  <= 1'b0;

            // мост забирает пачку
            if (b_req && b_ack) begin
                b_req       <= 1'b0;
                pend_taking <= 1'b1;
                take_cnt    <= 4'd0;
            end
            if (pend_taking && b_wr_take) begin
                take_cnt <= take_cnt + 1'b1;
                if (take_cnt == 4'd7) begin
                    pend_taking <= 1'b0;
                    pend_busy   <= 1'b0;
                end
            end

            if (ev) begin
                active <= 1'b1;
                if (ev_kind == EV_START) begin
                    wr_on <= ev_h0[7:6] == 2'b10;
                    wa    <= {page, ev_h0[5:0], ev_h1, ev_b};
                    if (ev_h0 == 8'h68 && ev_h1 == 8'h00 && ev_b == 8'h00)
                        evo_rst <= 1'b1;
                end else if (ev_kind == EV_WRITE && wr_on) begin
                    // байт в пачку; новая строка 32 байта — сначала отдать старую (ниже)
                    if (!buf_any || buf_line == wa[22:5]) begin
                        buf_w[wa[4:2]][8 * wa[1:0] +: 8] <= ev_b;
                        buf_m[wa[4:2]][wa[1:0]]          <= 1'b1;
                        buf_line <= wa[22:5];
                        buf_any  <= 1'b1;
                        wa       <= wa + 1'b1;
                        bytes_in <= bytes_in + 1'b1;
                    end
                    // иначе байт пропал бы: этого не бывает — пачка уходит раньше (см. ниже),
                    // мосту на это нужны десятки тактов, а следующий байт — через ≥ 17
                end
            end

            // пачка готова (дошли до конца строки 32 байта или посылка кончилась) — отдать мосту
            if (flush_now && (cs_s[2] || (wr_on && wa[22:5] != buf_line))) begin
                for (i = 0; i < 8; i = i + 1) begin
                    pend_w[i] <= buf_w[i];
                    pend_m[i] <= buf_m[i];
                    buf_m[i]  <= 4'd0;
                end
                b_addr    <= {buf_line, 3'b000};
                b_req     <= 1'b1;
                pend_busy <= 1'b1;
                buf_any   <= 1'b0;
            end
            if (cs_s[2])
                wr_on <= 1'b0;
        end
    end
endmodule
