// =====================================================================================================
// bz_top.sv — верхний модуль ядра ПЛИС «бомж-3D»: Quake 2 на VDAC4 (Tang Nano 20K, GW2AR-18).
// =====================================================================================================
//
// ЗАЧЕМ ЭТО ЯДРО
//   Quake 2 на VDAC4 делится так: ESP32-S3 крутит игру и готовит каждый кадр (какие стены видны, где
//   они на экране, как на них лежит текстура), а ПЛИС только закрашивает точки — как видеокарта. ESP
//   шлёт ПЛИС поток команд «нарисуй многоугольник с такими-то вершинами и градиентами», ПЛИС рисует в
//   SDRAM и показывает кадр на HDMI. Формат команд и вся арифметика — gpu/SPEC.md, образец «как должно
//   получиться до бита» — C-модель gpu/model/bomzh.c. Устройство ядра и план — gpu/rtl/ARCH.md.
//
// ЧТО ТАКОЕ ВЕРХНИЙ МОДУЛЬ
//   Схема ПЛИС собирается из модулей, как программа из функций. Верхний модуль — тот, чьи порты —
//   настоящие выводы микросхемы (их имена и номера ножек — в constraints/bz_top.cst). Сам он почти ничего
//   не считает: он заводит такты, раздаёт сбросы и соединяет модули проводами.
//
// ИЗ ЧЕГО СОСТОИТ (по пути данных)
//
//   ESP ──LINK (QSPI)──► fb_link ──► регистры + bz_fifo (очередь команд) ──► bz_gpu (рисует: ядро,
//   Evo ──SPI 28 МГц──► bz_evo_in (загрузки текстур прямо в SDRAM,            растеризатор, конвейер
//                       признак сброса Evo)                                    точки, 6 клиентов SDRAM)
//                                                                                          │
//                       bz_scan ◄── строки кадра ── sd_bridge ◄── арбитр ◄─────────────────┘
//                          │                        │
//                          ▼                        ▼
//                    hdmi_tx + сериализатор   sdram_train/sdram_burst ──► SDRAM 8 МБ (в корпусе GW2AR)
//                          │
//                          ▼ HDMI 1280×720p60 (картинка 424×240 × 3 по центру; 95 и 90 % — как в слоте 0)
//
//   Ключ BZ_STAGEA (build_bz.ps1 -StageA): вместо bz_gpu — ядро этапа A (bz_core, точка за точкой через
//   bz_memif), образец для сверки; с -No3D — оно же без POLY и POINT (пробная сборка оболочки).
//
//   BL616 ──SPI──► cp_spi: BL616 читает «снимок состояния»; бит «перезагрузи меня» — возврат в слот 0.
//
// ТАКТЫ (ДОМЕНЫ)
//   Такт — меандр, по фронту которого срабатывают все триггеры своей части схемы. Части с разными
//   тактами называются доменами; сигнал из домена в домен передают только особыми схемами (CDC, clock
//   domain crossing — переключатель с синхронизатором из 2–3 триггеров), иначе триггер может поймать
//   сигнал в момент смены и «зависнуть» между 0 и 1 (метастабильность). Здесь домены:
//     clk_serial 371,25 МГц — побитная выдача TMDS (10 бит на точку);
//     pixel_clk   74,25 МГц — точки 720p60 (371,25 / 5);
//     clk_sd     121,5  МГц — контроллер SDRAM (вторая PLL от кварца, как у FT-ядра 1.45);
//     clk_mem    60,75  МГц — ядро, очередь, LINK-регистры, клиенты SDRAM (ровно половина clk_sd);
//     link_sck, ft_sck, cp_sck — такты самих SPI (ими тактируются приёмники, см. их модули).
//   PLL (ФАПЧ, фазовая автоподстройка частоты) — блок ПЛИС, который из кварца 27 МГц делает нужные
//   частоты. Пока PLL не «захватила» частоту (LOCK = 0), её домен держится в сбросе.
//
// ВЫВОДЫ — как у FT-ядра VDAC4 (ts_fb_hdmi_top): те же имена портов, тот же .cst; выводы TMDS — с
// DRIVE=4 (с DRIVE=8 экран у пользователя терял сигнал, 31.10.2026).
module bz_top (
    input  wire        clk27,          // кварц Tang 27 МГц (вывод 4)

    // RECONFIG_N: ядро тянет к 0, когда просит перезагрузку (основной путь — через BL616, ниже)
    output tri         reconfig_n,

    // Evo
    input  wire        m_sel,          // M_SEL: 1 — Evo отдала монитор ядру Tang (плагин Quake 2 держит 1)
    output wire        ft_int_n,       // прерывание FT812 к Evo — у нас не нужно, держим 1 (нет прерывания)
    input  wire        ft_sck,         // SPI Z-контроллера Evo (адрес FT812): второй вход ядра
    input  wire        ft_cs_n,
    input  wire        ft_mosi,
    output wire        ft_miso,

    // BL616 (ведущий SPI): снимок состояния и просьба о перезагрузке
    input  wire        cp_sck,
    input  wire        cp_cs_n,
    input  wire        cp_mosi,
    output wire        cp_miso,
    output wire        cp_irq_n,

    // LINK к ESP32-S3 (QSPI, ESP — ведущий): регистры и очередь команд
    input  wire        link_sck,
    input  wire        link_cs_n,
    inout  wire [3:0]  link_io,
    output tri         link_irq_n,

    output wire        pa_en,          // усилитель звука Tang — выключен (звук Quake 2 идёт через ESP)

    // HDMI
    output wire        tmds_clk_p,
    output wire        tmds_clk_n,
    output wire [2:0]  tmds_data_p,
    output wire [2:0]  tmds_data_n,

    // SDRAM в корпусе GW2AR-18 (имена портов задаёт Gowin, выводы не назначаются)
    output wire        O_sdram_clk,
    output wire        O_sdram_cke,
    output wire        O_sdram_cs_n,
    output wire        O_sdram_cas_n,
    output wire        O_sdram_ras_n,
    output wire        O_sdram_wen_n,
    output wire [3:0]  O_sdram_dqm,
    output wire [10:0] O_sdram_addr,
    output wire [1:0]  O_sdram_ba,
    inout  wire [31:0] IO_sdram_dq
);
    // =================================================================================================
    // 1. ТАКТЫ И СБРОСЫ
    // =================================================================================================
    wire clk_serial, pixel_clk, lock_xtal;
    wire clk_mem, clk_sd, sdram_clk_p, lock_mem;
    wire [3:0] sd_phase;               // сдвиг такта микросхемы SDRAM — его подбирает sdram_train

    // PLL 1 — только HDMI: 27 × 55 / 4 = 371,25 МГц; делитель на 5 — 74,25 МГц (точка 720p)
    fb_pll_xtal #(.HD(1'b1), .OUT768(1'b0)) pll_hdmi (
        .clkin(clk27), .reset(1'b0), .clk_serial(clk_serial), .clk_mem(), .clk_sd(), .lock(lock_xtal)
    );
    ts_clkdiv5 clkdiv (.hclkin(clk_serial), .resetn(lock_xtal), .clkout(pixel_clk));

    // PLL 2 — SDRAM и ядро: 121,5 МГц, половина 60,75 МГц и такт микросхемы со сдвигом (CLKOUTP)
    fb_pll_sd pll_sd (
        .clkin(clk27), .phase(sd_phase), .clk_sd(clk_sd), .clk_mem(clk_mem),
        .clkoutp(sdram_clk_p), .lock(lock_mem)
    );

    // Сброс домена: сдвиговый регистр из 4 триггеров. Пока PLL без захвата — все нули (сброс). После
    // захвата в него по одному «вдвигаются» единицы, и через 4 такта сброс снимается — синхронно с
    // тактом своего домена (асинхронно снимать сброс нельзя: часть триггеров увидела бы его на такт
    // позже других).
    reg [3:0] rst_mem_sr, rst_sd_sr, rst_pix_sr;
    always @(posedge clk_mem or negedge lock_mem)
        if (!lock_mem) rst_mem_sr <= 4'd0; else rst_mem_sr <= {rst_mem_sr[2:0], 1'b1};
    always @(posedge clk_sd or negedge lock_mem)
        if (!lock_mem) rst_sd_sr <= 4'd0; else rst_sd_sr <= {rst_sd_sr[2:0], 1'b1};
    always @(posedge pixel_clk or negedge lock_xtal)
        if (!lock_xtal) rst_pix_sr <= 4'd0; else rst_pix_sr <= {rst_pix_sr[2:0], 1'b1};
    wire rst_mem_n = rst_mem_sr[3];
    wire rst_sd_n  = rst_sd_sr[3];
    wire rst_pix_n = rst_pix_sr[3];

    // =================================================================================================
    // 2. LINK: регистры и очередь команд (домен clk_mem)
    // =================================================================================================
    // fb_link принимает транзакции ESP (команда, адрес, слова) по своему такту link_sck и отдаёт
    // каждое записанное слово в домен clk_mem импульсом wr_stb. Адрес — номер 32-битного слова.
    //
    // Карта (байтовый адрес → смысл):
    //   0x00 ID        R  «VDC4» — общий для ядер VDAC4
    //   0x04 VERSION   R  {личность 0x51 ('Q', Quake 2), протокол LINK 01, версия ядра 0.01}
    //   0x08 STATUS    R  0 — очередь переполнялась, 1 — неизвестная команда, 2 — SDRAM готова,
    //                     3 — просим перезагрузку, 4 — Evo включала M_SEL, 5 — M_SEL сейчас
    //   0x0C FIFO_FREE R  сколько слов ещё можно положить в очередь (двоичное, прямо со счётчика — см. FREE_GRAY)
    //   0x10 FRAMES    R  сколько команд FRAME выполнено (ESP так видит, что кадр готов)
    //   0x14 EVO_PAGE  RW бит 0 — старший бит адреса SDRAM для загрузок с Evo
    //   0x18 EVO_BYTES R  байт принято с Evo
    //   0x1C FREE_GRAY R  (0.03) {0x47 'G', 12'd0, FIFO_FREE в коде Грея} — его читает Q2.ELF (см. ниже)
    //   0x3C SCRATCH   RW проверка связи
    //   0x88 RD_ADDR   W  (0.02, SPEC 0.4) адрес SDRAM для копии в окно (байтовый, кратен 32)
    //   0x8C RD_LEN    W  (0.02) пачек по 32 байта, 1…128; запись запускает копию (bz_rdwin)
    //   0x90 MARK      R  (0.02) число последней выполненной команды MARK
    //   0x1000…0x1FFC  R  (0.02) окно чтения SDRAM — 4 КБ; STATUS бит 6 — копия ещё идёт
    //   0x84 SIZE      RW {0x535A, размер}: 0 — 100 %, 1 — 95 %, 2 — 90 % (как у ядра NES; пишет Q2.ELF,
    //                     взяв SIZE слота 0; запись без 0x535A в старших битах не меняет размер)
    //   0xEC SD_TRAIN  R  итог подбора фазы SDRAM (как у FT-ядра)
    //   0xF0 BOOT      W  0x424F4F54 («BOOT») — назад в слот 0
    //   0x40000…0x7FFFC   окно очереди: любое записанное сюда слово — следующее слово потока команд
    //                     (ESP пишет одной длинной транзакцией, адрес растёт сам)
    wire        lk_wr_stb;
    wire [17:0] lk_wr_addr;
    wire [31:0] lk_wr_data;
    wire [11:0] lk_rd_addr;
    reg  [31:0] lk_rd_data;
    wire [3:0]  link_io_out;
    wire        link_io_oe;

    fb_link link (
        .sck(link_sck), .cs_n(link_cs_n), .io_in(link_io), .io_out(link_io_out), .io_oe(link_io_oe),
        .clk(clk_mem), .rst_n(rst_mem_n),
        .wr_stb(lk_wr_stb), .wr_addr(lk_wr_addr), .wr_data(lk_wr_data),
        .rd_addr(lk_rd_addr), .rd_data(lk_rd_data)
    );
    // двунаправленные линии QSPI: выдаём только когда fb_link отвечает (чтение), иначе «Z» — отпущены
    assign link_io    = link_io_oe ? link_io_out : 4'hz;
    assign link_irq_n = 1'bz;          // не ведём: подтяжку держит плата (так же у FT-ядра)

    // очередь команд
    wire        q_push = lk_wr_stb && lk_wr_addr[17:16] == 2'b01;   // окно 0x40000…0x7FFFC
    wire [31:0] cmd_q;
    wire        cmd_valid, cmd_ready, fifo_ovf;
    wire [11:0] fifo_free;
    bz_fifo #(.AW(11)) fifo (
        .clk(clk_mem), .rst_n(rst_mem_n),
        .push(q_push), .wdata(lk_wr_data), .overflow(fifo_ovf),
        .q(cmd_q), .q_valid(cmd_valid), .pop(cmd_valid && cmd_ready), .free(fifo_free)
    );

    // FIFO_FREE для ESP (24.11.2026, ядро 0.03). Число живёт в домене clk_mem, а fb_link берёт слово
    // для ответа по фронту link_sck — без всякой связи с clk_mem. Раньше туда шёл прямо выход вычитателя
    // (wp − rp − q_valid): попади фронт SCK на смену счётчика — часть битов старая, часть новая, мусор
    // (на плате 0.1.167 ESP прочитала 0x858 при очереди в 2048 слов; Q2.ELF принял бы его за место).
    // Теперь число сначала ложится в регистр clk_mem, а для ESP есть копия в коде Грея: соседние числа
    // в нём отличаются одним битом, а FIFO_FREE за такт clk_mem меняется не больше чем на 1 (push −1;
    // pop без подгрузки +1; подгрузка выхода — 0; push и pop вместе — 0 или +1 − 1). Значит, захват по
    // SCK посреди смены даёт старое или новое число, а не смесь. Перекос путей между битами (единицы нс
    // внутри ПЛИС) много меньше такта clk_mem (16,5 нс) — две смены подряд в окно захвата не попадут.
    // 0x0C (двоичное) — как у 0.02, прямо со счётчика: Q2.ELF с ядром 0.03 его не читает, а лишние 12
    // триггеров на забитом кристалле (CLS 96 %) не нужны.
    reg [11:0] free_gray_q;
    always @(posedge clk_mem or negedge rst_mem_n) begin
        if (!rst_mem_n) begin
            free_gray_q <= 12'h800 ^ 12'h400;          // пустая очередь: 2048 слов, в коде Грея
        end else begin
            free_gray_q <= fifo_free ^ {1'b0, fifo_free[11:1]};
        end
    end

    // регистры, которые пишет ESP
    reg [31:0] scratch;
    reg        evo_page;
    reg [1:0]  size;
    reg        boot_link;              // ESP написала BOOT
    reg [22:0] rd_addr_q;              // RD_ADDR (SPEC 0.4)
    reg [7:0]  rd_len_q;               // RD_LEN
    reg        rd_go;                  // такт: ESP записала RD_LEN — начать копию в окно
    always @(posedge clk_mem or negedge rst_mem_n) begin
        if (!rst_mem_n) begin
            scratch <= 32'd0; evo_page <= 1'b1; size <= 2'd0; boot_link <= 1'b0;
            rd_addr_q <= 23'd0; rd_len_q <= 8'd0; rd_go <= 1'b0;
        end else begin
          rd_go <= 1'b0;
          if (lk_wr_stb) begin
            case (lk_wr_addr)
                18'h22: rd_addr_q <= lk_wr_data[22:0];
                18'h23: begin rd_len_q <= lk_wr_data[7:0]; rd_go <= 1'b1; end
                18'h05: evo_page <= lk_wr_data[0];
                18'h0F: scratch  <= lk_wr_data;
                18'h21: if (lk_wr_data[31:16] == 16'h535A && lk_wr_data[1:0] != 2'd3) size <= lk_wr_data[1:0];
                18'h3C: if (lk_wr_data == 32'h424F_4F54) boot_link <= 1'b1;
                default: ;
            endcase
          end
        end
    end

    // =================================================================================================
    // 3. РИСОВАНИЕ: bz_gpu (этап B) или bz_core + bz_memif (этап A, ключ BZ_STAGEA)
    // =================================================================================================
    wire        bad_cmd, frame_pulse, swap_ack;
    wire [31:0] gpu_mark;               // регистр MARK (SPEC 0.4)
    wire [22:0] shown_fb;
    wire [31:0] shown_blend;
    wire        pal_we;
    wire [9:0]  pal_addr;
    wire [7:0]  pal_data;
`ifdef BZ_STAGEA
    wire        core_req, core_we, core_ack, core_idle;
    wire [22:0] core_addr;
    wire [31:0] core_wdata, core_rdata;
    wire [3:0]  core_be;
    assign gpu_mark = 32'd0;           // этап A (образец): MARK не знает

    bz_core core (
        .clk(clk_mem), .rst(!rst_mem_n),
        .cmd_data(cmd_q), .cmd_valid(cmd_valid), .cmd_ready(cmd_ready),
        .mem_req(core_req), .mem_we(core_we), .mem_addr(core_addr), .mem_wdata(core_wdata),
        .mem_be(core_be), .mem_ack(core_ack), .mem_rdata(core_rdata),
        .shown_fb(shown_fb), .shown_blend(shown_blend), .frame_pulse(frame_pulse), .swap_ack(swap_ack),
        .bad_cmd(bad_cmd), .idle(core_idle),
        .pal_we(pal_we), .pal_addr(pal_addr), .pal_data(pal_data)
    );
`endif

    // FRAMES — сколько кадров ядро уже сдало выводу
    reg [31:0] frames;
    always @(posedge clk_mem or negedge rst_mem_n)
        if (!rst_mem_n) frames <= 32'd0; else if (frame_pulse) frames <= frames + 1'b1;

    // =================================================================================================
    // 4. SDRAM: клиенты → арбитр → мост → контроллер
    // =================================================================================================
    // Клиенты моста sd_bridge говорят на одном языке: запрос (req, we, адрес слова, длина в пачках − 1,
    // метка) держится до ack; слова записи мост забирает потом сам (wr_take + номер слова), слова
    // чтения приходят потом (rd_valid + номер). Чьё слово — видно по метке (tag): у каждого клиента
    // своя. Арбитр — простой приоритет: вывод (иначе на экране провал) > загрузки с Evo (SPI не
    // подождёт) > рисование. У рисования этапа B клиентов шесть со своими метками (bz_gpu.sv), у него
    // свой арбитр внутри; ответы моста он разбирает сам — метки вывода и Evo среди его меток нет.
    localparam [2:0] TAG_SCAN = 3'b000, TAG_CORE = 3'b010, TAG_EVO = 3'b110;

    wire        sc_req, sc_ack, sc_rd_valid;
    wire [20:0] sc_addr;
    wire        ev_req, ev_we, ev_ack, ev_wr_take;
    wire [20:0] ev_addr;
    wire [31:0] ev_wdata;
    wire [3:0]  ev_wmask;
    wire        gp_req, gp_we;              // порт рисования
    wire [20:0] gp_addr;
    wire [2:0]  gp_tag;
    wire [31:0] gp_wdata;
    wire [3:0]  gp_wmask;

    wire        sd_ack, sd_wr_take, sd_rd_valid, mem_ready;
    wire [2:0]  sd_ack_tag, sd_wr_tag, sd_rd_tag;
    wire [5:0]  sd_wr_idx, sd_rd_idx;
    wire [31:0] sd_rd_data;

    // Копия SDRAM в окно для ESP (bz_rdwin, SPEC 0.4) — между Evo и рисованием: ESP ждёт её посреди своего
    // кадра, а рисование подождёт. Свободной метки нет (8 из 8: вывод, Evo, 6 у рисования), поэтому она
    // делит метку с загрузками Evo: те только пишут, копия только читает — слова чтения с этой меткой
    // все её. Чей ack — помнит rb_sel_q: мост принимает запрос по фронту, а ack выставляет тактом позже.
    wire        rb_req, rb_ack, rb_rd_valid;
    wire [20:0] rb_addr;
    wire sel_sc = sc_req;
    wire sel_ev = !sc_req && ev_req;
    wire sel_rb = !sc_req && !ev_req && rb_req;
    wire sd_req = sc_req || ev_req || rb_req || gp_req;
    wire        sd_we   = sel_sc ? 1'b0 : sel_ev ? ev_we : sel_rb ? 1'b0 : gp_we;
    wire [20:0] sd_addr = sel_sc ? sc_addr : sel_ev ? ev_addr : sel_rb ? rb_addr : gp_addr;
    wire [2:0]  sd_tag  = sel_sc ? TAG_SCAN : (sel_ev || sel_rb) ? TAG_EVO : gp_tag;
    reg         rb_sel_q;
    always @(posedge clk_mem or negedge rst_mem_n)
        if (!rst_mem_n) rb_sel_q <= 1'b0; else rb_sel_q <= sel_rb;
    // ответы — по метке
    assign sc_ack      = sd_ack && sd_ack_tag == TAG_SCAN;
    assign ev_ack      = sd_ack && sd_ack_tag == TAG_EVO && !rb_sel_q;
    assign rb_ack      = sd_ack && sd_ack_tag == TAG_EVO && rb_sel_q;
    assign rb_rd_valid = sd_rd_valid && sd_rd_tag == TAG_EVO;   // Evo не читает
    assign ev_wr_take  = sd_wr_take && sd_wr_tag == TAG_EVO;
    assign sc_rd_valid = sd_rd_valid && sd_rd_tag == TAG_SCAN;
    wire [31:0] sd_wdata = (sd_wr_tag == TAG_EVO) ? ev_wdata : gp_wdata;
    wire [3:0]  sd_wmask = (sd_wr_tag == TAG_EVO) ? ev_wmask : gp_wmask;

`ifdef BZ_STAGEA
    wire        mi_ack      = sd_ack && sd_ack_tag == TAG_CORE;
    wire        mi_wr_take  = sd_wr_take && sd_wr_tag == TAG_CORE;
    wire        mi_rd_valid = sd_rd_valid && sd_rd_tag == TAG_CORE;
    assign gp_tag = TAG_CORE;
    bz_memif memif (
        .clk(clk_mem), .rst_n(rst_mem_n),
        .c_req(core_req), .c_we(core_we), .c_addr(core_addr), .c_wdata(core_wdata), .c_be(core_be),
        .c_nocache(1'b0), .inv(1'b0),           // ядру этапа A забор и сброс не нужны
        .c_ack(core_ack), .c_rdata(core_rdata),
        .b_req(gp_req), .b_we(gp_we), .b_addr(gp_addr), .b_ack(mi_ack),
        .b_wr_take(mi_wr_take), .b_wr_idx(sd_wr_idx), .b_wdata(gp_wdata), .b_wmask(gp_wmask),
        .b_rd_valid(mi_rd_valid), .b_rd_idx(sd_rd_idx), .b_rd_data(sd_rd_data)
    );
`else
    wire        gpu_idle;
    wire [7:0]  gpu_dbg;
    bz_gpu gpu (
        .clk(clk_mem), .rst(!rst_mem_n),
        .cmd_data(cmd_q), .cmd_valid(cmd_valid), .cmd_ready(cmd_ready),
        .m_req(gp_req), .m_we(gp_we), .m_addr(gp_addr), .m_tag(gp_tag),
        .m_ack(sd_ack), .m_ack_tag(sd_ack_tag), .m_wr_take(sd_wr_take), .m_wr_idx(sd_wr_idx),
        .m_wr_tag(sd_wr_tag), .m_wdata(gp_wdata), .m_wmask(gp_wmask),
        .m_rd_valid(sd_rd_valid), .m_rd_idx(sd_rd_idx), .m_rd_tag(sd_rd_tag), .m_rd_data(sd_rd_data),
        .shown_fb(shown_fb), .shown_blend(shown_blend), .frame_pulse(frame_pulse), .swap_ack(swap_ack),
        .bad_cmd(bad_cmd), .mark(gpu_mark), .pal_we(pal_we), .pal_addr(pal_addr), .pal_data(pal_data),
        .idle(gpu_idle), .dbg_pix(gpu_dbg)
    );
`endif

    // мост clk_mem → clk_sd и контроллер с подбором фазы (как у FT-ядра 1.45 в слоте 1)
    wire        c_req, c_we, c_ack, c_wr_take, c_rd_valid, c_init, c_tag, c_rd_tag;
    wire [20:0] c_addr;
    wire [2:0]  c_len;
    wire [31:0] c_wdata, c_rd_data;
    wire [3:0]  c_wmask;
    wire [22:0] sd_train_st;
    wire [31:0] dq_out, dq_in;
    wire        dq_oe;

    // PREFETCH_HEAD: признаки головы очереди — из регистра (короче путь «указатель → память → c_addr»):
    // в сборке этапа B 23.11 он не уложился в такт clk_sd 121,5 МГц (−0,44 нс)
    sd_bridge #(.TAGW(3), .PREFETCH_HEAD(1'b1)) bridge (
        .clk(clk_mem), .rst_n(rst_mem_n), .clk_sd(clk_sd), .rst_sd_n(rst_sd_n),
        .req(sd_req), .we(sd_we), .addr(sd_addr), .req_len(3'd0), .req_tag(sd_tag),
        .ack(sd_ack), .ack_tag(sd_ack_tag),
        .wr_take(sd_wr_take), .wr_idx(sd_wr_idx), .wr_tag(sd_wr_tag),
        .wdata(sd_wdata), .wmask(sd_wmask), .wdata2(32'd0),
        .rd_valid(sd_rd_valid), .rd_idx(sd_rd_idx), .rd_tag(sd_rd_tag), .rd_data(sd_rd_data),
        .rd_phase(2'd0), .init_done(mem_ready),
        .c_req(c_req), .c_we(c_we), .c_addr(c_addr), .c_len(c_len), .c_ack(c_ack),
        .c_wr_take(c_wr_take), .c_wdata(c_wdata), .c_wmask(c_wmask),
        .c_rd_valid(c_rd_valid), .c_rd_data(c_rd_data), .c_rd_phase(), .c_init_done(c_init),
        .c_tag(c_tag), .c_rd_tag(c_rd_tag)
    );
    // Такты из паспорта SDRAM на 121,5 МГц (такт 8,2 нс): tRCD и tRP 15 нс → 2 такта, tWR 12 нс → 2,
    // tRFC 60 нс → 8; CAS latency 3. Фазу такта микросхемы (шаг 1/16 периода) после включения
    // подбирает sdram_train: пишет и читает проверочные слова на всех фазах и берёт середину окна.
    sdram_train #(.TAGW(1), .CLK_KHZ(121_500), .CL(3), .T_RCD(2), .T_RP(2), .T_WR(2), .T_RFC(8),
                  .INIT_US(200), .DEF_PHASE(4'd12)) sdram (
        .clk(clk_sd), .rst_n(rst_sd_n),
        .req(c_req), .we(c_we), .addr(c_addr), .req_len(c_len), .req_tag(c_tag),
        .ack(c_ack), .op_tag(), .wr_take(c_wr_take), .wr_idx(), .wdata(c_wdata), .wmask(c_wmask),
        .rd_valid(c_rd_valid), .rd_idx(), .rd_data(c_rd_data), .rd_tag(c_rd_tag), .init_done(c_init),
        .phase(sd_phase), .train_st(sd_train_st), .train_win(),
        .sdram_cs_n(O_sdram_cs_n), .sdram_ras_n(O_sdram_ras_n), .sdram_cas_n(O_sdram_cas_n),
        .sdram_we_n(O_sdram_wen_n), .sdram_a(O_sdram_addr), .sdram_ba(O_sdram_ba), .sdram_dqm(O_sdram_dqm),
        .sdram_dq_out(dq_out), .sdram_dq_oe(dq_oe), .sdram_dq_in(dq_in)
    );
    // шина данных SDRAM двунаправленная: выдаём слово только при записи, иначе отпускаем («Z»)
    assign IO_sdram_dq = dq_oe ? dq_out : 32'hzzzz_zzzz;
    assign dq_in       = IO_sdram_dq;  // чтение — прямо с выводов: регистр захвата стоит у самого вывода
    assign O_sdram_cke = 1'b1;
    assign O_sdram_clk = sdram_clk_p;  // такт микросхемы — со сдвигом, который подобрал sdram_train

    // =================================================================================================
    // 5. ВТОРОЙ ВХОД: SPI Evo → SDRAM
    // =================================================================================================
    wire        evo_rst, evo_active;
    wire [31:0] evo_bytes;
    bz_evo_in evo (
        .clk(clk_mem), .rst_n(rst_mem_n),
        .sck(ft_sck), .cs_n(ft_cs_n), .mosi(ft_mosi), .miso(ft_miso),
        .page(evo_page), .evo_rst(evo_rst), .active(evo_active), .bytes_in(evo_bytes),
        .b_req(ev_req), .b_we(ev_we), .b_addr(ev_addr), .b_ack(ev_ack),
        .b_wr_take(ev_wr_take), .b_wr_idx(sd_wr_idx), .b_wdata(ev_wdata), .b_wmask(ev_wmask)
    );
    assign ft_int_n = 1'b1;

    // =================================================================================================
    // 6. ВЫВОД HDMI
    // =================================================================================================
    wire [7:0] vr, vg, vb;
    wire       vde, vhs, vvs;
    bz_scan scan (
        .clk(clk_mem), .rst_n(rst_mem_n),
        .shown_fb(shown_fb), .frame_pulse(frame_pulse), .shown_blend(shown_blend), .swap_ack(swap_ack),
        .pal_we(pal_we), .pal_addr(pal_addr), .pal_data(pal_data), .size(size),
        .b_req(sc_req), .b_addr(sc_addr), .b_ack(sc_ack), .b_rd_valid(sc_rd_valid), .b_rd_data(sd_rd_data),
        .pixel_clk(pixel_clk), .prst_n(rst_pix_n),
        .r(vr), .g(vg), .b(vb), .de(vde), .hs(vhs), .vs(vvs)
    );

    // hdmi_tx — кодирует точки в слова TMDS (8 бит → 10) и раз в кадр вставляет пакет AVI InfoFrame:
    // «RGB полного диапазона, 16:9, без обрезки краёв, это игра». VIC 4 — номер формата 1280×720p60.
    wire [9:0] t_blue, t_green, t_red;
    hdmi_tx #(.HDMI(1'b1), .VIC(8'd4), .PIC_AR(2'b10)) hdmi (
        .pixel_clk(pixel_clk), .reset_n(rst_pix_n),
        .r(vr), .g(vg), .b(vb), .de(vde), .hs(vhs), .vs(vvs),
        .tmds_blue(t_blue), .tmds_green(t_green), .tmds_red(t_red)
    );
    // сериализатор: 10 бит слова TMDS за один pixel_clk выдаются побитно на clk_serial (×5, оба фронта)
    tmds_serializer_gowin serializer (
        .pixel_clk(pixel_clk), .serial_clk(clk_serial), .reset(!rst_pix_n),
        .channel0(t_blue), .channel1(t_green), .channel2(t_red),
        .tmds_clk_p(tmds_clk_p), .tmds_clk_n(tmds_clk_n),
        .tmds_data_p(tmds_data_p), .tmds_data_n(tmds_data_n)
    );

    // =================================================================================================
    // 7. ВОЗВРАТ В СЛОТ 0 (TS-Config)
    // =================================================================================================
    // Ядро просит перезагрузку ПЛИС (boot_req) в трёх случаях:
    //   1) ESP написала BOOT по LINK (выход из игры, F12 и т. п.);
    //   2) BIOS Evo при сбросе прислал по SPI команду FT812 RST_PULSE (68 00 00) — Evo сброшена;
    //   3) плагин Evo раньше включал M_SEL (игра шла), а теперь M_SEL = 0 полсекунды и Evo по SPI
    //      молчит — значит Evo сброшена или плагин вышел без BOOT. Правило — как у FT-ядра VDAC4.
    // Сам перезапуск делает BL616: он видит бит «перезагрузка» в снимке состояния (cp_spi) и линию
    // cp_irq_n = 0 и даёт ПЛИС команду JTAG Reconfig — ПЛИС грузит слот 0. Свой RECONFIG_N тоже тянем к 0
    // (запасной путь; на столе он ПЛИС не перезагружал, 18.10.2026).
    localparam int IDLE_CLKS = 30_375_000;     // 0,5 с на 60,75 МГц
    (* ASYNC_REG = "TRUE" *) reg [2:0] msel_s; // M_SEL — с вывода, через синхронизатор
    reg        armed;                  // M_SEL уже был 1: игра шла
    reg [24:0] idle_cnt;
    reg        boot_req;
    always @(posedge clk_mem or negedge rst_mem_n) begin
        if (!rst_mem_n) begin
            msel_s <= 3'd0; armed <= 1'b0; idle_cnt <= 25'd0; boot_req <= 1'b0;
        end else begin
            msel_s <= {msel_s[1:0], m_sel};
            if (msel_s[2])
                armed <= 1'b1;
            if (msel_s[2] || evo_active)
                idle_cnt <= 25'd0;
            else if (idle_cnt != IDLE_CLKS[24:0])
                idle_cnt <= idle_cnt + 1'b1;
            if (boot_link || evo_rst || (armed && idle_cnt == IDLE_CLKS[24:0]))
                boot_req <= 1'b1;
        end
    end

    // Снимок состояния для BL616 (16 байт, байт k — st_vec[8k +: 8]): 0xC5, версия 1, флаги
    // (бит 7 — «перезагрузи ПЛИС»). Остальное у этого ядра — нули.
    wire [127:0] st_vec = {104'd0, boot_req, 7'd0, 8'h01, 8'hC5};
    cp_spi cp (
        .sck(cp_sck), .cs_n(cp_cs_n), .mosi(cp_mosi), .miso(cp_miso),
        .tg(), .ev_kind(), .ev_data(), .st_vec(st_vec), .rb_addr(), .rb_q(32'd0)
    );
    assign cp_irq_n   = !boot_req;     // низкий уровень — «BL616, обрати внимание»
    assign reconfig_n = boot_req ? 1'b0 : 1'bz;
    assign pa_en      = 1'b0;

    // =================================================================================================
    // 7a. ЧТЕНИЕ SDRAM ДЛЯ ESP (SPEC 0.4): копия пачек в окно 4 КБ, окно читается тактом LINK
    // =================================================================================================
    wire        rd_busy;
    wire [31:0] rb_q;
    bz_rdwin rdwin (
        .clk(clk_mem), .rst_n(rst_mem_n),
        .go(rd_go), .addr(rd_addr_q), .len(rd_len_q), .busy(rd_busy),
        .b_req(rb_req), .b_addr(rb_addr), .b_ack(rb_ack), .b_rd_valid(rb_rd_valid), .b_rd_data(sd_rd_data),
        .rclk(link_sck), .raddr(lk_rd_addr[9:0]), .rdata(rb_q)
    );

    // =================================================================================================
    // 8. ЧТЕНИЕ РЕГИСТРОВ LINK
    // =================================================================================================
    // fb_link выставляет номер слова, которое отдаст следующим (rd_addr, домен link_sck), и берёт
    // значение как есть. Значения меняются редко; счётчик, пойманный посреди смены, ESP читает ещё раз.
    // Исключение — FIFO_FREE: он меняется постоянно, поэтому для него регистр в коде Грея (0x1C, выше).
    always @* begin
        if (lk_rd_addr[11:10] == 2'b01)
            lk_rd_data = rb_q;                                           // окно 0x1000…0x1FFC
        else case (lk_rd_addr)
            12'h000: lk_rd_data = 32'h5644_4334;                         // «VDC4»
            12'h001: lk_rd_data = {8'h51, 8'h01, 16'h0004};              // 'Q', LINK 1, версия 0.04 (ножницы со знаком)
            12'h002: lk_rd_data = {25'd0, rd_busy, msel_s[2], armed, boot_req, mem_ready, bad_cmd, fifo_ovf};
            12'h003: lk_rd_data = {20'd0, fifo_free};
            12'h007: lk_rd_data = {8'h47, 12'd0, free_gray_q};          // FREE_GRAY (0.03)
            12'h004: lk_rd_data = frames;
            12'h005: lk_rd_data = {31'd0, evo_page};
            12'h006: lk_rd_data = evo_bytes;
            12'h00F: lk_rd_data = scratch;
            12'h021: lk_rd_data = {16'h535A, 14'd0, size};
            12'h024: lk_rd_data = gpu_mark;                              // MARK (SPEC 0.4)
            12'h03B: lk_rd_data = {9'd0, sd_train_st};
            12'h03C: lk_rd_data = {12'hB00, 3'd0, boot_req, 16'd0};
            default: lk_rd_data = 32'd0;
        endcase
    end
endmodule
