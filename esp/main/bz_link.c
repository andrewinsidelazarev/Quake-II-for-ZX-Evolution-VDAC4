/*
 * =====================================================================================================
 * bz_link.c — поток команд «бомж-3D» из ESP в ядро Tang по LINK.
 * =====================================================================================================
 *
 * LINK — шина QSPI между ESP и Tang (прошивка ESP отдаёт её модулю функциями link_read, link_write,
 * link_burst). Ядро «бомж-3D» (gpu/rtl/bz_top.sv) видно на LINK так:
 *   0x04 VERSION   {личность 0x51 'Q', протокол LINK, версия ядра}
 *   0x08 STATUS    бит 0 — очередь переполнялась, 1 — неизвестная команда, 2 — SDRAM готова
 *   0x0C FIFO_FREE сколько слов ещё влезет в очередь команд ядра (очередь — 2048 слов в BSRAM)
 *   0x10 FRAMES    сколько команд FRAME ядро уже выполнило
 *   0x40000…      окно очереди: каждое записанное сюда слово — следующее слово потока команд
 *   С ядра 0.02 (SPEC 0.4, 24.11.2026) — чтение SDRAM мимо очереди (кадры md2 живут в SDRAM ядра):
 *   0x88 RD_ADDR, 0x8C RD_LEN (запись запускает копию пачек в окно), STATUS бит 6 — копия идёт,
 *   0x90 MARK (число последней выполненной команды MARK), 0x1000…0x1FFC — окно 4 КБ.
 *
 * ОТПРАВКА
 *   Писать в очередь больше, чем в ней свободно, нельзя: слова пропадут (STATUS бит 0). Поэтому:
 *   прочитать FIFO_FREE, отдать столько слов одной пачкой (link_burst пишет их транзакциями по 16
 *   слов в один адрес — адрес окна), повторить. Пока ядро занято (очередь полна), ESP ждёт.
 *   С 24.11.2026 FIFO_FREE читается дважды (берём меньшее, больше 2048 — сбой чтения), а раз в 200 мс
 *   смотрим флаги STATUS «очередь переполнялась» и «неизвестная команда» (stream_check).
 *   С ядра 0.03 место читается из FREE_GRAY (0x1C): то же число в коде Грея из регистра ядра. Прямой
 *   выход счётчика (как 0x0C у 0.02) LINK ловил посреди смены — смесь битов (на плате пришло 0x858).
 *   В коде Грея за смену меняется один бит, захват даёт старое или новое число. Старшие биты 0x47 —
 *   метка: другое значение там — сбой чтения. На ядре 0.02 (и в Unreal NG) остаётся 0x0C.
 *   На прошивке 0.1.168+ (ABI 1.21) место и STATUS читаются через link_read_checked (reg_read).
 *
 * ЯДРО — МИМО microSD-КАРТЫ TANG (правило пользователя 23.11)
 *   Ядро Q2.BIN едет с карты Evo вместе с программой Z80 (SPG) по цепочке
 *     карта Evo → Z80 → ESP32 → LINK → ПЛИС Tang (ядро TS-Config, слот 0: ящик BL616) → BL616 → JTAG → ПЛИС:
 *   Z80 отдаёт ядро ESP (MOD_QUERY с флагом 2, прошивка 0.1.164+), ESP держит его в PSRAM или на флешке
 *   USB и пишет по LINK в ящик BL616 внутри ядра TS-Config (окно 0x200, страница — регистр 0x34);
 *   BL616 на плате Tang читает ящик по своему SPI (раз в 50 мс), кладёт образ в свой flash-кэш,
 *   сверяет SHA-256 и по JTAG грузит ядро в ПЛИС. Так что Tang в пути есть — её ПЛИС с TS-Config как
 *   мост LINK ↔ BL616 и сам BL616; нет только microSD Tang: с неё ядро не читается никогда. Поэтому
 *   core_load зовётся, пока в Tang TS-Config (в начале задачи игры). Прошивка старше 0.1.164 искала бы
 *   /CORES/Q2.BIN на microSD Tang — на ней Q2.ELF не запускается (q2_app.c, start).
 *   Дальше проверяем личность ('Q') и что SDRAM ядра готова. При выгрузке приложения ядро ESP само
 *   вернёт Tang в слот 0 (TS-Config).
 *
 * РАЗМЕР КАРТИНКИ
 *   Своей настройки размера у Quake 2 нет (слово пользователя 23.11): как в слоте 0 — 100, 95 или 90 %.
 *   Пока Tang ещё в TS-Config, читаем его CTRL (LINK 0x40, биты 16:15), а загрузив ядро, пишем размер в
 *   его регистр SIZE (0x84, старшие биты 0x535A — защита от случайной записи), как NES.ELF.
 */
#include <stddef.h>
#include <string.h>
#include "q2esp.h"

#define BZ_VERSION    0x04u
#define BZ_ID         0x00u        /* «VDC4» — у всех ядер VDAC4                                 */
#define BZ_ID_VALUE   0x56444334u
#define BZ_STATUS     0x08u
#define BZ_STATUS_BITS 0x7Fu       /* у ядра 0.02 в STATUS только биты 0…6 (bz_top.sv), выше — 0  */
#define BZ_FIFO_FREE  0x0Cu
#define BZ_FIFO_BITS  0xFFFu       /* FIFO_FREE — 12 бит, выше — 0                               */
#define BZ_FIFO_DEPTH 2048u        /* очередь — 2048 слов: больше FIFO_FREE не бывает (bz_fifo.sv) */
#define BZ_FREE_GRAY  0x1Cu        /* ядро 0.03+: {0x47 'G', 12'd0, FIFO_FREE в коде Грея} (bz_top.sv) */
#define BZ_FREE_GRAY_TAG 0x47000000u
#define BZ_ST_OVF     (1u << 0)    /* STATUS: в полную очередь пришли слова и пропали (липкий)     */
#define BZ_ST_BADCMD  (1u << 1)    /* STATUS: ядро встретило неизвестное слово (липкий)            */
#define BZ_CHECK_MS   200u         /* как часто смотреть эти флаги во время игры                  */
#define BZ_SCRATCH    0x3Cu        /* ТОЛЬКО ЧИТАТЬ: здесь метка прошивки ESP (см. link_diag)     */
#define BZ_SCRATCH_TAG 0x5Cu       /* старший байт метки прошивки (tang_link.cpp:584)            */
#define BZ_FRAMES     0x10u
#define BZ_SIZE       0x84u
#define BZ_RD_ADDR    0x88u        /* SPEC 0.4: адрес SDRAM для копии в окно (кратен 32)        */
#define BZ_RD_LEN     0x8Cu        /* сколько пачек по 32 байта (1…128); запись запускает копию */
#define BZ_MARK       0x90u        /* число последней выполненной команды MARK                  */
#define BZ_WINDOW     0x1000u      /* окно чтения: 4 КБ                                         */
#define BZ_RD_BUSY    (1u << 6)    /* STATUS: копия ещё идёт                                    */
#define BZ_FIFO       0x40000u
#define TS_CTRL       0x40u        /* CTRL ядра TS-Config (слот 0): биты 16:15 — размер картинки */

static int      link_ok;
static int      can_read;          /* ядро 0.02+: чтение SDRAM по LINK (SPEC 0.4) */
static uint32_t free_reg;          /* откуда читать место в очереди: BZ_FREE_GRAY (ядро 0.03+) или BZ_FIFO_FREE */
static int      has_checked;       /* прошивка 0.1.168+ (ABI 1.21): есть link_read_checked */
/* Прошивка 0.1.176+ (ABI 1.22): link_burst_dma — очередь ядра транзакциями до 512 слов через DMA (27.11.2026;
 * плата board_log27: link_burst по 16 слов — ~1,75 слова/мкс, 8,3 мс на кадр). 1 — есть и работает; 0 — нет
 * поля или прошивка не дала буферов (ZM_NO_MEMORY) — link_burst, как раньше. */
static int      has_dma;

/* Одно слово регистра ядра. На прошивке 0.1.168+ — link_read_checked: два чтения под одним замком LINK,
 * не совпали — третье и медиана (одно испорченное из трёх отбрасывается). Старше и в Unreal NG (ABI 1.20)
 * поля нет — обычный link_read. Только для регистров состояния: окно и поток — обычным чтением. */
static int reg_read (uint32_t addr, uint32_t *v)
{
	if (has_checked)
		return q2_core->link_read_checked (addr, v, 1);
	return q2_core->link_read (addr, v, 1);
}
static uint32_t sent_words, wait_ms;
/* 26.11.2026: где время задачи LINK — в тактах (240 МГц): пачки link_burst, чтения места, ожидание места,
 * уступка чтению SDRAM игрой; и число вызовов link_burst (строка «q2: link time» в bz_link_report) */
static uint32_t lc_burst, lc_free, lc_wait, lc_rd, lc_nburst;
static uint32_t lc_ent, lc_ewait, ent_runs;   /* модели на ядре 0 (28.11.2026): такты на них (с отправкой изнутри), ждали кусок */
void BZ_EntRun (void);                       /* bz_front.c: модели кадра (задача LINK) */
extern volatile uint32_t bz_e_sp[2];         /* bz_front.c: стек этой задачи — сторона моделей (bz_int.h, bz_ectx) */
extern uint32_t bz_ent_skip[4];              /* bz_front.c: почему модели кадра не отданы задаче (отчёт) */
static inline uint32_t lc_now (void) { uint32_t c; __asm__ volatile ("rsr.ccount %0" : "=a"(c)); return c; }
static uint32_t link_free_est;      /* слов в очереди ядра наверняка свободно (0 — не знаем; link_send) */
static uint32_t read_calls, read_bytes, read_us;   /* итоги чтения SDRAM — в журнал при закрытии */
static uint32_t core_version;      /* VERSION ядра при открытии — эталон для проверки связи */
static uint32_t bad_reads;         /* сколько раз LINK отдал значение, которого ядро выдать не может */
static uint32_t check_at;          /* millis последней проверки флагов STATUS (bz_link_send)          */
static int      stream_broken;     /* флаги переполнения / неизвестной команды уже записаны в журнал  */
static uint32_t fifo_disagree;     /* сколько раз два чтения FIFO_FREE подряд дали «меньше» во втором */

/* Последние 8 отправок в очередь: что прочитали из FIFO_FREE (оба раза) и сколько слов отдали.
 * При поломке потока — в журнал: видно, не ушло ли слов больше, чем было места. */
static struct { uint16_t f1, f2, n; } last_sends[8];
static uint32_t last_pos;

/*
 * ПРОВЕРКА СВЯЗИ ПОСЛЕ СБОЯ (24.11.2026)
 *   На плате игра вышла: 100 мс STATUS читался как 0x88888ccc, а у ядра 0.02 в STATUS только биты 0…6 —
 *   такое значение ядро выдать не может. Чтение LINK без проверки целостности: ESP берёт, что пришло.
 *   Из одного числа не понять, сбоит ли сам LINK или ядро перестало отвечать. Поэтому при сбое — в журнал:
 *     ID и VERSION — числа известны заранее: верные — LINK читает ядро правильно;
 *     SCRATCH — ТОЛЬКО читаем. В нём прошивка ESP держит свою метку 0x5Cxxxxxx и раз в 2 с её проверяет:
 *       чужое число прошивка считает перезагрузкой Tang и посреди игры заново проверяет канал и
 *       запускает ядро (tang_link.cpp:580–657). Поэтому писать сюда нельзя. Старший байт 0x5C —
 *       признак, что LINK читает регистр ядра правильно; узор вроде CCCCCCCC бывает у ПЛИС,
 *       которая перезагружается;
 *     FIFO_FREE дважды через 50 мс — место в очереди растёт: ядро разбирает команды (рисует);
 *     MARK и STATUS — как есть, с пометкой, если значение невозможное;
 *     коды возврата link_read (0 — ZM_OK).
 *   Строки короткие: журнал ESP обрезает длинные.
 */
static void link_diag (const char *what)
{
	uint32_t id = 0, v = 0, sc = 0, f1 = 0, f2 = 0, mk = 0, st = 0;
	int      r[8];

	r[0] = q2_core->link_read (BZ_ID, &id, 1);
	r[1] = q2_core->link_read (BZ_VERSION, &v, 1);
	r[2] = 0;                                         /* место записи SCRATCH: больше не пишем */
	r[3] = q2_core->link_read (BZ_SCRATCH, &sc, 1);
	r[4] = q2_core->link_read (BZ_FIFO_FREE, &f1, 1);
	q2_core->delay (50);
	r[5] = q2_core->link_read (BZ_FIFO_FREE, &f2, 1);
	r[6] = q2_core->link_read (BZ_MARK, &mk, 1);
	r[7] = q2_core->link_read (BZ_STATUS, &st, 1);
	q2_core->log ("q2: LINK check (%s): ID %08x %s, VERSION %08x %s, SCRATCH %08x %s", what,
	              (unsigned)id, id == BZ_ID_VALUE ? "ok" : "BAD", (unsigned)v, v == core_version ? "ok" : "BAD",
	              (unsigned)sc, (sc >> 24) == BZ_SCRATCH_TAG ? "ok" : "BAD");
	q2_core->log ("q2: LINK check: FIFO_FREE %08x -> %08x in 50 ms, MARK %u, STATUS %08x%s", (unsigned)f1,
	              (unsigned)f2, (unsigned)mk, (unsigned)st, (st & ~BZ_STATUS_BITS) ? " impossible" : "");
	q2_core->log ("q2: LINK check: read codes %d %d %d %d %d %d %d %d; bad values so far %u",
	              r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], (unsigned)bad_reads);
}

int bz_link_open (uint32_t *version)
{
	uint32_t v = 0, st = 0, t0, ctrl = 0, size = 0, w;
	int      r;

	link_ok = 0;
	if (q2_core->link_read (TS_CTRL, &ctrl, 1) == ZM_OK)
		size = (ctrl >> 15) & 3u;
	if (size == 3)
		size = 0;
	r = q2_core->core_load ("Q2", &v);
	if (r != ZM_OK)
	{
		q2_core->log ("q2: core Q2 not loaded (%d)", r);
		return r;
	}
	if ((v >> 24) != 0x51)
	{
		q2_core->log ("q2: LINK core is not Q2 (version %08x)", (unsigned)v);
		return ZM_ERROR;
	}
	t0 = q2_core->millis ();
	while (q2_core->link_read (BZ_STATUS, &st, 1) == ZM_OK && !(st & 4))
	{
		if (q2_core->millis () - t0 > 2000)
		{
			q2_core->log ("q2: core SDRAM not ready (status %08x)", (unsigned)st);
			return ZM_ERROR;
		}
		q2_core->delay (10);
	}
	w = 0x535A0000u | size;
	q2_core->link_write (BZ_SIZE, &w, 1);
	*version = v;
	core_version = v;
	bad_reads = 0;
	stream_broken = 0;
	fifo_disagree = 0;
	check_at = q2_core->millis ();
	last_pos = 0;
	memset (last_sends, 0, sizeof(last_sends));
	link_free_est = 0;
	link_ok = 1;
	/* ядро 0.02+ умеет отдавать SDRAM в окно (SPEC 0.4): тогда кадры md2 живут там; старое ядро — в PSRAM */
	can_read = (v & 0xFFFFu) >= 2;
	free_reg = (v & 0xFFFFu) >= 3 ? BZ_FREE_GRAY : BZ_FIFO_FREE;
	has_checked = q2_core->abi_minor >= 21 &&
	              q2_core->size >= offsetof (zm_core_t, link_read_checked) + sizeof(q2_core->link_read_checked) &&
	              q2_core->link_read_checked != NULL;
	has_dma = q2_core->abi_minor >= 22 &&
	          q2_core->size >= offsetof (zm_core_t, link_burst_wait) + sizeof(q2_core->link_burst_wait) &&
	          q2_core->link_burst_dma != NULL && q2_core->link_burst_wait != NULL;
	q2_core->log ("q2: LINK queue space from %s, %s reads, %s", free_reg == BZ_FREE_GRAY ? "FREE_GRAY (Gray code)"
	              : "FIFO_FREE (binary)", has_checked ? "checked" : "plain", has_dma ? "DMA bursts" : "16-word bursts");
	sent_words = wait_ms = 0;
	read_calls = read_bytes = read_us = 0;
	link_diag ("core open");        /* образец: так проверка выглядит при исправной связи */
	return ZM_OK;
}

int bz_link_can_read (void)
{
	return link_ok && can_read;
}

/* Дождаться, что ядро выполнило команду MARK с номером mark (или более позднюю): всё, что шло в потоке
 * раньше, уже в SDRAM. Обычно — сразу (загрузка была давно). Номера растут; сравнение — по разности. */
int bz_link_wait_mark (uint32_t mark)
{
	uint32_t got = 0, t0 = q2_core->millis ();

	for (;;)
	{
		if (!link_ok || q2_core->link_read (BZ_MARK, &got, 1) != ZM_OK)
			return ZM_ERROR;
		if ((int32_t)(got - mark) >= 0)
			return ZM_OK;
		if (q2_core->millis () - t0 > 3000)
		{
			q2_core->log ("q2: core MARK %u, waiting for %u", (unsigned)got, (unsigned)mark);
			link_diag ("MARK wait");
			return ZM_ERROR;
		}
		q2_core->delay (1);
	}
}

/* Ядро уже выполнило MARK с номером mark (одно чтение, без ожидания; модели на ядре 0 — bz_front.c). */
int bz_link_mark_reached (uint32_t mark)
{
	uint32_t got = 0;

	return link_ok && q2_core->link_read (BZ_MARK, &got, 1) == ZM_OK && (int32_t)(got - mark) >= 0;
}

/*
 * Прочитать bytes байт SDRAM ядра с адреса addr (кратен 4) — SPEC 0.4:
 *   RD_ADDR ← адрес ближней пачки ниже (кратен 32), RD_LEN ← пачек по 32 байта (до 128 — окно 4 КБ): ядро
 *   копирует SDRAM в окно мимо очереди команд (рисование не ждёт); STATUS бит 6 (RD_BUSY) = 0 — копия
 *   готова; нужные слова окна (со сдвигом addr − пачка) — по 16 за чтение LINK (больше прошивка за раз не
 *   берёт). Слово SDRAM: младший байт — младший, как у ESP. Больше окна — частями.
 */
static int link_read_sdram (uint32_t addr, void *dst, uint32_t bytes)
{
	long long Q2M_Now (void);           /* микросекунды (sys_esp.c) */
	uint8_t  *d = (uint8_t *)dst;
	long long t0 = Q2M_Now ();

	if (!bz_link_can_read () || (addr & 3u))
		return ZM_ERROR;
	while (bytes)
	{
		uint32_t a0 = addr & ~31u, skip = addr - a0;          /* окно — с пачки, данные — со сдвигом */
		uint32_t chunk = bytes < 4096u - skip ? bytes : 4096u - skip;
		uint32_t bursts = (skip + chunk + 31u) / 32u, st = 0, words = (chunk + 3u) / 4u, i, n, tw;
		uint32_t buf[16];
		uint32_t seen[4], nseen = 0, polls = 0, more = 0, k;   /* разные значения STATUS за ожидание */

		if (q2_core->link_write (BZ_RD_ADDR, &a0, 1) != ZM_OK
		    || q2_core->link_write (BZ_RD_LEN, &bursts, 1) != ZM_OK)
			return ZM_ERROR;
		tw = q2_core->millis ();
		do
		{
			if (q2_core->link_read (BZ_STATUS, &st, 1) != ZM_OK)
				return ZM_ERROR;
			polls++;
			if (st & ~BZ_STATUS_BITS)
				bad_reads++;                          /* ядро такого выдать не может */
			for (k = 0; k < nseen && seen[k] != st; k++)
				;
			if (k == nseen)
			{
				if (nseen < 4)
					seen[nseen++] = st;
				else
					more = 1;
			}
			if ((st & BZ_RD_BUSY) && q2_core->millis () - tw > 100)
			{
				q2_core->log ("q2: core SDRAM read stuck: %u STATUS reads in 100 ms, values %08x %08x %08x %08x%s",
				              (unsigned)polls, (unsigned)seen[0], (unsigned)(nseen > 1 ? seen[1] : 0),
				              (unsigned)(nseen > 2 ? seen[2] : 0), (unsigned)(nseen > 3 ? seen[3] : 0), more ? " +" : "");
				q2_core->log ("q2: core SDRAM read: addr %08x, %u bursts", (unsigned)a0, (unsigned)bursts);
				link_diag ("SDRAM read");
				return ZM_ERROR;
			}
		} while (st & BZ_RD_BUSY);
		for (i = 0; i < words; i += n)
		{
			uint32_t k, take;

			n = words - i < 16u ? words - i : 16u;
			if (q2_core->link_read (BZ_WINDOW + skip + 4u * i, buf, n) != ZM_OK)
				return ZM_ERROR;
			take = chunk - 4u * i < 4u * n ? chunk - 4u * i : 4u * n;
			for (k = 0; k < take; k++)
				d[4u * i + k] = (uint8_t)(buf[k >> 2] >> (8u * (k & 3u)));
		}
		addr += chunk;
		d += chunk;
		bytes -= chunk;
		read_bytes += chunk;
	}
	read_calls++;
	read_us += (uint32_t)((Q2M_Now () - t0) / 240);   /* такты → мкс (sys_esp.c) */
	return ZM_OK;
}

/*
 * ПРОВЕРКА ПОТОКА (24.11.2026)
 *   На плате STATUS ядра стал 0x37: очередь переполнилась (бит 0) и ядро встретило неизвестное слово
 *   (бит 1). Слова пропали — дальше ядро читает поток со сдвигом: данные принимает за команды и пишет
 *   мусор в SDRAM (текстуры стали цветным шумом). Оба флага липкие — держатся до перезапуска ядра.
 *   Раньше Q2 их во время игры не смотрела. Теперь — раз в BZ_CHECK_MS (одно чтение LINK, ~2 мкс) и при
 *   первом срабатывании пишет в журнал: STATUS, сколько кадров выполнило ядро (FRAMES), сколько слов ушло,
 *   последние 8 отправок (оба чтения FIFO_FREE и сколько слов отдали) и проверку связи link_diag.
 *   Невозможный STATUS (биты выше 6) — не поломка потока, а сбой чтения: его считаем в bad_reads.
 */
static void stream_check (void)
{
	uint32_t st = 0, frames = 0, k, i;

	if (stream_broken || q2_core->millis () - check_at < BZ_CHECK_MS)
		return;
	check_at = q2_core->millis ();
	if (reg_read (BZ_STATUS, &st) != ZM_OK)
		return;
	if (st & ~BZ_STATUS_BITS)
	{
		bad_reads++;
		return;
	}
	if (!(st & (BZ_ST_OVF | BZ_ST_BADCMD)))
		return;
	stream_broken = 1;
	q2_core->link_read (BZ_FRAMES, &frames, 1);
	q2_core->log ("q2: LINK: command stream broken: STATUS %08x (%s%s), core frames %u, sent %u words",
	              (unsigned)st, (st & BZ_ST_OVF) ? "queue overflow " : "", (st & BZ_ST_BADCMD) ? "bad command" : "",
	              (unsigned)frames, (unsigned)sent_words);
	for (k = 0; k < 8; k += 4)
	{
		uint32_t v[12];

		for (i = 0; i < 4; i++)
		{
			uint32_t p = (last_pos + k + i) & 7u;     /* от старых к новым */
			v[3 * i] = last_sends[p].f1;
			v[3 * i + 1] = last_sends[p].f2;
			v[3 * i + 2] = last_sends[p].n;
		}
		q2_core->log ("q2: LINK: last sends (FIFO_FREE twice -> words): %u/%u->%u %u/%u->%u %u/%u->%u %u/%u->%u",
		              (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3], (unsigned)v[4], (unsigned)v[5],
		              (unsigned)v[6], (unsigned)v[7], (unsigned)v[8], (unsigned)v[9], (unsigned)v[10], (unsigned)v[11]);
	}
	q2_core->log ("q2: LINK: FIFO_FREE second read lower %u times, bad values %u", (unsigned)fifo_disagree,
	              (unsigned)bad_reads);
	link_diag ("stream");
}

/* FIFO_FREE годится, только если ядро могло его выдать: не больше глубины очереди (2048). Иначе —
 * сбой чтения LINK: первые три — в журнал, место считаем нулём (подождать и прочитать снова). */
static uint32_t fifo_free_checked (uint32_t v)
{
	if (v <= BZ_FIFO_DEPTH)
		return v;
	if (++bad_reads <= 3)
		q2_core->log ("q2: LINK: FIFO_FREE %08x impossible (bad values %u)", (unsigned)v, (unsigned)bad_reads);
	return 0;
}

/* Место в очереди одним чтением LINK. Ядро 0.03+: FREE_GRAY — проверить метку 0x47 и нули в битах
 * 23:12, перевести код Грея в двоичное (каждый бит — XOR всех старших). Ядро 0.02: FIFO_FREE как есть. */
static int read_free (uint32_t *out)
{
	uint32_t v, b;

	if (reg_read (free_reg, &v) != ZM_OK)
		return ZM_ERROR;
	if (free_reg == BZ_FREE_GRAY)
	{
		if ((v & ~0xFFFu) != BZ_FREE_GRAY_TAG)
		{
			if (++bad_reads <= 3)
				q2_core->log ("q2: LINK: FREE_GRAY %08x impossible (bad values %u)", (unsigned)v,
				              (unsigned)bad_reads);
			*out = 0;
			return ZM_OK;
		}
		b = v & 0xFFFu;
		b ^= b >> 1;
		b ^= b >> 2;
		b ^= b >> 4;
		b ^= b >> 8;
		v = b;
	}
	*out = fifo_free_checked (v);
	return ZM_OK;
}

/*
 * 26.11.2026 (плата board_log18, board_log19). Пачка в очередь ядра не начинается, пока игра читает SDRAM
 * ядра (rd_active, bz_link_read): чтение — десятки мелких обращений к LINK (адрес, опрос STATUS, окно по 16
 * слов), и каждое ждало конца пачки до 8 КБ (~1,5 мс): 2,3 чтения за кадр — 3,4 мс (с этим — 2,0).
 * Место в очереди — своей оценкой (link_free_est): прочитанное FIFO_FREE минус отправленное — нижняя граница
 * (ядро слова только забирает). FIFO_FREE (два чтения с проверкой — 4–6 обращений к LINK) читается, лишь когда
 * оценки не хватает: пачки по 512 слов с чтением перед каждой (board_log19) сделали LINK медленнее, 25 → 27 мс.
 * Очередь полна — ждать, пока освободится хотя бы LINK_MIN_FREE слов (или весь остаток), опросом раз в
 * LINK_POLL_US до LINK_SPIN_US, потом сон на 1 мс (сон FreeRTOS не короче 1 мс, а 8 КБ очереди ядро может
 * разобрать быстрее и стоять без команд: board_log18 — «очередь полна» 11–14 мс за кадр).
 */
#define LINK_MIN_FREE  512u      /* ждать хотя бы столько свободных слов, а не слать пачки по чуть-чуть */
#define LINK_POLL_US   150u      /* шаг короткого ожидания места в очереди ядра                         */
#define LINK_SPIN_US   1500u     /* сколько ждать коротко, прежде чем уснуть на 1 мс                    */
#define LINK_RD_POLL_US 20u      /* шаг ожидания конца чтения SDRAM игрой                                */

static volatile int rd_active;   /* 1 — игра читает SDRAM ядра (bz_link_read): задача LINK пачку не начинает */

/* Подождать us микросекунд счётчиком тактов (240 МГц) — без сна: ядро ESP 0, где работает задача LINK,
 * почти простаивает, а сон FreeRTOS — не короче 1 мс. */
static void spin_us (uint32_t us)
{
	uint32_t c0, c;

	__asm__ volatile ("rsr.ccount %0" : "=a"(c0));
	do
		__asm__ volatile ("rsr.ccount %0" : "=a"(c));
	while (c - c0 < us * 240u);
}

/* Модели кадра у задачи LINK (28.11.2026, ниже — «МОДЕЛИ НА ЯДРЕ 0»): post — отдала игра, run — начала задача,
 * done — досчитала; join_seen — сколько меток моделей задача прошла (их куски ушли). Модели кадра начинаются, когда
 * ушли куски прошлого (иначе новые куски встали бы на место прошлых). Задача, которая ждёт места в очереди ядра,
 * бросает ожидание, когда можно начать модели. */
static volatile uint32_t ent_seq_post, ent_seq_run, ent_seq_done, join_seen;

static inline int ent_startable (void)
{
	return ent_seq_run != ent_seq_post && join_seen == ent_seq_run;
}

/*
 * Отправить слова *pw, *pn (сдвигаются на ушедшее), сколько позволяет очередь ядра. wait: 1 — ждать места, пока не
 * уйдёт всё; 0 — не ждать (вернуть 1, когда очередь полна: модели на ядре 0 шлют между своими делами — bz_link_tick);
 * 2 — ждать, но вернуть 1, как только игра отдала модели (задача займётся ими, а остаток дошлёт изнутри них).
 * ZM_OK — ушло всё, ZM_ERROR — LINK.
 */
static int link_chunk (const uint32_t **pw, uint32_t *pn, int wait)
{
	const uint32_t *words = *pw;
	uint32_t        count = *pn, spun = 0;
	int             r = ZM_OK;

	stream_check ();
	while (count)
	{
		uint32_t f1 = 0, f2 = 0, n, t0, c0;

		/* игра читает SDRAM ядра — дать ей LINK (до 4 мс; чтение — доли миллисекунды) */
		c0 = lc_now ();
		for (t0 = 0; rd_active && t0 < 4000u / LINK_RD_POLL_US; t0++)
			spin_us (LINK_RD_POLL_US);
		lc_rd += lc_now () - c0;

		if (!link_free_est || (link_free_est < LINK_MIN_FREE && link_free_est < count))
		{
			/* Место в очереди читаем дважды и берём меньшее. Между чтениями ядро только забирает слова, так
			 * что второе число не меньше первого; если меньше — одно из чтений испорчено (LINK без проверки
			 * целостности). Испорченное «больше» без второго чтения переполнило бы очередь.
			 * 26.11.2026 (плата board_log27: чтения места — 7–7,7 мс задачи LINK за кадр, ~45 мкс на пару): с
			 * link_read_checked прошивка сама читает регистр дважды и при несовпадении берёт медиану трёх — второе
			 * наше чтение лишнее, оно только удваивало обращения к LINK. */
			int bad;
			c0 = lc_now ();
			bad = !link_ok || read_free (&f1) != ZM_OK;
			if (!bad)
			{
				if (has_checked)
					f2 = f1;
				else
					bad = read_free (&f2) != ZM_OK;
			}
			lc_free += lc_now () - c0;
			if (bad)
			{
				link_ok = 0;
				r = ZM_ERROR;
				break;
			}
			if (f2 < f1)
				fifo_disagree++;
			link_free_est = f1 < f2 ? f1 : f2;
			if (link_free_est < LINK_MIN_FREE && link_free_est < count)
			{
				/* мало места: ядро ещё рисует — подождать (см. выше) или (модели, новые модели) вернуться */
				if (!wait || (wait == 2 && ent_startable ()))
				{
					r = 1;
					break;
				}
				t0 = q2_core->millis ();
				c0 = lc_now ();
				if (spun < LINK_SPIN_US)
				{
					spin_us (LINK_POLL_US);
					spun += LINK_POLL_US;
				}
				else
				{
					q2_core->delay (1);
					spun = 0;
				}
				wait_ms += q2_core->millis () - t0;
				lc_wait += lc_now () - c0;
				continue;
			}
		}
		spun = 0;
		n = count < link_free_est ? count : link_free_est;
		last_sends[last_pos].f1 = (uint16_t)f1;
		last_sends[last_pos].f2 = (uint16_t)f2;
		last_sends[last_pos].n = (uint16_t)n;
		last_pos = (last_pos + 1) & 7u;
		c0 = lc_now ();
		if (has_dma)
		{
			/* без ожидания хвоста: следующий обмен по LINK (чтение места, чтение SDRAM игрой) дождётся его сам.
			 * Окно 0x40000 + 2 КБ транзакции — внутри окна очереди 0x40000…0x7FFFC. */
			int rr = q2_core->link_burst_dma (BZ_FIFO, words, n, ZM_LINK_NOWAIT);

			if (rr == ZM_NO_MEMORY)
			{
				has_dma = 0;                    /* ничего не послано — дальше link_burst */
				q2_core->log ("q2: LINK DMA: no buffers (internal RAM) — 16-word bursts");
				continue;
			}
			if (rr != ZM_OK)
			{
				link_ok = 0;
				r = ZM_ERROR;
				break;
			}
		}
		else if (q2_core->link_burst (BZ_FIFO, words, n) != ZM_OK)
		{
			link_ok = 0;
			r = ZM_ERROR;
			break;
		}
		lc_burst += lc_now () - c0;
		lc_nburst++;
		words += n;
		count -= n;
		sent_words += n;
		link_free_est -= n;
	}
	*pw = words;
	*pn = count;
	return r;
}

static int link_send (const uint32_t *words, uint32_t count)
{
	return link_chunk (&words, &count, 1);
}

/* Раз в 5 с (q2_app.c, 26.11.2026): слов в ядро за кадр и сколько задача LINK ждала места в очереди ядра
 * (ядро ещё рисует) — часть «link» в строке «rf parts». Большое ожидание — кадр упирается в само ядро. */
void bz_link_report (int frames)
{
	static uint32_t words0, wait0, reads0, rbytes0;
	uint32_t        words = sent_words, wait = wait_ms, reads = read_calls, rbytes = read_bytes;

	/* 26.11.2026: и чтения SDRAM ядра (кадры md2, звук) — сколько и сколько байт за кадр */
	if (frames > 0)
		q2_core->log ("q2: link, per frame %u words, core queue full %u.%u ms; SDRAM reads %u.%u, %u B",
		              (unsigned)((words - words0) / frames),
		              (unsigned)((wait - wait0) / frames), (unsigned)((wait - wait0) * 10u / frames % 10u),
		              (unsigned)((reads - reads0) / frames), (unsigned)((reads - reads0) * 10u / frames % 10u),
		              (unsigned)((rbytes - rbytes0) / frames));
	/* 26.11.2026: время задачи LINK по частям (мс за кадр) и скорость самой передачи (слов в мс внутри link_burst) */
	if (frames > 0)
	{
		const uint32_t k = 240u * (uint32_t)frames;          /* тактов в мкс × кадров */
		q2_core->log ("q2: link time, ms/frame: burst %u.%u (%u calls, %u words/ms), free %u.%u, wait %u.%u, rd %u.%u",
		              (unsigned)(lc_burst / k / 1000u), (unsigned)(lc_burst / k % 1000u / 100u),
		              (unsigned)(lc_nburst / (uint32_t)frames),
		              (unsigned)(lc_burst / 240000u ? (words - words0) / (lc_burst / 240000u) : 0),
		              (unsigned)(lc_free / k / 1000u), (unsigned)(lc_free / k % 1000u / 100u),
		              (unsigned)(lc_wait / k / 1000u), (unsigned)(lc_wait / k % 1000u / 100u),
		              (unsigned)(lc_rd / k / 1000u), (unsigned)(lc_rd / k % 1000u / 100u));
	}
	/* 28.11.2026: модели на ядре 0 — сколько кадров и время задачи на них (вместе с отправкой изнутри них), из него —
	 * ожидание свободного куска */
	if (frames > 0)
	{
		static uint32_t runs0;
		const uint32_t  k = 240u * (uint32_t)frames;
		static uint32_t sk0[4];
		q2_core->log ("q2: models on core 0: %u of %u frames, ms/frame %u.%u, waiting for a chunk %u.%u; not given: "
		              "no models %u, no task %u, no verts %u, md2 %u",
		              (unsigned)(ent_runs - runs0), (unsigned)frames,
		              (unsigned)(lc_ent / k / 1000u), (unsigned)(lc_ent / k % 1000u / 100u),
		              (unsigned)(lc_ewait / k / 1000u), (unsigned)(lc_ewait / k % 1000u / 100u),
		              (unsigned)(bz_ent_skip[0] - sk0[0]), (unsigned)(bz_ent_skip[1] - sk0[1]),
		              (unsigned)(bz_ent_skip[2] - sk0[2]), (unsigned)(bz_ent_skip[3] - sk0[3]));
		memcpy (sk0, bz_ent_skip, sizeof(sk0));
		runs0 = ent_runs;
	}
	lc_ent = lc_ewait = 0;
	lc_burst = lc_free = lc_wait = lc_rd = lc_nburst = 0;
	words0 = words;
	wait0 = wait;
	reads0 = reads;
	rbytes0 = rbytes;
}

/* Чтение SDRAM с замером (Q2M_LREAD) */
int bz_link_read (uint32_t addr, void *dst, uint32_t bytes)
{
	int r;

	Q2M_BEGIN (Q2M_LREAD);
	rd_active = 1;                      /* задача LINK новую пачку не начнёт (link_send) */
	r = link_read_sdram (addr, dst, bytes);
	rd_active = 0;
	Q2M_END (Q2M_LREAD);
	return r;
}

/* Отправка с замером: время LINK за кадр — в журнал раз в 5 с (Q2M_LINK, q2_app.c), 25.11.2026 */
int bz_link_send (const uint32_t *words, uint32_t count)
{
	int r;

	Q2M_BEGIN (Q2M_LINK);
	r = link_send (words, count);
	Q2M_END (Q2M_LINK);
	return r;
}

/*
 * ЗАДАЧА LINK (25.11.2026). Половину буфера кадра (bz_front.c, cb_main_ship) шлёт в ядро своя задача на ядре
 * ESP 0 — оно почти простаивает (IDLE0 93–96 %), а отправка — ~19 мс на кадр (плата, board_log11). Задача
 * берёт одну половину за раз; новую bz_link_send_async отдаёт, только когда прошлая ушла, — порядок слов
 * потока тот же. Вызовы LINK прошивка выполняет под своей блокировкой (tang_link.cpp takeLock), поэтому
 * чтения игры (кадры md2, звуки, MARK) идут между пачками задачи. Время ожидания игры — Q2M_LWAIT.
 */
/*
 * 26.11.2026: очередь, а не одна половина. Плата (board_log17): игра ждала задачу LINK 4–7 мс за кадр — мир
 * даёт команды быстрее, чем LINK их уносит, а модели и частицы — медленнее. Теперь в отправке может быть до
 * tx_depth буферов (bz_link_set_depth; у bz_front.c — по числу запасных буферов кадра), и игра ждёт, только
 * когда заняты все. Порядок слов тот же: задача шлёт буферы по очереди. Счётчики tq_put (пишет игра) и
 * tq_done (пишет задача) — каждый своей стороной; в отправке — tq_put − tq_done.
 */
/*
 * МОДЕЛИ НА ЯДРЕ 0 (28.11.2026; bz_front.c — там же, зачем и что у моделей своё). Пока очередь ядра полна (12–19 мс
 * кадра, board_log41–42), задача не крутится вхолостую, а считает непрозрачные модели кадра (BZ_EntRun): игра
 * отдаёт их перед миром (bz_link_ent_post) и обходит мир сама. Модели пишут в свои куски (eq_*, ENT_CHUNKS по
 * ENT_WORDS — память bz_front.c) и время от времени (bz_link_tick, не чаще LINK_TICK_US) шлют, что можно, не ожидая
 * места: буферы мира — как раньше, по очереди. Куски моделей идут в поток только на своём месте: дойдя до моделей
 * (после мира и неба), игра ставит в очередь метку (tq_w = NULL, bz_link_ent_join) — задача, дойдя до неё, шлёт
 * куски по порядку (и дописанные потом), а дальше — буферы игры за меткой. Порядок слов — тот же, что у одного
 * потока, где модели шли бы после неба. Нет свободного куска — модели ждут (шлют, что есть; до метки — игру).
 */
#define TQ_MAX 4
#define EQ_MAX 8
#define LINK_TICK_US 200u        /* модели отдают время отправке не чаще этого (чтение места — ~22 мкс) */
static void          *tx_work, *tx_done, *ent_sig;
static const uint32_t *tq_w[TQ_MAX];     /* NULL — метка моделей */
static uint32_t       tq_n[TQ_MAX];
static volatile uint32_t tq_put, tq_done;
static volatile uint32_t tq_jput;        /* меток моделей поставлено (игра); пройдено — join_seen (задача) */
static int            tx_depth = 1;
static volatile int   tx_quit, tx_alive, tx_err;
static int            tx_state;          /* 0 — не заводилась, 1 — работает, -1 — не вышло (шлём сами) */
static int            tx_big;            /* стек задачи — под модели (иначе моделей ей не давать) */
/* куски моделей (bz_link_ent_pool) и их очередь: оба счётчика ведёт задача */
static uint32_t      *eq_pool, eq_words, eq_chunks;
static uint32_t       eq_n[EQ_MAX];
static uint32_t       eq_put, eq_get;
static int            in_join;           /* задача дошла до метки — шлёт куски моделей */
/* что шлётся сейчас: 1 — буфер игры (tq_done), 2 — кусок моделей (eq_get) */
static const uint32_t *cur_w;
static uint32_t       cur_n;
static int            cur_kind;
static uint32_t       tick_at;

/* Сколько буферов может быть в отправке разом (1…TQ_MAX): столько буфер кадра держит в запасе. */
void bz_link_set_depth (int depth)
{
	tx_depth = depth < 1 ? 1 : depth > TQ_MAX ? TQ_MAX : depth;
}

/* Один шаг отправки: следующее по порядку (буфер игры, метка, куски моделей) и сколько его уйдёт (wait —
 * как у link_chunk). 0 — слать нечего, 1 — что-то ушло или кончилось, 2 — очередь ядра полна (не ждали). */
static int sink_step (int wait)
{
	int r;

	if (!cur_kind)
	{
		if (in_join)
		{
			if (eq_get != eq_put)
			{
				uint32_t i = eq_get % eq_chunks;

				cur_w = eq_pool + i * eq_words;
				cur_n = eq_n[i];
				cur_kind = 2;
			}
			else if (ent_seq_run == join_seen + 1 && ent_seq_done == ent_seq_run)
			{
				/* модели этой метки досчитаны и ушли — метка пройдена; сначала её счёт, потом общий (tx_wait_below) */
				in_join = 0;
				join_seen++;
				__sync_synchronize ();
				tq_done++;
				q2_core->signal_give (tx_done);
				return 1;
			}
			else
				return 0;           /* модели ещё считаются (или не начаты) — их ведёт эта же задача */
		}
		else if (tq_done != tq_put)
		{
			uint32_t i = tq_done % TQ_MAX;

			if (!tq_w[i])
			{
				in_join = 1;
				return 1;
			}
			cur_w = tq_w[i];
			cur_n = tq_n[i];
			cur_kind = 1;
		}
		else
			return 0;
	}
	Q2M_BEGIN (Q2M_LINK);
	r = link_chunk (&cur_w, &cur_n, wait);
	Q2M_END (Q2M_LINK);
	if (r == ZM_ERROR)
	{
		tx_err = 1;
		cur_n = 0;
	}
	if (!cur_n)
	{
		if (cur_kind == 1)
		{
			__sync_synchronize ();
			tq_done++;
			q2_core->signal_give (tx_done);
		}
		else
			eq_get++;
		cur_kind = 0;
		return 1;
	}
	return r == 1 ? 2 : 1;
}

static void tx_task (void *arg)
{
	/* стек задачи (arg ≠ NULL — 16 КБ) — сторона моделей (bz_ectx, bz_int.h): от вершины (рядом с этим кадром) на 16 КБ
	 * вниз. Признак — аргументом, а не tx_big: задача может начаться раньше, чем task_start вернётся. */
	if (arg)
	{
		volatile uint32_t here = 0;

		bz_e_sp[0] = (uint32_t)(uintptr_t)&here - 16384u;
		bz_e_sp[1] = 16384u + 256u;
	}
	(void)arg;
	while (!tx_quit)
	{
		if (ent_startable ())
		{
			uint32_t c0 = lc_now ();

			ent_seq_run = ent_seq_post;
			__sync_synchronize ();
			BZ_EntRun ();               /* bz_front.c: модели кадра; внутри — bz_link_tick, bz_link_ent_chunk */
			lc_ent += lc_now () - c0;
			ent_runs++;
			continue;
		}
		if (!sink_step (2))
			q2_core->signal_wait (tx_work, 100);
	}
	bz_e_sp[1] = 0;
	tx_alive = 0;
}

/* Модели (внутри BZ_EntRun, задача LINK): пора — отправить, что можно, не ожидая места в очереди ядра. */
void bz_link_tick (void)
{
	uint32_t c = lc_now (), k;

	if (c - tick_at < LINK_TICK_US * 240u)
		return;
	tick_at = c;
	for (k = 0; k < 4 && sink_step (0) == 1; k++)
		;
}

/* Модели: кусок full (n слов; NULL — первый) полон — в очередь кусков; дать следующий свободный. Свободного нет
 * (все ждут метки или уходят) — слать, что есть, а до метки — ждать игру. */
uint32_t *bz_link_ent_chunk (uint32_t *full, uint32_t n)
{
	if (full)
	{
		eq_n[eq_put % eq_chunks] = n;
		eq_put++;
	}
	if (eq_put - eq_get >= eq_chunks)
	{
		uint32_t c0 = lc_now ();

		while (eq_put - eq_get >= eq_chunks)
			if (!sink_step (1))
				q2_core->signal_wait (tx_work, 1);
		lc_ewait += lc_now () - c0;
	}
	return eq_pool + (eq_put % eq_chunks) * eq_words;
}

/* Модели кадра кончились: последний кусок — в очередь, игре — «готово» (bz_link_ent_join). */
void bz_link_ent_end (uint32_t *last, uint32_t n)
{
	if (last && n)
	{
		eq_n[eq_put % eq_chunks] = n;
		eq_put++;
	}
	__sync_synchronize ();
	ent_seq_done = ent_seq_run;
	q2_core->signal_give (ent_sig);
}

/* Куски для моделей (bz_front.c, до первой карты; NULL — их нет, модели рисует игра). Не во время моделей. */
void bz_link_ent_pool (uint32_t *pool, int chunks, uint32_t words)
{
	eq_pool = pool;
	eq_chunks = pool ? (uint32_t)(chunks > EQ_MAX ? EQ_MAX : chunks) : 0;
	eq_words = words;
	eq_put = eq_get = 0;
}

/* Задаче LINK можно отдавать модели: она работает, у неё стек под них, есть куски, LINK цел. */
int bz_link_ent_ok (void)
{
	return tx_state > 0 && tx_big && eq_pool && eq_chunks && link_ok && !tx_err;
}

/* Игра: модели кадра — задаче (bz_front.c уже положил всё, что им нужно). */
void bz_link_ent_post (void)
{
	__sync_synchronize ();
	ent_seq_post++;
	q2_core->signal_give (tx_work);
}

/* Игра, на месте моделей: метка в очередь; ждать, пока задача досчитает модели (их куски уйдут за меткой). */
void bz_link_ent_join (void)
{
	while (tq_put - tq_done >= TQ_MAX)
		q2_core->signal_wait (tx_done, 20);
	tq_jput++;
	tq_w[tq_put % TQ_MAX] = NULL;
	tq_n[tq_put % TQ_MAX] = 0;
	__sync_synchronize ();
	tq_put++;
	q2_core->signal_give (tx_work);
	while (ent_seq_done != ent_seq_post)
		q2_core->signal_wait (ent_sig, 20);
}

/* Дождаться, что в отправке не больше limit буферов (метка моделей — не буфер). */
static void tx_wait_below (uint32_t limit)
{
	if (tq_put - tq_done - (tq_jput - join_seen) > limit)
	{
		Q2M_BEGIN (Q2M_LWAIT);
		while (tq_put - tq_done - (tq_jput - join_seen) > limit)
			q2_core->signal_wait (tx_done, 20);
		Q2M_END (Q2M_LWAIT);
	}
}

/* Дождаться, что задача LINK отправила всё отданное (и куски моделей за меткой). ZM_ERROR — отправка не удалась. */
int bz_link_wait_idle (void)
{
	if (tq_put != tq_done)
	{
		Q2M_BEGIN (Q2M_LWAIT);
		while (tq_put != tq_done)
			q2_core->signal_wait (tx_done, 20);
		Q2M_END (Q2M_LWAIT);
	}
	return tx_err ? ZM_ERROR : ZM_OK;
}

/* Отдать count слов задаче LINK (буфер не трогать, пока не отданы ещё tx_depth следующих). Задачи нет — шлём
 * сами. */
int bz_link_send_async (const uint32_t *words, uint32_t count)
{
	if (tx_state == 0)
	{
		tx_state = -1;
		tx_work = q2_core->signal_create ();
		tx_done = q2_core->signal_create ();
		ent_sig = q2_core->signal_create ();
		tx_quit = tx_err = 0;
		tq_put = tq_done = 0;
		tq_jput = join_seen = 0;
		in_join = cur_kind = 0;
		cur_n = 0;
		eq_put = eq_get = 0;
		ent_seq_post = ent_seq_run = ent_seq_done = 0;
		tx_alive = 1;
		/* 28.11.2026: стек — под модели (BZ_EntRun: код id, свет по дереву карты; у id ещё и вершины на стеке —
		 * у нас они в куче), в PSRAM: внутреннего ОЗУ на 16 КБ нет (свободно ~22 КБ). Не вышло — прежние 4 КБ,
		 * модели тогда рисует игра. */
		tx_big = 0;
		if (tx_work && tx_done && ent_sig)
		{
			if (q2_core->task_start (tx_task, (void *)1, 16384u | ZM_TASK_STACK_PSRAM, 3, 0) == ZM_OK)
				tx_state = tx_big = 1;
			else if (q2_core->task_start (tx_task, NULL, 4096, 3, 0) == ZM_OK ||
			         q2_core->task_start (tx_task, NULL, 4096 | ZM_TASK_STACK_PSRAM, 3, 0) == ZM_OK)
				tx_state = 1;
		}
		if (tx_state < 0)
			tx_alive = 0;
		q2_core->log ("q2: LINK sender task on core 0: %s%s", tx_state > 0 ? "running" : "failed, sending inline",
		              tx_state > 0 ? (tx_big ? ", models on core 0" : ", small stack — models on core 1") : "");
	}
	if (tx_state < 0)
		return bz_link_send (words, count);
	tx_wait_below ((uint32_t)tx_depth - 1u);
	if (tx_err)
		return ZM_ERROR;
	tq_w[tq_put % TQ_MAX] = words;
	tq_n[tq_put % TQ_MAX] = count;
	__sync_synchronize ();
	tq_put++;
	q2_core->signal_give (tx_work);
	return ZM_OK;
}

void bz_link_close (void)
{
	if (tx_state > 0)
	{
		uint32_t t0;

		bz_link_wait_idle ();
		tx_quit = 1;
		q2_core->signal_give (tx_work);
		for (t0 = q2_core->millis (); tx_alive && q2_core->millis () - t0 < 500; )
			q2_core->delay (5);
		tx_state = 0;
		if (ent_runs)
			q2_core->log ("q2: models on core 0: %u frames", (unsigned)ent_runs);
	}
	if (has_dma && link_ok && q2_core->link_burst_wait () != ZM_OK)
		q2_core->log ("q2: LINK DMA: last burst did not finish");
	if (sent_words)
		q2_core->log ("q2: LINK %u words, waited %u ms for queue", (unsigned)sent_words, (unsigned)wait_ms);
	if (read_calls)
		q2_core->log ("q2: SDRAM reads %u, %u KB, %u ms", (unsigned)read_calls, (unsigned)(read_bytes >> 10),
		              (unsigned)(read_us / 1000));
	if (bad_reads)
		q2_core->log ("q2: LINK: %u values the core cannot give (STATUS, FIFO_FREE)", (unsigned)bad_reads);
	if (fifo_disagree)
		q2_core->log ("q2: LINK: FIFO_FREE second read lower %u times", (unsigned)fifo_disagree);
	link_ok = 0;
}
