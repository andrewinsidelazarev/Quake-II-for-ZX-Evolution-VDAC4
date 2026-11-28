/*
 * =====================================================================================================
 * sys_esp.c — Sys_* Quake 2 на ESP VDAC4 (у id это win32/sys_win.c и win32/q_shwin.c).
 * =====================================================================================================
 *
 * Время — zm_core_t::millis (миллисекунды с запуска ESP); Quake 2 считает от первого вызова.
 * Ошибка и выход — в журнал ядра и q2_esp_exit (назад в задачу игры, q2_app.c).
 * Консоль Quake 2 (Com_Printf) — тоже в журнал ядра (его видно сетевой консолью ESP).
 *
 * ПАМЯТЬ HUNK
 *   Модели id грузит в «hunk»: Hunk_Begin заводит область, Hunk_Alloc раздаёт из неё куски подряд,
 *   Hunk_End говорит «модель загружена», Hunk_Free освобождает всё разом. На Windows id резервирует
 *   сразу большой кусок адресов (модели 2 МБ, карта 16 МБ!) и берёт реальную память по мере нужды.
 *   У ESP виртуальной памяти нет, а PSRAM всего 8 МБ, поэтому здесь иначе:
 *     - область — цепочка кусков PSRAM. Точный запрос (меньше HUNK_EXACT, 2 МБ: md2, спрайт) — первым
 *       куском целиком. Область карты (у id «запас» 16 МБ) начинается с малого куска HUNK_STEP (64 КБ),
 *       следующие куски — по HUNK_STEP или ровно по запросу, если он больше: столько, сколько карта
 *       правда занимает, без запаса;
 *     - у точного запроса первый Hunk_Alloc обязан вернуть тот же адрес, что Hunk_Begin (id хранит его
 *       как указатель на заголовок модели), — первый кусок раздаётся с самого начала;
 *     - Hunk_End отдаёт неиспользованный хвост последнего куска (realloc на меньший размер: прошивка
 *       0.1.166+ урезает блок на месте — указатели внутрь него не меняются; прежние прошивки realloc
 *       всегда переносили блок — это нашёл Unreal NG 24.11; если блок всё же сдвинулся бы — Sys_Error,
 *       иначе модель осталась бы с висячими указателями);
 *     - заголовок куска (следующий, размер) лежит перед данными, Hunk_Free идёт по цепочке.
 *   Как и у id, выданная память обнулена.
 *   Почему область карты без запаса (плата, 24.11.2026): раньше её первый кусок был 2 МБ «на вырост».
 *   На плате после запуска свободно на 1,4 МБ меньше, чем в Unreal NG (прошивка сама держит больше
 *   PSRAM), — 2 МБ запаса забрали почти всё, и первый же буфер lump-а карты (FS_ReadPart, 145 КБ) не
 *   нашёл места: «malloc: no memory for 148853 bytes» на base2, хотя сама карта поместилась бы.
 *   Кусок, если такого нет одним куском, берётся меньше (вдвое, но не меньше запроса). Памяти нет
 *   совсем — в журнал отчёт (mem_report: сколько PSRAM свободно всего и самым большим куском, сколько у
 *   зоны и hunk) и Sys_Error.
 *   Адрес области карты id нужен лишь для Hunk_Free, поэтому её первый кусок может остаться пустым
 *   (первый Hunk_Alloc больше него — кусок сжимается до заголовка). Точный запрос (md2 — ровно по файлу,
 *   спрайт) первым куском берётся целиком: у модели и спрайта адрес области — это заголовок, и первый
 *   Hunk_Alloc обязан лечь в первый кусок (иначе extradata смотрел бы на пустой кусок — Sys_Error).
 *   Переходя к следующему куску, hunk сжимает прошлый на месте (как Hunk_End) — хвосты не теряются.
 *   Не хватило и самого малого: рендер отдаёт модели прошлой карты (R_EspPurgeStale, p06), по одной,
 *   и выделение повторяется (24.11: на смене карты демо 2 модели прошлой держали ~3,4 МБ).
 */
#include "q2esp.h"
#include "../../port/qcommon/qcommon.h"

int      curtime;
unsigned sys_frame_time;

/* ---------------------------------------------------------------- замер частей кадра (q2m.h) */
/* В port/ стоят метки Q2M_BEGIN/END вокруг частей кадра (сервер, игра, клиент, экран, рендер, звук) —
 * те же, что у замерочной сборки ПК. Здесь время — clock_gettime (микросекунды; прошивка его даёт),
 * итоги раз в 5 с печатает задача игры (q2_app.c). */
long long q2m_t0[Q2M_N];
long long q2m_acc[Q2M_N];

/* Метка времени — в тактах ядра (240 МГц), 26.11.2026. Раньше — clock_gettime прошивки и деление 64-битного на
 * 1000 программно: профиль Unreal NG — 5,7 % времени кадра. Здесь одна команда rsr.ccount; 32-битный счётчик
 * (17,9 с на круг) продлевается до 64 бит своим старшим словом у каждого ядра (у ESP32-S3 у ядер разный
 * счётчик; ядро — бит 13 PRID: 0xCDCD — ядро 0, 0xABAB — ядро 1). Задачи, что ставят метки, привязаны к
 * ядру (игра — 1, задача LINK — 0). В микросекунды переводит отчёт (q2_app.c, Q2M_TICKS_PER_US). */
long long Q2M_Now (void)
{
	static uint32_t last[2], high[2];
	uint32_t c, prid;
	int      k;

	__asm__ volatile ("rsr.ccount %0" : "=a"(c));
	__asm__ volatile ("rsr.prid %0" : "=a"(prid));
	k = (int)((prid >> 13) & 1u);
	if (c < last[k])
		high[k]++;
	last[k] = c;
	return (long long)(((uint64_t)high[k] << 32) | c);
}

/* ---------------------------------------------------------------- время */

int Sys_Milliseconds (void)
{
	static uint32_t base;
	static int      started;
	uint32_t        now = q2_core->millis ();

	if (!started)
	{
		base = now;
		started = 1;
	}
	curtime = (int)(now - base);
	return curtime;
}

/* ---------------------------------------------------------------- ошибки, выход, консоль */

void Sys_Error (char *error, ...)
{
	va_list argptr;
	char    text[1024];

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);
	q2_core->log ("q2: Sys_Error: %s", text);
	q2_esp_exit (1);
}

void Sys_Quit (void)
{
	q2_esp_exit (0);
}

/* Строки консоли — в журнал ядра. Строка журнала — до 136 знаков: длинные режутся. */
void Sys_ConsoleOutput (char *string)
{
	char line[120];
	int  n = 0;

	for (; *string; string++)
	{
		if (*string == '\n' || n == (int)sizeof(line) - 1)
		{
			line[n] = 0;
			if (n)
				q2_core->log ("q2> %s", line);
			n = 0;
			if (*string == '\n')
				continue;
		}
		if ((unsigned char)*string >= 32)
			line[n++] = (*string & 127) < 32 ? '.' : (char)(*string & 127);   /* цветной текст id — бит 7 */
	}
	if (n)
	{
		line[n] = 0;
		q2_core->log ("q2> %s", line);
	}
}

char *Sys_ConsoleInput (void)
{
	return NULL;
}

/* Каждый кадр клиента: ввод (клавиатура и мышь с Evo — следующий шаг) и время событий. */
void Sys_SendKeyEvents (void)
{
	if (q2_stop_request)
		q2_esp_exit (0);
	sys_frame_time = (unsigned)Sys_Milliseconds ();
}

void Sys_Init (void)
{
}

void Sys_AppActivate (void)
{
}

void Sys_CopyProtect (void)
{
}

char *Sys_GetClipboardData (void)
{
	return NULL;
}

/* ---------------------------------------------------------------- игра в этом же модуле */

void *GetGameAPI (void *import);

void Sys_UnloadGame (void)
{
}

void *Sys_GetGameAPI (void *parms)
{
	return GetGameAPI (parms);
}

/* ---------------------------------------------------------------- память hunk */

#define HUNK_EXACT (2u << 20)       /* запрос Hunk_Begin меньше этого — точный (md2, спрайт), иначе — карта */
#define HUNK_STEP  (64u << 10)      /* кусок области карты, если запрос меньше (мелкие lump-ы — подряд) */

typedef struct hunkblk_s
{
	struct hunkblk_s *next;
	uint32_t          size;     /* байт данных в куске */
	uint32_t          pad[2];   /* заголовок 16 байт: данные — с адреса, кратного 16 */
} hunkblk_t;

static hunkblk_t *hunk_first, *hunk_last;
static uint32_t   hunk_used;    /* занято в последнем куске */
static uint32_t   hunk_total;   /* занято во всей области — итог Hunk_End */
static int        hunk_exact;   /* 1 — точный запрос: первый Hunk_Alloc обязан лечь в первый кусок */
static int        hunk_level;   /* 1 — область карты начата в области уровня (ниже) */

/* ---------------------------------------------------------------- область уровня */

/*
 * ОБЛАСТЬ УРОВНЯ (24.11.2026)
 *   Зачем. Unreal NG с PSRAM как на плате: base2 и waste2 грузились по разу, а base2 после waste2 — нет:
 *   свободно 576 КБ, но самый большой кусок — 160 КБ, а кусок карты нужен на 211 КБ. За два уровня
 *   таблицы столкновений, куски карты и мелкие блоки игры перемешались по куче, и сплошных мест не
 *   осталось. Общий объём при этом помещался.
 *   Как. Сразу после Qcommon_Init, пока куча ещё сплошная, берётся один большой кусок — «область уровня»
 *   (q2_level_init). В неё подряд, без дыр, ложатся таблицы столкновений (CM_AllocTables → хук
 *   CM_LevelAllocHook) и hunk карты рендера (Hunk_Begin карты берёт весь остаток области, Hunk_End
 *   отдаёт хвост). Грузится другая карта — CM_FreeTables зовёт CM_LevelResetHook, и область снова
 *   заполняется с начала. Не влезло в область — обычная куча, как раньше. Так был устроен и сам
 *   Quake 2 на ПК: память уровня — один кусок с «отметкой» начала уровня.
 *   Тонкость. После сброса заголовки кусков hunk старой карты в области затёрты новыми таблицами, поэтому
 *   Hunk_Free такой карты по цепочке не ходит: её куски из обычной кучи (не влезла в область) записаны
 *   отдельно — level_spill. Старая карта к этому времени уже не рисуется (R_EspFrameBlocked: CM_LoadMap
 *   зовёт CM_NewMapHook раньше CM_FreeTables), а освобождает её R_BeginRegistration — раньше, чем
 *   начнётся новая.
 */
/*   Второй конец области (сверху вниз) — временный буфер (q2_level_scratch): буфер lump-ов карты рендера
 *   (Mod_LumpScratch, p06), lump-ов столкновений (CM_Lump → CM_LevelScratchHook) и группы кадров md2 по
 *   пути в SDRAM ядра. Он нужен, пока строится уровень, и больших сплошных кусков от обычной кучи тогда
 *   не требуется вовсе. Карта рендера растёт снизу на месте (Hunk_Alloc), пока не упрётся в буфер сверху.
 *   Замер (Unreal NG с PSRAM как на плате): base2 — столкновения 642 КБ + карта ~1650 КБ + буфер 182 КБ.
 */
#define LEVEL_KEEP  (704u << 10)    /* обычной куче оставить хотя бы столько: шапки md2, буфер команд кадра,
                                     * картинки по пути (Unreal NG, PSRAM как на плате: при 400 КБ на втором
                                     * круге демо распаковке шкурки PCX не нашлось 58 КБ одним куском).
                                     * 25.11.2026: 640 → 704 КБ — со звуком (заголовки звуков, файлы pak при
                                     * потоковой загрузке) плата на base2 после waste2 не нашла 53 КБ; звук
                                     * отдал при запуске ~60 КБ (таблица громкости, буферы), область уровня
                                     * осталась прежней — 2,4 МБ при пике base2 2254 КБ */
#define LEVEL_MIN   (1u << 20)      /* область меньше этого не заводить */
#define LEVEL_SPILL 64              /* кусков из обычной кучи у карт в области (каждый ≥ 64 КБ — хватит) */

static byte     *level_base;        /* начало области (NULL — области нет) */
static uint32_t  level_size;        /* её размер */
static uint32_t  level_used;        /* занято с начала (новые куски — отсюда) */
static uint32_t  level_peak;        /* наибольшее занятое — для журнала */
static uint32_t  level_last;        /* смещение последнего выданного куска: только его можно сжать */
static uint32_t  level_top;         /* занято сверху временным буфером (0 — буфера нет) */
static struct { void *area; hunkblk_t *blk; } level_spill[LEVEL_SPILL];
static int       level_nspill;

static int level_owns (const void *p)
{
	return level_base && (const byte *)p >= level_base && (const byte *)p < level_base + level_size;
}

/* сколько свободно между нижней частью и временным буфером сверху */
static uint32_t level_room (void)
{
	return level_base ? level_size - level_top - level_used : 0;
}

/* n байт снизу области (кратно 16) или NULL — не влезает */
static void *level_take (uint32_t n)
{
	byte *p;

	n = (n + 15) & ~15u;
	if (level_room () < n)
		return NULL;
	p = level_base + level_used;
	level_last = level_used;
	level_used += n;
	if (level_used > level_peak)
		level_peak = level_used;
	return p;
}

/* CM_LevelAllocHook (cmodel.c): обнулённая таблица столкновений из области или NULL */
static void *level_cm_alloc (int size)
{
	void *p = level_take ((uint32_t)size);

	if (p)
		memset (p, 0, (uint32_t)size);
	return p;
}

/* CM_LevelResetHook (cmodel.c): таблицы прежней карты отданы — область снова с начала */
static void level_reset (void)
{
	level_used = 0;
	level_last = 0;
	level_top = 0;      /* временный буфер, брошенный оборванной загрузкой, тоже больше не нужен */
}

/* Временный буфер на size байт сверху области (один за раз) или NULL — тогда обычная куча. */
void *q2_level_scratch (int size)
{
	uint32_t n = ((uint32_t)size + 15) & ~15u;

	if (level_top || size <= 0 || level_room () < n)
		return NULL;
	level_top = n;
	if (level_used + level_top > level_peak)
		level_peak = level_used + level_top;
	return level_base + level_size - n;
}

/* Отдать временный буфер; 1 — он из области (его место освобождено, если это нынешний буфер сверху),
 * 0 — не из неё (тогда его освобождает зовущий). Указатель внутрь области — всегда 1: даже буфер,
 * брошенный загрузкой, оборванной ошибкой (после сброса области), не должен уйти в free кучи. */
int q2_level_scratch_free (void *p)
{
	if (!p || !level_owns (p))
		return 0;
	if (level_top && (byte *)p == level_base + level_size - level_top)
		level_top = 0;
	return 1;
}

/* CM_LevelScratchHook / CM_LevelScratchFreeHook (cmodel.c) — то же для lump-ов столкновений */
static void *level_cm_scratch (int size)
{
	return q2_level_scratch (size);
}

static int level_cm_scratch_free (void *p)
{
	return q2_level_scratch_free (p);
}

/* Самый большой кусок PSRAM, что куча даст сейчас: двоичный поиск с точностью 16 КБ (взять и отдать). */
static uint32_t mem_largest (void)
{
	uint32_t lo = 0, hi = 8u << 20;

	while (hi - lo > 16384)
	{
		uint32_t mid = lo + (hi - lo) / 2;
		void    *p = q2_core->alloc (mid, ZM_MEM_PSRAM);

		if (p)
		{
			(q2_core->free) (p);   /* в скобках: иначе макрос free из compat.h */
			lo = mid;
		}
		else
			hi = mid;
	}
	return lo;
}

/* Зона Z_Malloc (common.c) по меткам и её самые большие блоки — в журнал. Заголовок блока — как
 * zhead_t в common.c (там он объявлен прямо в .c, поэтому повторён здесь); z_chain — голова кольца. */
typedef struct zhead_rep_s
{
	struct zhead_rep_s *prev, *next;
	short               magic;
	short               tag;
	int                 size;
} zhead_rep_t;

static void zone_report (void)
{
	extern zhead_rep_t z_chain;
	struct { int tag; unsigned bytes, count; } tags[8];
	int          ntags = 0, i, j;
	unsigned     top[6] = { 0 };
	int          toptag[6] = { 0 };
	zhead_rep_t *z;
	char         line[200];
	int          at;

	for (z = z_chain.next; z && z != &z_chain; z = z->next)
	{
		for (i = 0; i < ntags && tags[i].tag != z->tag; i++)
			;
		if (i == ntags && ntags < 8)
		{
			tags[ntags].tag = z->tag;
			tags[ntags].bytes = tags[ntags].count = 0;
			ntags++;
		}
		if (i < ntags)
		{
			tags[i].bytes += (unsigned)z->size;
			tags[i].count++;
		}
		for (i = 0; i < 6 && top[i] >= (unsigned)z->size; i++)
			;
		if (i < 6)
		{
			for (j = 5; j > i; j--)
			{
				top[j] = top[j - 1];
				toptag[j] = toptag[j - 1];
			}
			top[i] = (unsigned)z->size;
			toptag[i] = z->tag;
		}
	}
	at = snprintf (line, sizeof(line), "q2: zone by tag:");
	for (i = 0; i < ntags && at < (int)sizeof(line); i++)
		at += snprintf (line + at, sizeof(line) - at, " %d=%u KB/%u", tags[i].tag, tags[i].bytes >> 10,
		                tags[i].count);
	q2_core->log ("%s", line);
	at = snprintf (line, sizeof(line), "q2: zone largest:");
	for (i = 0; i < 6 && top[i] && at < (int)sizeof(line); i++)
		at += snprintf (line + at, sizeof(line) - at, " %u KB(tag %d)", top[i] >> 10, toptag[i]);
	q2_core->log ("%s", line);
}

/* Одна строка в журнал после загрузки каждой карты (R_EndRegistration, ref_esp p06): самый большой
 * свободный кусок PSRAM, зона, картинки и модели рендера. Сколько свободно всего, на плате видно в
 * начале каждой строки журнала (psram=, psram_min=). Здесь нет жадного захвата всей кучи, как в
 * q2_mem_report: игра идёт, и другим задачам ESP память может понадобиться в любой миг. Зачем (плата,
 * 24.11.2026): Quake 2 упал на base2 без памяти — по таким строкам видно, сколько запаса у каждой карты. */
#define MAP_KEEP_LARGEST (128u << 10)

void q2_map_report (void)
{
	extern int z_count, z_bytes;
	void       R_EspModelBytes (unsigned *bytes, int *count);   /* рендер (ref_esp, p06) */
	unsigned   models;
	int        nmodels;

	R_EspModelBytes (&models, &nmodels);
	/* 28.11.2026, проверка памяти (пользователь: «делай тест на свободную память — не свободно — освобождай»; плата
	 * board_log43 — base2 не загрузилась): после карты самый большой свободный кусок PSRAM меньше MAP_KEEP_LARGEST —
	 * куски моделей на ядре 0 (48 КБ, хвост буфера кадра) отдаются куче, модели дальше рисует игра. */
	{
		uint32_t largest = mem_largest ();

		if (largest < MAP_KEEP_LARGEST && BZ_EntRelease ())
			q2_core->log ("q2: memory test: PSRAM largest %u KB < %u — model chunks given back, models on core 1",
			              (unsigned)(largest >> 10), (unsigned)(MAP_KEEP_LARGEST >> 10));
	}
	/* коротко: журнал ESP обрезает длинные строки (Unreal NG — после ~140 знаков) */
	q2_core->log ("q2: map: PSRAM max %u KB; level %u/%u KB; zone %u KB/%d; models %u KB/%d; cmd %u KB; md2 SDRAM %u KB",
	              (unsigned)(mem_largest () >> 10), (unsigned)(level_used >> 10), (unsigned)(level_size >> 10),
	              (unsigned)z_bytes >> 10, z_count, models >> 10, nmodels, (unsigned)(BZ_CmdBufBytes () >> 10),
	              (unsigned)(BZ_StoreBytes () >> 10));
}

/* Отчёт о памяти в журнал: свободно всего (сумма кусков, взятых жадно — самый большой, потом
 * следующий…) и самым большим куском; зона Z_Malloc (z_bytes, z_count — common.c) и hunk. */
/* Сколько PSRAM свободно всего: куски берутся жадно (самый большой, потом следующий…) и отдаются.
 * Пока считает, вся куча занята — звать только там, где другим задачам ESP память не нужна. */
static uint32_t mem_total (void)
{
	void     *taken[64];
	int       n = 0;
	uint32_t  total = 0;

	while (n < 64)
	{
		uint32_t s = mem_largest ();
		if (s < 16384 || !(taken[n] = q2_core->alloc (s, ZM_MEM_PSRAM)))
			break;
		total += s;
		n++;
	}
	while (n > 0)
		(q2_core->free) (taken[--n]);
	return total;
}

/* Завести область уровня (см. «ОБЛАСТЬ УРОВНЯ» выше). Зовёт задача игры (q2_app.c) после Qcommon_Init,
 * до первой карты: куча ещё не раздроблена картами. Область — из самого большого куска, но так, чтобы
 * обычной куче осталось LEVEL_KEEP. */
void q2_level_init (void)
{
	uint32_t largest = mem_largest (), total = mem_total ();
	uint32_t size = total > LEVEL_KEEP ? total - LEVEL_KEEP : 0;

	if (size > largest)
		size = largest;
	size &= ~16383u;
	if (size < LEVEL_MIN || !(level_base = q2_core->alloc (size, ZM_MEM_PSRAM)))
	{
		q2_core->log ("q2: level area: none (PSRAM free %u KB, largest %u KB)",
		              (unsigned)(total >> 10), (unsigned)(largest >> 10));
		level_base = NULL;
		return;
	}
	level_size = size;
	level_used = level_last = level_peak = level_top = 0;
	CM_LevelAllocHook = level_cm_alloc;
	CM_LevelResetHook = level_reset;
	CM_LevelScratchHook = level_cm_scratch;
	CM_LevelScratchFreeHook = level_cm_scratch_free;
	q2_core->log ("q2: level area %u KB (PSRAM free %u KB, largest %u KB)", (unsigned)(size >> 10),
	              (unsigned)(total >> 10), (unsigned)(largest >> 10));
}

void q2_mem_report (const char *why)
{
	extern int z_count, z_bytes;
	uint32_t   largest = mem_largest (), total = mem_total ();

	q2_core->log ("q2: memory (%s): PSRAM free %u KB, largest %u KB; zone %u KB in %d blocks; hunk %u KB",
	              why, (unsigned)(total >> 10), (unsigned)(largest >> 10), (unsigned)z_bytes >> 10, z_count,
	              (unsigned)(hunk_total >> 10));
	zone_report ();
	{
		void     R_EspImageBytes (unsigned *bytes, int *count);   /* рендер (ref_esp, p06) */
		void     R_EspModelBytes (unsigned *bytes, int *count);
		unsigned images, models, purged;
		int      nimages, nmodels, npurged;

		R_EspImageBytes (&images, &nimages);
		R_EspModelBytes (&models, &nmodels);
		R_EspPurgeStats (&npurged, &purged);
		/* коротко: журнал ESP обрезает длинные строки (Unreal NG — после ~140 знаков) */
		q2_core->log ("q2: render: img %u KB/%d; models %u KB/%d + loading %u KB; purged %d (%u KB); md2 SDRAM %u KB",
		              images >> 10, nimages, models >> 10, nmodels, (unsigned)(hunk_total >> 10),
		              npurged, purged >> 10, (unsigned)(BZ_StoreBytes () >> 10));
		unsigned q2_memfile_bytes (int *count);                    /* compat.c */
		int      nfiles;
		unsigned files = q2_memfile_bytes (&nfiles);

		q2_core->log ("q2: level area %u/%u KB (peak %u); cmd buffers %u KB; files in memory %u KB/%d",
		              (unsigned)(level_used >> 10), (unsigned)(level_size >> 10), (unsigned)(level_peak >> 10),
		              (unsigned)(BZ_CmdBufBytes () >> 10), files >> 10, nfiles);
	}
}

/* Кусок hunk не меньше need байт: сколько просят (want), а если столько нет — вдвое меньше, пока не
 * станет need. Нет и need — модели прошлой карты прочь (R_EspPurgeStale) по одной, и ещё раз; нечего
 * отдать — отчёт о памяти и Sys_Error. */
static hunkblk_t *hunk_block (uint32_t want, uint32_t need)
{
	hunkblk_t *b;

	for (;;)
	{
		b = q2_core->alloc (sizeof(hunkblk_t) + want, ZM_MEM_PSRAM);
		if (b)
			break;
		if (want > need)
		{
			want = want / 2 > need ? want / 2 : need;
			continue;
		}
		if (!q2_mem_reclaim ())
			break;
	}
	if (!b)
	{
		q2_mem_report ("hunk");
		Sys_Error ("Hunk: no memory for %u bytes", (unsigned)want);
	}
	b->next = NULL;
	b->size = want;
	return b;
}

/* Отдать хвост куска после used байт — на месте (прошивка 0.1.166+); сдвинутый блок — Sys_Error.
 * Кусок в области уровня: хвост вернётся области, только если кусок — последний выданный. */
static void hunk_shrink (hunkblk_t *b, uint32_t used)
{
	if (level_owns (b))
	{
		if ((uint32_t)((byte *)b - level_base) == level_last)
			level_used = level_last + ((sizeof(hunkblk_t) + used + 15) & ~15u);
		b->size = used;
		return;
	}
	if (used < b->size)
	{
		hunkblk_t *s = (q2_core->realloc) (b, sizeof(hunkblk_t) + (used ? used : 8));
		if (s != b)
			Sys_Error ("Hunk: heap moved a shrinking block");
		s->size = used;
	}
}

void *Hunk_Begin (int maxsize)
{
	/* точный запрос — первым куском целиком; область карты — в области уровня: кусок пока из одного
	 * заголовка, он растёт на месте с каждым Hunk_Alloc; области нет — малым куском обычной кучи */
	hunk_exact = (uint32_t)maxsize < HUNK_EXACT;
	hunk_level = 0;
	if (hunk_exact)
		hunk_first = hunk_last = hunk_block ((uint32_t)maxsize, (uint32_t)maxsize);
	else if (level_room () >= sizeof(hunkblk_t) + HUNK_STEP)
	{
		hunk_first = hunk_last = level_take (sizeof(hunkblk_t));
		hunk_first->next = NULL;
		hunk_first->size = 0;
		hunk_level = 1;
	}
	else
		hunk_first = hunk_last = hunk_block (HUNK_STEP, 32);
	hunk_used = 0;
	hunk_total = 0;
	return hunk_first + 1;
}

void *Hunk_Alloc (int size)
{
	uint32_t n = ((uint32_t)size + 31) & ~31u;      /* как у id: кратно 32 */
	byte    *p;

	if (hunk_used + n > hunk_last->size && level_owns (hunk_last) &&
	    (uint32_t)((byte *)hunk_last - level_base) == level_last &&
	    level_room () >= hunk_used + n - hunk_last->size)
	{
		/* кусок в области уровня — последний снизу: растёт на месте (до временного буфера сверху) */
		level_used += hunk_used + n - hunk_last->size;
		if (level_used + level_top > level_peak)
			level_peak = level_used + level_top;
		hunk_last->size = hunk_used + n;
	}
	if (hunk_used + n > hunk_last->size)
	{
		hunkblk_t *b;

		if (!hunk_total && hunk_exact)
			Sys_Error ("Hunk_Alloc: first %u bytes do not fit the first block (%u)",
			           (unsigned)n, (unsigned)hunk_last->size);
		hunk_shrink (hunk_last, hunk_used);         /* хвост прошлого куска — обратно в кучу */
		b = hunk_block (n > HUNK_STEP ? n : HUNK_STEP, n);
		if (hunk_level)                             /* карта не влезла в область уровня: кусок кучи — на учёт */
		{
			if (level_nspill >= LEVEL_SPILL)
				Sys_Error ("Hunk: too many heap blocks for a level map");
			level_spill[level_nspill].area = hunk_first + 1;
			level_spill[level_nspill].blk = b;
			level_nspill++;
		}
		hunk_last->next = b;
		hunk_last = b;
		hunk_used = 0;
	}
	p = (byte *)(hunk_last + 1) + hunk_used;
	hunk_used += n;
	hunk_total += n;
	memset (p, 0, n);
	return p;
}

int Hunk_End (void)
{
	hunk_shrink (hunk_last, hunk_used);
	return (int)hunk_total;
}

void Hunk_Free (void *base)
{
	hunkblk_t *b = base ? (hunkblk_t *)base - 1 : NULL;
	int        i;

	if (b && level_owns (b))
	{
		/* карта в области уровня: по цепочке не ходить (после сброса области заголовки её кусков
		 * затёрты), отдать только записанные куски обычной кучи; место в области вернёт сброс */
		for (i = 0; i < level_nspill; )
			if (level_spill[i].area == base)
			{
				(q2_core->free) (level_spill[i].blk);
				level_spill[i] = level_spill[--level_nspill];
			}
			else
				i++;
		return;
	}
	while (b)
	{
		hunkblk_t *next = b->next;
		(q2_core->free) (b);   /* в скобках: иначе макрос free из compat.h подменит имя поля */
		b = next;
	}
}

/* ---------------------------------------------------------------- каталоги и поиск файлов */

void Sys_Mkdir (char *path)
{
	(void)path;                 /* файлы, записанные игрой, живут в памяти (compat.c): папок нет */
}

/* Поиск — только среди файлов в памяти (сохранения: «…/save/current/<уровень>.sav»). */
static char find_pattern[MAX_OSPATH];
static int  find_iter;

char *Sys_FindFirst (char *path, unsigned musthave, unsigned canthave)
{
	(void)musthave;
	(void)canthave;
	strncpy (find_pattern, path, sizeof(find_pattern) - 1);
	find_pattern[sizeof(find_pattern) - 1] = 0;
	find_iter = 0;
	return (char *)q2_mem_find (find_pattern, &find_iter);
}

char *Sys_FindNext (unsigned musthave, unsigned canthave)
{
	(void)musthave;
	(void)canthave;
	return (char *)q2_mem_find (find_pattern, &find_iter);
}

void Sys_FindClose (void)
{
	find_iter = 0;
}
