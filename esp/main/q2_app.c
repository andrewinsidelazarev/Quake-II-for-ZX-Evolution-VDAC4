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

/* Раз в 5 с — в журнал: кадров в секунду и среднее время частей кадра (метки Q2M_* в port/, q2m.h). */
static void report_times (int frames, int ms)
{
	static const char *name[Q2M_N] = { "sv", "game", "cl", "scr", "rf", "snd" };
	char line[120];
	int  i, n;

	n = snprintf (line, sizeof(line), "q2: %d.%d fps, ms/frame", frames * 10000 / ms / 10, frames * 10000 / ms % 10);
	for (i = 0; i < Q2M_N && n < (int)sizeof(line); i++)
	{
		long long us = frames ? q2m_acc[i] / frames : 0;
		n += snprintf (line + n, sizeof(line) - n, " %s %d.%d", name[i], (int)(us / 1000), (int)(us % 1000 / 100));
		q2m_acc[i] = 0;
	}
	q2_core->log ("%s", line);
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
	                        "+set", "maxentities", "800", NULL };
	/* static: переменные, изменённые после setjmp, longjmp может испортить (так говорит стандарт C) */
	static uint32_t version;
	static int      last, now, report, frames;

	(void)arg;
	if (!setjmp (exit_jmp))
	{
		if (bz_link_open (&version) != ZM_OK)
			q2_esp_exit (2);
		q2_core->log ("q2: core on LINK, version %08x", (unsigned)version);
		q2_mem_report ("before init");
		Qcommon_Init ((int)(sizeof(argv) / sizeof(argv[0])) - 1, argv);   /* без последнего NULL */
		q2_mem_report ("after init");
		q2_level_init ();               /* область уровня — пока куча сплошная, до первой карты (sys_esp.c) */
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
