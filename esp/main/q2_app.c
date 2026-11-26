/*
 * =====================================================================================================
 * q2_app.c — вход модуля Q2.ELF: таблица приложения для ядра ESP и задача, в которой идёт игра.
 * =====================================================================================================
 *
 * КАК ЗАПУСКАЕТСЯ ПРИЛОЖЕНИЕ VDAC4
 *   Плагин Z80 просит ядро ESP запустить Q2.ELF. Ядро грузит модуль в PSRAM, связывает и зовёт его
 *   main(argc, argv): argv[1] — таблица функций ядра (zm_core_t), argv[2] — место, куда модуль кладёт
 *   указатель на свою таблицу zm_app_t. Дальше ядро зовёт start — там модуль заводит задачу игры.
 *   При выгрузке ядро зовёт stop, потом само освобождает всю память модуля, его задачи и сам модуль.
 *
 * ЗАДАЧА ИГРЫ
 *   То же, что main() у id: Qcommon_Init один раз, затем Qcommon_Frame в цикле с числом миллисекунд,
 *   прошедших с прошлого кадра. Перед этим — ядро «бомж-3D» в Tang (bz_link_open): без него рисовать
 *   нечем.
 *   Ядро ESP 1, приоритет 1 — как у обмена с Evo (loopTask Arduino): FreeRTOS делит время поровну
 *   между задачами одного приоритета, и Evo получает ответы, хотя игра считает без остановки.
 *   Задача приоритетом 2+ на ядре 1 «душит» обмен (PLAYER 1.26: «ESP does not answer»).
 *   Стек — 192 КБ в PSRAM: у рендера id большие массивы на стеке (рёбра, поверхности).
 *
 * ВЫХОД
 *   Sys_Error, Sys_Quit, exit и просьба ядра выгрузиться (stop) приходят в q2_esp_exit: longjmp
 *   назад в задачу игры, мимо всех функций Quake 2 (у id они зовут exit, здесь процесса нет). Задача
 *   отпускает Tang (ядро ESP вернёт его в TS-Config) и кончается.
 */
#include "q2esp.h"
#include "../../port/qcommon/qcommon.h"

#include <setjmp.h>

#define Q2_STACK (192u * 1024u)

/* Самый длинный кадр, который видит игра (мс). Загрузка карты в Quake 2 идёт целиком внутри одного кадра
 * («precache» → таблицы столкновений → CL_PrepRefresh), и на плате, где pak читается с карты Evo через Z80
 * (~76 КБ/с), этот кадр длится минуты: base2 — 141 с (24.11.2026). Следующий кадр сервера получал
 * msec = 141 000, его часы svs.realtime прыгали вперёд, и SV_CheckTimeouts выкидывал своего же клиента:
 * «молчал» дольше timeout = 125 с. Сообщение об отключении при этом не уходит (клиент сразу cs_free), и
 * клиент навсегда остаётся «подключённым» к серверу, который его не знает, — чёрный экран вместо демо.
 * Предел 200 мс — как у самого id в CL_Frame (cls.frametime не больше 1/5 с): игровые кадры на плате
 * короче, а время, ушедшее на загрузку, игра просто не замечает. */
#define Q2_FRAME_MAX_MS 200

const zm_core_t *q2_core;
volatile int     q2_stop_request;

static jmp_buf       exit_jmp;
static volatile int  task_running;
static int           exit_code;

void q2_esp_exit (int code)
{
	exit_code = code;
	longjmp (exit_jmp, 1);
}

#define Q2M_TICKS_PER_US 240     /* частота ядра ESP32-S3, МГц */

/*
 * Счётчики производительности Xtensa на ядре игры (26.11.2026): куда уходят такты. Unreal NG исполняет
 * 240 команд за мкс без задержек памяти, и в нём рендер втрое-вчетверо быстрее, чем на плате, — похоже,
 * на плате ядро в основном ждёт PSRAM (кэш команд 16 КБ, данных 32 КБ). Два счётчика (PM0, PM1) отладочного
 * блока ядра, доступ — командами RER/WER (регистры ERI, xtensa-debug-module.h ESP-IDF). Каждые 5 с — другая
 * пара событий, по кругу: стоянки из-за промаха кэша команд и данных; все стоянки команд и данных;
 * исполненные команды и «пузыри» конвейера. В Unreal NG команд RER/WER нет — там замер выключен (строка
 * прошивки «…unreal-ng…»).
 */
#define PM_PGM     0x101000u    /* ERI_PERFMON_PGM: бит 0 — счёт включён */
#define PM_COUNT0  0x101080u    /* ERI_PERFMON_PM0 (+4 — PM1) */
#define PM_CTRL0   0x101100u    /* ERI_PERFMON_PMCTRL0: выбор события << 8, маска << 16, TRACELEVEL << 4 */

static uint32_t pm_read (uint32_t a)
{
	uint32_t v;
	__asm__ volatile ("rer %0, %1" : "=a"(v) : "a"(a));
	return v;
}

static void pm_write (uint32_t a, uint32_t v)
{
	__asm__ volatile ("wer %0, %1" : : "a"(v), "a"(a) : "memory");
}

static void perf_report (int ms)
{
	/* {событие, маска} для PM0 и PM1 — xt_perf_consts.h */
	static const uint16_t cfg[3][4] = {
		{ 4, 0x0001, 3, 0x0008 },      /* стоянка: промах кэша команд | промах кэша данных */
		{ 4, 0x01FF, 3, 0x01FE },      /* все стоянки команд | все стоянки данных           */
		{ 2, 0x8DFF, 6, 0x01FD },      /* исполнено команд | пузыри конвейера               */
	};
	static const char *what[3][2] = { { "I-cache miss stall", "D-cache miss stall" },
	                                  { "all I stall", "all D stall" }, { "instructions", "bubbles" } };
	static int  state = -1;            /* -1 — не заводили; 0…2 — какая пара считается сейчас */
	uint64_t    c0, c1, cycles = (uint64_t)ms * 240000u;
	int         k;

	if (state == -2 || !q2_core->firmware || strstr (q2_core->firmware, "unreal"))
	{
		state = -2;
		return;
	}
	if (state >= 0 && cycles && ms < 15000)    /* дольше — 32-битный счётчик мог перевалить */
	{
		c0 = pm_read (PM_COUNT0);
		c1 = pm_read (PM_COUNT0 + 4);
		q2_core->log ("q2: cpu1 %u ms: %s %u.%u%%, %s %u.%u%% of cycles", (unsigned)ms,
		              what[state][0], (unsigned)(c0 * 100u / cycles), (unsigned)(c0 * 1000u / cycles % 10u),
		              what[state][1], (unsigned)(c1 * 100u / cycles), (unsigned)(c1 * 1000u / cycles % 10u));
	}
	state = (state + 1) % 3;
	pm_write (PM_PGM, 0);
	for (k = 0; k < 2; k++)
	{
		/* TRACELEVEL 15 без KRNLCNT: считать при любом CINTLEVEL ≤ 15, то есть всегда. (KRNLCNT = 1 считает ТОЛЬКО
		 * при CINTLEVEL > TRACELEVEL — в прерываниях; так было в board_log14: «команд 3 % тактов».) */
		pm_write (PM_CTRL0 + 4u * k, ((uint32_t)cfg[state][2 * k] << 8) | ((uint32_t)cfg[state][2 * k + 1] << 16) | (15u << 4));
		pm_write (PM_COUNT0 + 4u * k, 0);
	}
	pm_write (PM_PGM, 1);
}

/* Раз в 5 с — в журнал: кадров в секунду и среднее время частей кадра (метки Q2M_* в port/, q2m.h). */
static void report_times (int frames, int ms)
{
	static const char *name[Q2M_N] = { "sv", "game", "cl", "scr", "rf", "snd",
	                                   "setup", "world", "bmod", "ent", "part", "alpha", "link", "lread",
	                                   "face", "lm", "tex", "atri", "lwait",
	                                   "ents", "2d", "end",
	                                   "frm", "lit", "xf", "trs", "mdl", "vert", "tri" };
	char line[120];
	int  i, n;

	/* строки: части кадра, (с 25.11.2026) части рендера и LINK, внутри рендера и (26.11.2026) части экрана вне
	 * рендера — журнал режет строку на 136 знаках */
	n = snprintf (line, sizeof(line), "q2: %d.%d fps, ms/frame", frames * 10000 / ms / 10, frames * 10000 / ms % 10);
	for (i = 0; i < Q2M_N && n < (int)sizeof(line); i++)
	{
		long long us = frames ? q2m_acc[i] / Q2M_TICKS_PER_US / frames : 0;   /* метки — в тактах (sys_esp.c) */
		if (i == Q2M_R_SETUP || i == Q2M_F_FACE || i == Q2M_C_ENTS || i == Q2M_A_FRM)
		{
			q2_core->log ("%s", line);
			n = snprintf (line, sizeof(line), i == Q2M_R_SETUP ? "q2: rf parts, ms/frame" :
			              i == Q2M_F_FACE ? "q2: rf inside, ms/frame" :
			              i == Q2M_C_ENTS ? "q2: scr parts, ms/frame" : "q2: ent parts, ms/frame");
		}
		if (i >= Q2M_K_MDL)        /* 26.11.2026: счёт (моделей, вершин, треугольников) за кадр, а не такты */
			n += snprintf (line + n, sizeof(line) - n, " %s %d", name[i], frames ? (int)(q2m_acc[i] / frames) : 0);
		else
			n += snprintf (line + n, sizeof(line) - n, " %s %d.%d", name[i], (int)(us / 1000), (int)(us % 1000 / 100));
		q2m_acc[i] = 0;
	}
	q2_core->log ("%s", line);
	{
		void bz_link_report (int frames); /* bz_link.c: поток в ядро и ожидание места в его очереди */
		void snd_esp_report (void);       /* snddma_esp.c: звук за эти 5 с (если он запущен) */
		bz_link_report (frames);
		snd_esp_report ();
	}
	perf_report (ms);
}

static void game_task (void *arg)
{
	/* Командная строка Quake 2: пути — относительно хранилища (store_esp.c); экран — режим 12,
	 * 424×240 (16:9; вывод ядра ×3 = 1272×720).
	 * maxentities 800 (у id 1024; 24.11.2026, PSRAM платы): объект игры (edict_t) — 875 байт, и 224 лишних
	 * объекта держали 191 КБ, которых не хватало картам. Замер пиков на ПК (47 карт после загрузки и бои
	 * 90 с на сложности 3, results/edicts): выше 800 — только space (912), но она по памяти ESP не
	 * помещается и так; дальше всех — security и power2 (750), command (688). */
	static char *argv[] = { "q2", "+set", "basedir", "q2", "+set", "sw_mode", "12",
	                        "+set", "maxentities", "800",
	                        /* звук смешивается на 0,3 с вперёд (у id 0,2): кадр на плате бывает 0,3–0,5 с,
	                         * и с 0,2 звук рвался тишиной (плата 25.11, «q2: sound: … silence») */
	                        "+set", "s_mixahead", "0.3", NULL };
	/* static: переменные, изменённые после setjmp, longjmp может испортить (так говорит стандарт C) */
	static uint32_t version;
	static int      last, now, report, frames;

	(void)arg;
	if (!setjmp (exit_jmp))
	{
		if (bz_link_open (&version) != ZM_OK)
			q2_esp_exit (2);
		q2_core->log ("q2: core on LINK, version %08x", (unsigned)version);
		/* адрес кода — для перевода адреса сбоя в функцию по отладочной копии ELF (25.11.2026) */
		q2_core->log ("q2: code anchor game_task %p", (void *)game_task);
		q2_mem_report ("before init");
		Qcommon_Init ((int)(sizeof(argv) / sizeof(argv[0])) - 1, argv);   /* без последнего NULL */
		q2_mem_report ("after init");
		q2_level_init ();               /* область уровня — пока куча сплошная, до первой карты (sys_esp.c) */
		BZ_AllocCmdBuffers ();          /* буферы команд — следом, внизу кучи, до первой карты (bz_front.c) */
		last = report = Sys_Milliseconds ();
		for (;;)
		{
			now = Sys_Milliseconds ();
			if (now == last)
			{
				q2_core->delay (1);      /* кадр быстрее миллисекунды: Qcommon_Frame нужен msec > 0 */
				continue;
			}
			Qcommon_Frame (now - last > Q2_FRAME_MAX_MS ? Q2_FRAME_MAX_MS : now - last);   /* загрузка — не «молчание» */
			last = now;
			frames++;
			if (now - report >= 5000)
			{
				report_times (frames, now - report);
				frames = 0;
				report = now;
			}
		}
	}
	q2_core->log ("q2: exit %d", exit_code);
	bz_link_close ();
	task_running = 0;
}

/* Прошивка не старше vdac4-0.1.<least>: 0.1.164 — первая, что берёт ядро с карты Evo (MOD_QUERY с
 * флагом 2), 0.1.166 — первая, что без флешки читает pak с карты Evo по запросу у программы Z80.
 * ABI у них не менялся (1.19, 1.20), поэтому смотрим строку версии «vdac4-A.B.C». */
static int firmware_at_least (const char *fw, unsigned least)
{
	unsigned a = 0, b = 0, c = 0;
	const char *p;

	if (!fw || strncmp (fw, "vdac4-", 6))
		return 0;
	p = fw + 6;
	while (*p >= '0' && *p <= '9')
		a = a * 10 + (unsigned)(*p++ - '0');
	if (*p++ != '.')
		return 0;
	while (*p >= '0' && *p <= '9')
		b = b * 10 + (unsigned)(*p++ - '0');
	if (*p++ != '.')
		return 0;
	while (*p >= '0' && *p <= '9')
		c = c * 10 + (unsigned)(*p++ - '0');
	return a > 0 || b > 1 || (b == 1 && c >= least);
}

/*
 * Горячий модуль Q2HOT.ELF (26.11.2026, пользователь: «Да, переноси»; esp/hot/q2hot.c, gpu/front/bz_int.h).
 * На плате ~22 % тактов ядро игры ждёт выборки команд из PSRAM (board_log15). Самые частые функции переднего
 * края (грань мира, треугольники моделей, частицы) прошивка кладёт во внутреннее ОЗУ (code_ram, ABI 1.19) —
 * целым модулем, который присылает запускатель (ESP App SPG: «+Q2HOT.ELF»). Нет прошивки 0.1.157+, нет файла,
 * не лёг во внутреннее ОЗУ, не той версии — игра идёт на своих копиях этих функций, как раньше.
 */
static void *hot_handle;
void BZ_HotTables (void **imp, void **exp);    /* gpu/front/bz_front.c */
int  BZ_HotUse (int on);

static void hot_load (const zm_core_t *core)
{
	static const uint32_t want[] = { 16u * 1024u, 8u * 1024u };
	uint32_t got = 0, placed = 0;
	void    *imp, *exp;
	int      r = ZM_ERROR, entry = -99;
	unsigned i;

	if (core->abi_minor < 19 || core->size < offsetof (zm_core_t, module_open_ex) + sizeof(core->module_open_ex))
	{
		core->log ("q2: hot module: firmware without code_ram (ABI 1.%u) — hot code stays in PSRAM", core->abi_minor);
		return;
	}
	for (i = 0; i < sizeof(want) / sizeof(want[0]) && r != ZM_OK; i++)
		r = core->code_ram (want[i], &got);
	if (r != ZM_OK)
	{
		core->log ("q2: hot module: code_ram refused (%d) — hot code stays in PSRAM", r);
		return;
	}
	BZ_HotTables (&imp, &exp);
	r = core->module_open_ex ("Q2HOT.ELF", ZM_MODULE_TEXT_INTERNAL, imp, exp, &hot_handle, &entry, &placed);
	if (r != ZM_OK)
	{
		core->log ("q2: hot module: Q2HOT.ELF not opened (%d) — hot code stays in PSRAM", r);
		hot_handle = NULL;
		return;
	}
	if (entry != 0 || !(placed & ZM_MODULE_TEXT_INTERNAL) || !BZ_HotUse (1))
	{
		/* не той версии (entry < 0) или код лёг в PSRAM — толку нет, свои копии не хуже */
		core->log ("q2: hot module: Q2HOT.ELF entry %d, %s — not used", entry,
		           (placed & ZM_MODULE_TEXT_INTERNAL) ? "code in internal RAM" : "code in PSRAM");
		BZ_HotUse (0);
		core->module_close (hot_handle);
		hot_handle = NULL;
		return;
	}
	core->log ("q2: hot module: Q2HOT.ELF code in internal RAM (code_ram %u bytes)", (unsigned)got);
}

static int start (const zm_core_t *core)
{
	int r;

	q2_core = core;
	/* нужны: LINK пачками (ABI 1.15), стек сразу в PSRAM (1.18) и файлы данных приложения с карты Evo —
	 * pak в кэше на флешке (1.20, прошивка 0.1.165: data_open, data_read, data_close) */
	if (core->abi_minor < 20 ||
	    core->size < offsetof (zm_core_t, data_close) + sizeof(core->data_close))
	{
		core->log ("q2: ESP firmware too old (ABI 1.%u, need 1.20+: vdac4-0.1.165)", core->abi_minor);
		return ZM_ERROR;
	}
	if (!firmware_at_least (core->firmware, 166))
	{
		/* ядро — только с карты Evo (Evo → ESP32 → LINK → BL616 → ПЛИС): прошивка старше 0.1.164 взяла
		 * бы его с microSD Tang — так нельзя (правило пользователя 23.11); игра — и без флешки USB
		 * (пользователь 23.11: «Игра должна работать и без флешки otg esp32») — с 0.1.166 */
		core->log ("q2: ESP firmware %s: need vdac4-0.1.166+ (core and pak from the Evo card, "
		           "pak without a USB drive)", core->firmware ? core->firmware : "?");
		return ZM_ERROR;
	}
	hot_load (core);                /* code_ram — лучше всего здесь, в start (zm_app.h) */
	q2_stop_request = 0;
	task_running = 1;
	r = core->task_start (game_task, NULL, Q2_STACK | ZM_TASK_STACK_PSRAM_FIRST, 1, 1);
	if (r != ZM_OK)
	{
		task_running = 0;
		core->log ("q2: game task not started (%d)", r);
	}
	return r;
}

static void stop (void)
{
	uint32_t t0 = q2_core->millis ();

	q2_stop_request = 1;            /* Sys_SendKeyEvents увидит и выйдет из игры */
	while (task_running && q2_core->millis () - t0 < 3000)
		q2_core->delay (10);
	if (task_running)
		q2_core->log ("q2: game task did not stop in 3 s");
	else if (hot_handle)
	{
		BZ_HotUse (0);
		q2_core->module_close (hot_handle);
		hot_handle = NULL;
	}
}

/* Команд Z80 у Quake 2 пока нет (клавиатура, мышь, загрузчик — следующие шаги). */
static int command (uint8_t cmd, const uint8_t *data, uint16_t length)
{
	(void)cmd;
	(void)data;
	(void)length;
	return ZM_ERROR;
}

int main (int argc, char **argv)
{
	static const zm_app_t app = {
		sizeof(zm_app_t), ZM_APP_ABI_MAJOR, ZM_APP_ABI_MINOR, "quake2", "0.1",
		0x70, 0x76, start, stop, command, NULL, NULL, NULL,
	};

	(void)argc;
	q2_core = (const zm_core_t *)argv[1];
	*(const zm_app_t **)argv[2] = &app;
	return ZM_APP_ABI_MAJOR;
}
