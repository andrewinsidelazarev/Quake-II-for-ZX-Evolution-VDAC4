/*
 * =====================================================================================================
 * bz_front.c — передний край «бомж-3D»: многоугольники программного Quake 2 → команды адаптера.
 * =====================================================================================================
 *
 * ЧТО ТАКОЕ «ПЕРЕДНИЙ КРАЙ»
 *   Видеоадаптер в ПЛИС умеет только закрашивать: ему говорят «вот многоугольник на экране, вот как по
 *   нему меняются глубина и координаты текстуры». Всё, что до этого — какие стены видны, как они
 *   повёрнуты к камере, где на экране их углы, какой свет на них лежит, — считает процессор. На VDAC4
 *   это будет ESP32-S3; здесь, на ПК, этот файл — прообраз того кода, и по нему видно, сколько у ESP
 *   работы и сколько байт уйдёт по LINK.
 *
 * КАК ЭТО ВСТРОЕНО В QUAKE 2
 *   ref_bz.dll — копия программного рендера id (ref_soft) с врезками (measure/patches/p04_ref_bz.py).
 *   Каждый кадр рисуется дважды:
 *     1) обычным программным рендером id — это эталон, с которым сравниваем;
 *     2) вторым проходом (bz_pass = 1): те же функции id обходят карту, отсекают невидимое, считают
 *        свет — но вместо рисования отдают многоугольники сюда (BZ_RenderFace, BZ_AliasTriangle,
 *        BZ_Particle, BZ_DrawPoly, BZ_AlphaSurfaces). 2D (строка состояния, меню) идёт через BZ_Draw*.
 *   Здесь многоугольник отсекается, проецируется на экран, и числа id переводятся в форматы адаптера
 *   (Q12.4, Q2.30, Q12.20, Q16.16 — gpu/SPEC.md). Текстуры и lightmap грузятся в память модели командой
 *   WRITE при первом обращении (как ESP будет грузить их по LINK).
 *   В конце кадра (BZ_EndFrame) поток команд выполняет C-модель (gpu/model/bomzh.c), её кадр
 *   сравнивается с кадром id точка в точку, счётчики пишутся в <out>.bz.csv, а при bz_trace 1 сам поток —
 *   в <out>.bzcmd (по нему стенд gpu/rtl/tb сверяет Verilog с моделью).
 *
 * ЗАЧЕМ БРАТЬ ГОТОВОЕ У ID
 *   Градиенты (D_CalcGradients), уровень mip (D_MipLevelForScale), свет (R_BuildLightMap), свет вершин
 *   моделей — уже выверенный код программного рендера. Если взять его, разница с эталоном — только
 *   от растеризатора адаптера, и её легко увидеть и объяснить.
 */
#include "r_local.h"
#include "bz_front.h"
#include "bomzh.h"
#include "bz_int.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* функции и данные id, которых нет в r_local.h */
image_t *R_TextureAnimation (mtexinfo_t *tex);
int      D_MipLevelForScale (float scale);
void     R_BuildLightMap (void);
void     R_PolygonCalculateGradients (void);
extern unsigned   blocklights[1024];
extern vec3_t     transformed_modelorg;
extern polydesc_t r_polydesc;
void BZ_InstallCrashHandler (const char *out_prefix);   /* bz_crash.c, только стенд */

int   bz_pass;
float bz_poly_alpha = 1;
int   bz_poly_textured = 1;
int   bz_raw;

#define BZ_MEM      (8u << 20)      /* SDRAM Tang: 8 МБ                                      */
#define BZ_NEAR     0.6f            /* ближняя плоскость: zi ≤ 1,67 влезает в Q2.30          */
#define TURB_SPEED  20              /* как SPEED и CYCLE в r_scan.c id                       */
#define TURB_CYCLE  128

/* ---------------------------------------------------------------- раскладка SDRAM модели */

static bz_mapa_t mapa;             /* тип — bz_int.h */

/* Хранилище в SDRAM ядра (SPEC 0.4, ниже — «хранилище»): растёт от конца памяти вниз, текстуры — от
 * mapa.tex вверх до store_lo. */
static uint32_t store_lo;
static uint32_t store_gen = 1;     /* растёт с каждой раскладкой SDRAM (layout): старые адреса хранилища — мусор */

static void layout (int w, int h)
{
	uint32_t fb = ((uint32_t)(((w + 15) & ~15) * h) + 0xFFF) & ~0xFFFu;   /* шаг строки — W вверх до 16 */
	uint32_t t;

	mapa.fb0 = 0;
	mapa.fb1 = fb;
	mapa.z = 2 * fb;
	t = (mapa.z + 2 * fb + 0xFFFF) & ~0xFFFFu;
	mapa.colormap = t;
	mapa.alphamap = t + 0x4000;
	mapa.pal = t + 0x14000;
	mapa.turb = t + 0x14400;
	mapa.solid = t + 0x14600;       /* 256 байт: байт i = i — «текстура 1×1» цвета i       */
	mapa.raw = t + 0x20000;         /* 256 КБ                                               */
	mapa.lm = mapa.raw + 0x40000;
	mapa.tex = mapa.lm + LM_SIZE;
	mapa.tex_end = BZ_MEM;
	store_lo = BZ_MEM;              /* хранилище пусто (модели перегружаются вместе с режимом экрана) */
	store_gen++;                    /* звуки, лежавшие в хранилище, — устарели (BZ_StoreGeneration) */
}

/* ---------------------------------------------------------------- буферы команд */

static cbuf_t  cb_main, cb_sky;     /* небо копится отдельно и идёт после непрозрачного мира */
static uint32_t sky_frame_words;    /* наибольшее число слов неба за кадр — для замера (bz.csv sky_bytes) */
static cbuf_t *cb = &cb_main;
static int     need_setup = 1;      /* SETUP ещё не ушёл (ставит BZ_SetMode, снимает BZ_EndFrame) */

/* определены в bz_hot.c (подключён ниже), а зовутся и раньше */
static inline void put (uint32_t v);
static texent_t   *tex_get (image_t *img, int mip, int w, int h, int lin);
static void        tex_page_mark (int p);

#ifdef BZ_ESP
int bz_link_send (const uint32_t *words, uint32_t count);
int bz_link_send_async (const uint32_t *words, uint32_t count);   /* bz_link.c: отдать задаче LINK */
int bz_link_wait_idle (void);                                      /* дождаться, что всё ушло       */
/* ESP: буфер кадра — две половины по 32 КБ (25.11.2026; было одна на 64 КБ): полная уходит в ядро задачей
 * LINK на ядре ESP 0 (bz_link.c), а игра тем временем пишет во вторую — отправка (~19 мс на кадр на плате)
 * идёт параллельно с подготовкой кадра. Порядок слов тот же: задача шлёт половины по одной, по очереди. */
#define CB_ESP_WORDS (1u << 13)
/* 26.11.2026: запасных буферов — два, а не один (плата board_log17: игра ждала LINK 4–7 мс за кадр). В отправке
 * до двух, третий заполняется; +32 КБ PSRAM. Не нашлось второго — работает с одним, как раньше. */
#define CB_SPARES 2
void bz_link_set_depth (int depth);   /* bz_link.c: сколько буферов может быть в отправке разом */
static uint32_t *cb_spare[CB_SPARES]; /* запасные: [0] ушёл раньше всех — его отправка уже кончилась */
static int       cb_nspare, cb_spare_failed;

/* Отдать накопленное задаче LINK и писать дальше в самый давний запасной буфер: в отправке — не больше
 * cb_nspare последних (bz_link_send_async ждёт, пока их станет меньше), а он отдан раньше них. */
static void cb_main_ship (void)
{
	uint32_t *t;
	int       i;

	if (!cb_main.n)
		return;
	while (cb_nspare < CB_SPARES && !cb_spare_failed)
	{
		if ((cb_spare[cb_nspare] = malloc (cb_main.cap * sizeof(uint32_t))) == NULL)
		{
			cb_spare_failed = 1;     /* PSRAM нет — больше не пробовать (до BZ_Shutdown) */
			break;
		}
		bz_link_wait_idle ();        /* новый запасной — в общий круг: прежние отправки — до конца */
		cb_nspare++;
		bz_link_set_depth (cb_nspare);
	}
	if (!cb_nspare)
	{
		/* запасного нет (PSRAM) — как раньше: отправить сейчас и ждать */
		if (bz_link_wait_idle () != 0 || bz_link_send (cb_main.w, cb_main.n) != 0)
			ri.Sys_Error (ERR_FATAL, "LINK to the Q2 core failed");
		cb_main.n = 0;
		return;
	}
	if (bz_link_send_async (cb_main.w, cb_main.n) != 0)
		ri.Sys_Error (ERR_FATAL, "LINK to the Q2 core failed");
	t = cb_main.w;
	cb_main.w = cb_spare[0];
	for (i = 0; i < cb_nspare - 1; i++)
		cb_spare[i] = cb_spare[i + 1];
	cb_spare[cb_nspare - 1] = t;
	cb_main.n = 0;
}

/* Буферы команд — кадра, неба и запасные (3 × 32 КБ + 4 КБ) — сразу после области уровня, до первой карты
 * (esp/main/q2_app.c; 26.11.2026). Плата board_log22: смена waste2 → base2 — «malloc: no memory for 65536 bytes»,
 * свободно 144 КБ кусками не больше 48 КБ: раньше буферы брались при первой отправке, а она — уже посреди
 * загрузки первой карты (полоса загрузки), и после смены карты они дробили свободное. Не в BZ_Init: та идёт
 * раньше области уровня, а область — всё свободное минус запас кучи; буферы из неё её урезали бы (board_log23:
 * 2272 → 2176 КБ, base2 не поместилась). Здесь они — из запаса кучи, как и раньше. */
void BZ_AllocCmdBuffers (void)
{
	if (!cb_main.w)
	{
		cb_main.cap = CB_ESP_WORDS;
		if ((cb_main.w = malloc (cb_main.cap * sizeof(uint32_t))) == NULL)
			ri.Sys_Error (ERR_FATAL, "bz: out of memory");
	}
	if (!cb_sky.w)
	{
		cb_sky.cap = 1024u;
		if ((cb_sky.w = malloc (cb_sky.cap * sizeof(uint32_t))) == NULL)
			ri.Sys_Error (ERR_FATAL, "bz: out of memory");
	}
	while (cb_nspare < CB_SPARES && !cb_spare_failed)
	{
		if ((cb_spare[cb_nspare] = malloc (cb_main.cap * sizeof(uint32_t))) == NULL)
			cb_spare_failed = 1;
		else
			cb_nspare++;
	}
	bz_link_set_depth (cb_nspare);
}

/* Памяти нет (q2_malloc, esp/main/compat.c — после сброса моделей прошлой карты): отдать один запасной буфер
 * кадра (32 КБ). Сначала — дождаться конца отправок: запасной мог
 * ещё уходить в ядро. Больше их не брать (до BZ_Shutdown): взятый заново посреди карты снова дробил бы кучу.
 * 1 — отдан, 0 — отдавать нечего. */
int BZ_ReleaseSpare (void)
{
	if (!cb_nspare)
		return 0;
	bz_link_wait_idle ();
	cb_nspare--;
	free (cb_spare[cb_nspare]);
	cb_spare[cb_nspare] = NULL;
	cb_spare_failed = 1;
	bz_link_set_depth (cb_nspare);
	return 1;
}
static void emit_setup (void);

/* ESP: SETUP (таблицы и раскладка SDRAM) — в ядро прямо сейчас, раньше накопленного в буфере кадра (так же
 * его ставит в начало BZ_EndFrame); потом — само накопленное. Буфер кадра остаётся новым и пустым. */
static void cb_setup_first (void)
{
	cbuf_t old;

	if (bz_link_wait_idle () != 0)       /* задача LINK — ничего в отправке */
		ri.Sys_Error (ERR_FATAL, "LINK to the Q2 core failed");
	old = cb_main;
	memset (&cb_main, 0, sizeof(cb_main));
	need_setup = 0;                 /* до emit_setup: его слова уходят частями тем же путём */
	emit_setup ();
	/* остаток SETUP — тем же путём, что и его начало (половина могла уже уйти задаче LINK), и только когда
	 * всё ушло — накопленное раньше: две отправки разом перемешали бы поток (25.11.2026) */
	cb_main_ship ();
	if (bz_link_wait_idle () != 0 || bz_link_send (old.w, old.n) != 0)
		ri.Sys_Error (ERR_FATAL, "LINK to the Q2 core failed");
	free (old.w);
}
#endif

static void put_full (uint32_t v)
{
#ifdef BZ_ESP
	/* ESP («диета», шаг 3): буфер кадра не растёт — полный уходит в очередь ядра сразу. Иначе первый кадр
	 * уровня (все видимые текстуры) и загрузка текстур при регистрации держали бы в PSRAM мегабайты слов.
	 * Порядок слов тот же; помнить TEX/LMAP можно и дальше — ядро их уже получило. SETUP должен идти
	 * раньше всех команд: если он ещё не ушёл (первый кадр не нарисован — Quake 2 сразу грузит уровень
	 * демо, Unreal NG 24.11), он уходит первым, как в BZ_EndFrame, а за ним — накопленное. */
	if (cb == &cb_main && cb->n == cb->cap && cb->cap)
	{
		if (need_setup)
			cb_setup_first ();
		else
			cb_main_ship ();
	}
#endif
	if (cb->n == cb->cap)
	{
#ifdef BZ_ESP
		/* небо — начиная с 4 КБ: на демо его до 280 байт за кадр (bz.csv sky_bytes, 24.11), а 64 КБ, как
		 * у буфера кадра, лежали бы в PSRAM зря (буфер неба растёт вдвое, если правда нужно больше) */
		cb->cap = cb->cap ? cb->cap * 2 : (cb == &cb_sky ? 1024u : CB_ESP_WORDS);
#else
		cb->cap = cb->cap ? cb->cap * 2 : 1u << 16;
#endif
		cb->w = realloc (cb->w, cb->cap * sizeof(uint32_t));
		if (!cb->w)
			ri.Sys_Error (ERR_FATAL, "bz: out of memory");
	}
	cb->w[cb->n++] = v;
}

/* ---------------------------------------------------------------- счётчики переднего края */

static bz_fstat_t fs;              /* тип — bz_int.h */

#ifndef BZ_ESP
static void store_trap (uint32_t addr, uint32_t bytes, const uint64_t *counter);   /* ниже, хранилище */
#endif

static void emit_write (uint32_t addr, const byte *data, uint32_t bytes, uint64_t *counter)
{
	uint32_t words = (bytes + 3) / 4, i;

#ifndef BZ_ESP
	store_trap (addr, 4u * words, counter);   /* ПК: запись не должна задеть живое хранилище */
#endif
	put (((uint32_t)BZ_WRITE << 24) | words);
	put (addr);
	for (i = 0; i < words; i++)
	{
		uint32_t v = 0, b;
		for (b = 0; b < 4; b++)
			if (i * 4 + b < bytes)
				v |= (uint32_t)data[i * 4 + b] << (8 * b);
		put (v);
	}
	*counter += 4u * words;
}

/* ---------------------------------------------------------------- текстуры в SDRAM */

/* Ближайшая степень двойки (в логарифме): 96 → 128, 320 → 256, 80 → 64. */
static int pow2_near (int v)
{
	int p = 1;
	while (p < v)
		p <<= 1;
	if (p > v && (double)v * v < (double)p * p / 2)
		p >>= 1;
	return p;
}

/*
 * Таблица текстур в SDRAM — открытая адресация: ключ ищется с места по свёртке номера и дальше подряд.
 * Ключ — номер картинки (image_t.bz_id: выдаётся один раз и не повторяется), мип-уровень, размер, вид и
 * свёртка имени FNV-1a (страховка). До «диеты», шаг 3 (24.11) ключом был адрес точек в памяти id; но
 * с BZ_TEX_RESIDENT точки после загрузки в ядро отдаются, и по их адресу легла бы другая картинка, а у
 * шкурок и спрайтов имя в ключе было одно на всех («skin», «poly»).
 * Таблица не должна заполниться целиком (поиск не найдёт пустого места): при 7/8 — сброс, как при
 * полной SDRAM (на демо 2017 кадров до этого не доходит — поток команд тот же).
 */
static texent_t texhash[TEXHASH];
static uint32_t tex_next, tex_count;    /* tex_next — верх занятого текстурами (отчёт, store_trap)      */

/*
 * СТРАНИЦЫ ТЕКСТУР (25.11.2026)
 *   Раньше текстуры клались подряд от mapa.tex, а когда место кончалось (или хранилище — кадры md2 и звуки —
 *   росло вниз), таблица сбрасывалась целиком: каждую нужную текстуру ESP перечитывала из pak (на плате — с
 *   карты Evo, запросом к Z80) и грузила снова. Со звуками в хранилище места стало меньше, и сбросы пошли
 *   посреди игры: на плате tex_get — 40 мс на кадр (медиана), всплески до 237.
 *   Теперь область текстур — страницы по TEX_PAGE от mapa.tex. Текстура ложится в текущую страницу; не
 *   влезла — следующая пустая, а пустых нет — освобождается страница, которой дольше всех не пользовались
 *   (tex_page_frame — номер кадра последнего обращения). Хранилищу, когда оно растёт вниз, отдаются только
 *   страницы, на которые оно наползает. Порядок команд кадра прежний: если освобождается страница, текстуры
 *   которой уже нарисованы в этом кадре, их TEX стоят в потоке раньше WRITE новой текстуры — ядро рисует по
 *   порядку, картинка та же (как и при прежнем сбросе всего).
 *   Удаление из таблицы (открытая адресация, линейный поиск) — со сдвигом хвоста цепочки назад (Кнут, алг. R).
 */
static uint32_t tex_fill[TEX_PAGES];       /* занято байт в странице                                   */
static uint32_t tex_page_frame[TEX_PAGES]; /* кадр последнего обращения к странице                     */
static int      tex_cur = -1;              /* текущая страница (-1 — нет)                              */
static uint32_t tex_frame;                 /* номер кадра (BZ_EndFrame)                                */

static uint32_t name_fnv (const char *s)
{
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

image_t *bz_alias_skin;            /* r_alias.c (p04): шкурка модели, что рисуется              */
image_t *bz_poly_image;            /* r_sprite.c (p04): картинка спрайта для BZ_DrawPoly        */
static int image_serial;           /* последний выданный номер картинки                         */

/* Номер картинки — при первой встрече; освобождённое id место таблицы картинок (memset) получит новый. */
static int image_id (image_t *img)
{
	if (!img->bz_id)
	{
		img->bz_id = ++image_serial;
		/* 26.11.2026: хэш имени — тут же, один раз (раньше tex_get считал его на каждой грани). Имя у места
		 * картинки не меняется, пока номер выдан: R_FindFreeImage даёт только чистое (memset) место. */
		img->bz_name_hash = name_fnv (img->name);
	}
	return img->bz_id;
}

static void tex_flush (void)
{
	memset (texhash, 0, sizeof(texhash));
	memset (tex_fill, 0, sizeof(tex_fill));
	tex_cur = -1;
	tex_next = mapa.tex;
	tex_count = 0;
	fs.tex_flushes++;
}

static uint32_t tex_page_base (int p)
{
	return mapa.tex + (uint32_t)p * TEX_PAGE;
}

/* Страница целиком ниже хранилища — её можно занимать */
static int tex_page_ok (int p)
{
	return p >= 0 && p < TEX_PAGES && tex_page_base (p) + TEX_PAGE <= store_lo;
}

static uint32_t tex_home (const texent_t *e)
{
	return ((uint32_t)e->id * 2654435761u + (unsigned)(e->mip * 2 + e->lin)) & (TEXHASH - 1);
}

/* Убрать запись i: хвост цепочки сдвигается назад, чтобы поиск не оборвался на дыре */
static void tex_del (uint32_t i)
{
	uint32_t j = i;

	for (;;)
	{
		uint32_t k;

		j = (j + 1) & (TEXHASH - 1);
		if (!texhash[j].id)
			break;
		k = tex_home (&texhash[j]);
		if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
			continue;               /* её место — между i и j: остаётся */
		texhash[i] = texhash[j];
		i = j;
	}
	texhash[i].id = 0;
	tex_count--;
}

/* Освободить страницу p: её текстуры забыты, при нужде загрузятся снова */
static void tex_evict (int p)
{
	uint32_t lo = tex_page_base (p), hi = lo + TEX_PAGE, i, top = mapa.tex;
	int      q;

	for (i = 0; i < TEXHASH; )
		if (texhash[i].id && texhash[i].addr >= lo && texhash[i].addr < hi)
			tex_del (i);            /* на место i мог сдвинуться следующий — проверить его */
		else
			i++;
	tex_fill[p] = 0;
	tex_page_frame[p] = 0;
	if (tex_cur == p)
		tex_cur = -1;
	for (q = 0; q < TEX_PAGES; q++)
		if (tex_fill[q] && tex_page_base (q) + tex_fill[q] > top)
			top = tex_page_base (q) + tex_fill[q];
	tex_next = top;
	fs.tex_flushes++;
}

/* Страница для новой текстуры: пустая, иначе — давно не нужная (освобождается). -1 — ни одной (не бывает:
 * хранилище оставляет текстурам STORE_RESERVE — две страницы). */
static int tex_page_pick (void)
{
	int p, best = -1;

	for (p = 0; p < TEX_PAGES; p++)
		if (tex_page_ok (p) && !tex_fill[p])
			return p;
	for (p = 0; p < TEX_PAGES; p++)
		if (tex_page_ok (p) && (best < 0 || tex_page_frame[p] < tex_page_frame[best]))
			best = p;
	if (best >= 0)
		tex_evict (best);
	return best;
}

/* Таблица почти полна: освободить занятую страницу, к которой дольше всех не обращались */
static void tex_evict_lru (void)
{
	int p, best = -1;

	for (p = 0; p < TEX_PAGES; p++)
		if (tex_fill[p] && (best < 0 || tex_page_frame[p] < tex_page_frame[best]))
			best = p;
	if (best >= 0)
		tex_evict (best);
	else
		tex_flush ();
}

/* для замера на ПК (swimp_bz.c): сколько записей таблицы текстур занято сейчас */
int BZ_TexCount (void)
{
	return (int)tex_count;
}

/* Найти текстуру в SDRAM модели или загрузить её. lin = 1 — строками (картинки, шкурки,
 * спрайты); lin = 0 — плитками 8×8 со степенью двойки (стены, вода, небо), чужой размер
 * растягивается до ближайшей степени двойки (у Quake 2 таких 33 стены из 2188).
 * Точки — мип-уровня mip картинки img (w×h — сколько их брать строками ширины w); если их уже отдали
 * (BZ_TEX_RESIDENT), они читаются заново из файла картинки (R_EspReloadPixels). */
/* 26.11.2026: поиск в таблице — tex_get (bz_hot.c, горячий модуль); здесь — его продолжение: нет в таблице —
 * занять место и загрузить (поиск повторяется — он дёшев, а загрузка редка). */
static texent_t *tex_load_body (image_t *img, int mip, int w, int h, int lin)
{
	int       id = image_id (img);
	uint32_t  hsh = ((uint32_t)id * 2654435761u + (unsigned)(mip * 2 + lin)) & (TEXHASH - 1);
	uint32_t  nh = img->bz_name_hash;
	texent_t *e;
	byte     *buf, *temp = NULL;
	const byte *pix;
	uint32_t  size, x, y;

	for (;; hsh = (hsh + 1) & (TEXHASH - 1))
	{
		e = &texhash[hsh];
		if (!e->id)
			break;
		if (e->id == id && e->mip == mip && e->w == w && e->h == h && e->lin == lin && e->name_hash == nh)
		{
			tex_page_mark ((int)((e->addr - mapa.tex) / TEX_PAGE));
			return e;
		}
	}
	{
		int16_t w2 = (int16_t)(lin ? w : pow2_near (w)), h2 = (int16_t)(lin ? h : pow2_near (h));
		uint32_t a;

		size = ((uint32_t)(w2 * h2) + 63) & ~63u;
		if (size > TEX_PAGE)
			ri.Sys_Error (ERR_FATAL, "bz: %s is %u bytes — bigger than a texture page", img->name, (unsigned)size);
		/* место: таблица почти полна — освободить давно не нужную страницу; текущая страница не годится
		 * (полна или под хранилищем) — другую. Таблица при этом могла сдвинуться — искать место заново. */
		if (tex_count >= TEXHASH / 8 * 7)
		{
			tex_evict_lru ();
			return tex_get (img, mip, w, h, lin);
		}
		if (!tex_page_ok (tex_cur) || tex_fill[tex_cur] + size > TEX_PAGE)
		{
			tex_cur = tex_page_pick ();
			if (tex_cur < 0)
				ri.Sys_Error (ERR_FATAL, "bz: no texture page left (store_lo %08x)", (unsigned)store_lo);
			return tex_get (img, mip, w, h, lin);
		}
		a = tex_page_base (tex_cur) + tex_fill[tex_cur];
		tex_fill[tex_cur] += size;
		tex_page_mark (tex_cur);
		if (a + size > tex_next)
			tex_next = a + size;
		e->w2 = w2;
		e->h2 = h2;
		e->addr = a;
	}
	e->id = id;
	e->mip = (uint8_t)mip;
	e->w = (int16_t)w;
	e->h = (int16_t)h;
	e->lin = (uint8_t)lin;
	e->name_hash = nh;
	tex_count++;

	pix = img->pixels[mip];
	if (!pix)
		pix = temp = R_EspReloadPixels (img, mip);   /* точки отданы ядру: заново из файла */
	buf = malloc (size);
	if (!pix)
	{
		/* файла нет или он другой — текстура будет чёрной, но память ядра не мусор */
		ri.Con_Printf (PRINT_ALL, "bz: %s: no pixels to load\n", img->name);
		memset (buf, 0, size);
	}
	else if (lin)
		memcpy (buf, pix, w * h);
	else
		for (y = 0; y < (uint32_t)e->h2; y++)
			for (x = 0; x < (uint32_t)e->w2; x++)
			{
				/* ближайший тексель исходника; при степени двойки — он сам */
				byte c = pix[(y * h / e->h2) * w + x * w / e->w2];
				uint32_t a;
				if (e->w2 >= 8 && e->h2 >= 8)
					a = (((y >> 3) * (e->w2 >> 3) + (x >> 3)) << 6) + ((y & 7) << 3) + (x & 7);
				else
					a = y * e->w2 + x;
				buf[a] = c;
			}
	{
		/* загрузка — в основной поток кадра, даже для грани неба (25.11.2026, плата: текстура неба до 64 КБ
		 * ложилась в буфер неба, тот рос вдвое — «realloc: no memory for 65536 bytes» посреди игры). Небо
		 * рисуется после мира, загрузка уходит раньше — страница неба до конца кадра не отдаётся (tex_page_mark). */
		cbuf_t *keep = cb;

		cb = &cb_main;
		emit_write (e->addr, buf, (uint32_t)(e->w2 * e->h2), &fs.tex_bytes);
		cb = keep;
	}
	free (buf);
	if (temp)
		free (temp);
	fs.tex_uploads++;
	return e;
}

/* С меткой Q2M_F_TEX (26.11.2026, BZ_PROF_FINE): загрузка текстуры в SDRAM ядра */
static texent_t *tex_load (image_t *img, int mip, int w, int h, int lin)
{
	texent_t *e;
	BZ_PROF_BEGIN (Q2M_F_TEX);
	e = tex_load_body (img, mip, w, h, lin);
	BZ_PROF_END (Q2M_F_TEX);
	return e;
}

/*
 * BZ_TEX_RESIDENT («диета», шаг 3, 24.11; Unreal NG: на загрузке base2 картинки держали 1,2 МБ PSRAM, и
 * её не хватило): картинка загружена — её текстуры сразу в SDRAM ядра, точки на ESP — прочь. ESP сама
 * текстуры не рисует, точки ей нужны только для загрузки в ядро. Загружаются виды, которые точно
 * понадобятся: у стен и неба — все мип-уровни плитками, у шкурок, спрайтов и картинок — строками.
 * Другой вид (подложка TileClear — плитками) или повтор после сброса таблицы — R_EspReloadPixels.
 * Пока раскладки SDRAM нет (режим экрана ещё не задан), точки остаются — загрузка при рисовании.
 */
void BZ_ImageLoaded (image_t *img)
{
#ifdef BZ_TEX_RESIDENT
	int m, n = 0;

	while (n < 4 && img->pixels[n])
		n++;
	img->bz_mips = n;
	if (!n || !BZ_Enabled () || !mapa.tex_end)
		return;
	if (img->type == it_wall || img->type == it_sky)
		for (m = 0; m < n; m++)
			tex_get (img, m, img->width >> m, img->height >> m, 0);
	else
		tex_get (img, 0, img->width, img->height, 1);
	free (img->pixels[0]);       /* остальные мип-уровни — в том же блоке (R_LoadWal) */
	for (m = 0; m < 4; m++)
		img->pixels[m] = NULL;
#else
	(void)img;
#endif
}

/* ---------------------------------------------------------------- хранилище в SDRAM ядра */

/*
 * ЗАЧЕМ (24.11.2026, «Да, SDRAM Tang»)
 *   PSRAM у ESP — 8 МБ на всё, и уровень Quake 2 в неё не влезал: одни кадры анимации моделей md2 —
 *   0,2–1,3 МБ на уровень (88 % объёма md2). А нужны рендеру из них единицы в кадр: две позы каждой
 *   модели в кадре. Поэтому кадры md2 лежат в SDRAM ядра Tang (там 8 МБ, и до верха её не доходят даже
 *   текстуры самых тяжёлых карт), а на ESP — кольцо недавних кадров (BZ_Md2Frame, ниже).
 *
 * КАК
 *   - Место: хранилище растёт от конца SDRAM вниз (store_lo), текстуры — снизу вверх до store_lo. Не
 *     хватило места текстуре — сброс таблицы текстур, как раньше (tex_get); хранилищу — модель остаётся
 *     в PSRAM, как раньше (BZ_StoreAlloc вернёт 0). Освобождённый кусок — в список свободных; кусок у
 *     нижней границы поднимает её.
 *   - Запись: команда WRITE в тот же буфер кадра, что и загрузки текстур, — по порядку с ними. Раньше
 *     (первая проба 24.11) хранилище писалось в ядро сразу, мимо буфера: после сброса таблицы текстур
 *     их старые, ещё не отправленные загрузки ложились поверх свежих кадров md2 (mapstats на ПК: без
 *     оружия в руке, чужой пол). После загрузки модели — MARK (BZ_StoreMark), тоже в буфер.
 *   - Чтение: ESP — если в буфере кадра ещё лежат неотправленные записи хранилища, буфер уходит в ядро
 *     сейчас (как при заполнении), ESP ждёт MARK и читает окно (bz_link_read: RD_ADDR/RD_LEN, RD_BUSY,
 *     слова окна). ПК — из теневой копии хранилища (store_shadow): модель исполняет буфер лишь в конце
 *     кадра. Что в памяти модели те же байты, проверяет store_check после каждого кадра — затёртое
 *     хранилище остановит прогон.
 */
#define STORE_ALIGN   32u            /* пачка SDRAM: копия в окно читает пачками по 32 байта */
#define STORE_RESERVE (512u << 10)   /* текстурам оставлять хотя бы столько (две страницы TEX_PAGE) */
#define STORE_FREE_N  512          /* с 25.11 в хранилище и звуки — сотни кусков на уровень */

#ifdef BZ_ESP
int bz_link_can_read (void);
int bz_link_wait_mark (uint32_t mark);
int bz_link_read (uint32_t addr, void *dst, uint32_t bytes);
#else
static bz_t *model;               /* модель адаптера (объявлена и ниже — у C это одно и то же) */
#endif

static struct { uint32_t addr, size; } store_free[STORE_FREE_N];   /* свободные куски внутри хранилища */
static int      store_nfree;
static uint32_t store_mark;        /* номер последнего MARK после загрузки в хранилище */
static uint32_t store_bytes;       /* сколько байт хранилища занято (для отчёта о памяти ESP) */
static int      store_unsent;      /* ESP: в буфере кадра есть записи хранилища, ядру ещё не отправленные */
static int      store_dirty;       /* ESP: после последнего MARK были записи в хранилище */
static int      store_mark_seen;   /* ESP: ядро подтвердило последний MARK, и записей после него не было */
static int      store_writing;     /* 1 — emit_write пишет само хранилище (BZ_StoreWrite) */

#ifndef BZ_ESP
/* ПК: теневая копия хранилища (читает store_read) и занятые куски — для сверки с памятью модели */
#define STORE_SPANS 4096
static byte    *store_shadow;
static struct { uint32_t addr, size; } store_span[STORE_SPANS];
static int      store_nspan;
#endif

/* Можно ли класть данные в SDRAM ядра: раскладка уже есть, а ядро (ESP) умеет читать SDRAM (SPEC 0.4). */
int BZ_StoreAvailable (void)
{
	static cvar_t *store_cv;

	if (!BZ_Enabled () || !mapa.tex_end)
		return 0;
	if (!store_cv)
		store_cv = ri.Cvar_Get ("bz_store", "1", 0);   /* 0 — кадры md2 в PSRAM, как до 24.11 (сверка) */
	if (!store_cv->value)
		return 0;
#ifdef BZ_ESP
	return bz_link_can_read ();
#else
	return model != NULL;
#endif
}

uint32_t BZ_StoreBytes (void)
{
	return store_bytes;
}

/* Сколько байт держат буферы команд кадра и неба (для отчёта о памяти ESP; буфер неба растёт вдвое,
 * когда кадр копит больше неба, чем влезает, и назад не сжимается). */
uint32_t BZ_CmdBufBytes (void)
{
#ifdef BZ_ESP
	return (cb_main.cap * (1u + (uint32_t)cb_nspare) + cb_sky.cap) * (uint32_t)sizeof(uint32_t);
#else
	return (cb_main.cap + cb_sky.cap) * (uint32_t)sizeof(uint32_t);
#endif
}

/* ПК: занятые куски хранилища — для сверки теневой копии с памятью модели (store_check). */
static void store_span_add (uint32_t addr, uint32_t size)
{
#ifndef BZ_ESP
	if (store_nspan < STORE_SPANS)
	{
		store_span[store_nspan].addr = addr;
		store_span[store_nspan].size = size;
		store_nspan++;
	}
#else
	(void)addr; (void)size;
#endif
}

static void store_span_del (uint32_t addr)
{
#ifndef BZ_ESP
	int i;
	for (i = 0; i < store_nspan; i++)
		if (store_span[i].addr == addr)
		{
			store_span[i] = store_span[--store_nspan];
			return;
		}
#else
	(void)addr;
#endif
}

/* Кусок хранилища на bytes байт (адрес кратен 32) или 0 — места нет. */
uint32_t BZ_StoreAlloc (uint32_t bytes)
{
	uint32_t size = (bytes + STORE_ALIGN - 1) & ~(STORE_ALIGN - 1);
	int      i;

	if (!size || !BZ_StoreAvailable ())
		return 0;
	for (i = 0; i < store_nfree; i++)   /* сначала — свободный кусок внутри */
		if (store_free[i].size >= size)
		{
			uint32_t a = store_free[i].addr;
			store_free[i].addr += size;
			store_free[i].size -= size;
			if (!store_free[i].size)
				store_free[i] = store_free[--store_nfree];
			store_bytes += size;
			store_span_add (a, bytes);     /* сверка — только записанных байт, не хвоста округления */
			return a;
		}
	if (store_lo < size || store_lo - size < mapa.tex + STORE_RESERVE)
		return 0;                       /* текстурам остаётся хотя бы STORE_RESERVE — иначе модель остаётся в PSRAM */
	store_lo -= size;
	/* страницы текстур, на которые наползло хранилище, — освободить (25.11.2026: раньше сбрасывались все) */
	{
		int p;
		for (p = 0; p < TEX_PAGES; p++)
			if (tex_fill[p] && !tex_page_ok (p))
				tex_evict (p);
		if (!tex_page_ok (tex_cur))
			tex_cur = -1;
	}
	store_bytes += size;
	store_span_add (store_lo, bytes);
	return store_lo;
}

/* Вернуть кусок (адрес и длина — как у BZ_StoreAlloc). Соседние свободные куски сливаются; свободный
 * кусок у нижней границы поднимает её — это место снова могут взять текстуры. */
void BZ_StoreFree (uint32_t addr, uint32_t bytes)
{
	uint32_t size = (bytes + STORE_ALIGN - 1) & ~(STORE_ALIGN - 1);
	int      i, merged;

	if (!addr || !size)
		return;
	store_bytes -= size;
	store_span_del (addr);
	do
	{
		merged = 0;
		for (i = 0; i < store_nfree; i++)
			if (store_free[i].addr + store_free[i].size == addr || addr + size == store_free[i].addr)
			{
				if (store_free[i].addr < addr)
					addr = store_free[i].addr;
				size += store_free[i].size;
				store_free[i] = store_free[--store_nfree];
				merged = 1;
				break;
			}
	} while (merged);
	if (addr == store_lo)
	{
		store_lo += size;
		return;
	}
	if (store_nfree < STORE_FREE_N)
	{
		store_free[store_nfree].addr = addr;
		store_free[store_nfree].size = size;
		store_nfree++;
	}
	/* список полон — кусок потерян до смены режима экрана (на уровне моделей до ~60 и звуков до ~300 — 512 с запасом) */
}

/* Записать bytes байт в хранилище: команда WRITE в буфер кадра — по порядку с загрузками текстур. */
void BZ_StoreWrite (uint32_t addr, const void *data, uint32_t bytes)
{
	cbuf_t *save = cb;

	cb = &cb_main;                  /* в регистрацию это и так он; небо (cb_sky) — не место загрузкам */
	store_writing = 1;
	emit_write (addr, (const byte *)data, bytes, &fs.other_bytes);
	store_writing = 0;
	cb = save;
#ifdef BZ_ESP
	store_unsent = 1;
	store_dirty = 1;
	store_mark_seen = 0;
#else
	if (!store_shadow)
		store_shadow = calloc (1, BZ_MEM);
	if (store_shadow && addr + bytes <= BZ_MEM)
		memcpy (store_shadow + addr, data, bytes);
#endif
}

/* После загрузки: MARK — ядро скажет, когда всё записанное уже в SDRAM. */
void BZ_StoreMark (void)
{
	cbuf_t *save = cb;

	cb = &cb_main;
	put ((uint32_t)BZ_MARK << 24);
	put (++store_mark);
	cb = save;
#ifdef BZ_ESP
	store_unsent = 1;
	store_dirty = 0;
	store_mark_seen = 0;
#endif
}

#ifdef BZ_ESP
/* ESP: всё, что накоплено в буфере кадра, — в ядро сейчас (SETUP, если он ещё не ушёл, — первым): так же,
 * как put отправляет полный буфер. Помнить TEX/LMAP можно и дальше — ядро их уже получило. */
static void cb_send_now (void)
{
	if (need_setup)
		cb_setup_first ();
	else
		cb_main_ship ();
	/* зовущий будет читать SDRAM ядра — всё отданное должно уйти */
	if (bz_link_wait_idle () != 0)
		ri.Sys_Error (ERR_FATAL, "LINK to the Q2 core failed");
}
#endif

/* Прочитать bytes байт хранилища (адрес кратен 4). */
static void store_read (uint32_t addr, void *dst, uint32_t bytes)
{
#ifdef BZ_ESP
	/* 26.11.2026: ядро уже подтвердило последний MARK, и записей в хранилище с тех пор не было — читать сразу,
	 * не дожидаясь, пока задача LINK отправит отданную половину буфера кадра (плата, board_log13: игра ждала
	 * LINK ~11 мс за кадр — в основном здесь, на каждом кадре md2 и куске звука). Чтение SDRAM (RD_ADDR,
	 * RD_LEN, STATUS, окно) и отправка задачи (FIFO_FREE, FIFO) — разные регистры ядра; каждый вызов LINK
	 * прошивка делает под своим замком. */
	if (!store_mark_seen)
	{
		if (store_unsent)           /* записи хранилища ещё в буфере кадра — отправить, иначе MARK не придёт */
		{
			cb_send_now ();
			store_unsent = 0;
		}
		else if (bz_link_wait_idle () != 0)   /* отданные задаче LINK записи (и MARK) — до ядра */
			ri.Sys_Error (ERR_FATAL, "LINK to the Q2 core failed");
		if (bz_link_wait_mark (store_mark) != 0)
			ri.Sys_Error (ERR_FATAL, "LINK: SDRAM read of the Q2 core failed");
		store_mark_seen = !store_dirty;
	}
	if (bz_link_read (addr, dst, bytes) != 0)
		ri.Sys_Error (ERR_FATAL, "LINK: SDRAM read of the Q2 core failed");
#else
	/* модель исполнит буфер кадра лишь в конце кадра — до того верна теневая копия */
	if (!store_shadow || addr + bytes > BZ_MEM)
		ri.Sys_Error (ERR_FATAL, "bz: store read %08x+%u without a shadow", (unsigned)addr, (unsigned)bytes);
	memcpy (dst, store_shadow + addr, bytes);
#endif
}

/* Для звука (snd_mix.c, 25.11.2026): кусок хранилища — адрес кратен 4, как у store_read. */
void BZ_StoreRead (uint32_t addr, void *dst, uint32_t bytes)
{
	store_read (addr, dst, bytes);
}

/* Номер раскладки SDRAM: звук, положенный в хранилище при другой раскладке, надо перечитать. */
uint32_t BZ_StoreGeneration (void)
{
	return store_gen;
}

#ifndef BZ_ESP
/* ПК: любая запись в SDRAM, кроме самого хранилища, задевшая живой кусок хранилища, — стоп с адресами и
 * видом записи (текстура, lightmap, прочее): так видно, кто затирает кадры md2. */
static void store_trap (uint32_t addr, uint32_t bytes, const uint64_t *counter)
{
	int i;

	if (store_writing)
		return;
	for (i = 0; i < store_nspan; i++)
		if (addr < store_span[i].addr + store_span[i].size && store_span[i].addr < addr + bytes)
			ri.Sys_Error (ERR_FATAL, "bz: %s write %08x+%u hits store %08x+%u (store_lo %08x, tex_next %08x)",
			              counter == &fs.tex_bytes ? "texture" : counter == &fs.lm_bytes ? "lightmap" : "other",
			              (unsigned)addr, (unsigned)bytes, (unsigned)store_span[i].addr,
			              (unsigned)store_span[i].size, (unsigned)store_lo, (unsigned)tex_next);
}

/* ПК, после каждого исполненного кадра: в памяти модели (то есть в SDRAM ядра) хранилище цело — его не
 * затёрли текстуры, lightmap или кадры, и MARK дошёл. Иначе ESP прочитала бы чужое — стоп. */
static void store_check (void)
{
	int i;

	if (!store_nspan || !store_shadow)
		return;
	if (bz_mark (model) != store_mark)
		ri.Sys_Error (ERR_FATAL, "bz: core MARK %u, expected %u", (unsigned)bz_mark (model), (unsigned)store_mark);
	for (i = 0; i < store_nspan; i++)
		if (memcmp (bz_mem (model) + store_span[i].addr, store_shadow + store_span[i].addr, store_span[i].size))
			ri.Sys_Error (ERR_FATAL, "bz: store at %08x (%u bytes) overwritten",
			              (unsigned)store_span[i].addr, (unsigned)store_span[i].size);
}
#endif

/* ---------------------------------------------------------------- кольцо кадров md2 */

/*
 * Кадры md2 из хранилища — в кольцо 128 КБ (PSRAM ESP): новый кадр ложится за последним, старые
 * затираются по кругу. Замер на ПК (24.11, трассы демо и боёв по 90 с на сложности 3, claude-scripts
 * md2fifo_sim.py): кольцо 256 КБ берёт с LINK в среднем до 0,6 КБ на кадр игры, p99 — до 5,3 КБ, в худшем
 * кадре (вошла новая модель) — до 12 КБ; LRU лучше лишь чуть-чуть, а кольцо не дробит кучу.
 * 128 КБ (плата 24.11: после загрузки base2 свободной PSRAM было 368 КБ, а кольцо 256 КБ одним куском не
 * нашлось): в среднем до 0,9 КБ на кадр, p99 — до 7,3 КБ, худший кадр — те же 12 КБ. Кольцо берётся при
 * установке режима (BZ_SetMode), пока куча ещё не раздроблена картой, а не при первом кадре.
 * Кадр ищется по таблице (номер модели, кадр); запись верна, пока кольцо не ушло дальше неё почти на круг
 * (pos — счётчик записанных в кольцо байт, растёт всегда, 64 бита — не переполнится). «Почти» — запас
 * MD2_GUARD: найденный кадр должен пережить следующий промах (рендер берёт два кадра модели подряд —
 * позу и прошлую позу, — и запись второго, с пропуском хвоста кольца, не длиннее двух наибольших кадров
 * md2: 40 + 2048 вершин × 4 байта).
 */
#define MD2_RING  (128u << 10)
#define MD2_GUARD (2u * (40u + 2048u * 4u) + 64u)
#define MD2_HASH  2048

static byte    *md2_ring;
static uint64_t md2_pos;            /* сколько байт записано в кольцо за всё время */
static struct { uint32_t id; int frame; uint64_t pos; } md2_tab[MD2_HASH];
static uint64_t md2_misses, md2_miss_bytes;   /* для отчёта */

/* Кадр frame модели id (номер — md2_load, p06), лежащий в хранилище по адресу addr, size байт. */
const void *BZ_Md2Frame (uint32_t id, int frame, uint32_t addr, int size)
{
	uint32_t h = (id * 2654435761u + (uint32_t)frame * 40503u) & (MD2_HASH - 1);
	uint32_t off;

	if (md2_tab[h].id == id && md2_tab[h].frame == frame && md2_pos - md2_tab[h].pos <= MD2_RING - MD2_GUARD)
		return md2_ring + md2_tab[h].pos % MD2_RING;
	if (!md2_ring)
		md2_ring = malloc (MD2_RING);
	off = (uint32_t)(md2_pos % MD2_RING);
	if (off + (uint32_t)size > MD2_RING)  /* в конце кольца не влезает — с начала */
	{
		md2_pos += MD2_RING - off;
		off = 0;
	}
	store_read (addr, md2_ring + off, (uint32_t)size);
	md2_tab[h].id = id;
	md2_tab[h].frame = frame;
	md2_tab[h].pos = md2_pos;
	md2_pos += ((uint32_t)size + 3) & ~3u;
	md2_misses++;
	md2_miss_bytes += (uint32_t)size;
	return md2_ring + off;
}

/* Состояние TEX/LMAP буфера больше не известно (новый кадр, дописан чужой буфер). */
static void cb_forget (cbuf_t *b)
{
	b->tex_ok = 0;
	b->lm_ok = 0;
}

/* ---------------------------------------------------------------- lightmap */

/*
 * Таблица lightmap — прямого отображения: у каждой грани одно место (номер грани по модулю размера).
 * Две грани на одном месте вытесняют друг друга: lightmap собирается и пишется в SDRAM заново — тот же,
 * так что кадр от размера таблицы не меняется, меняется только поток LINK. Номер грани — её место в
 * массиве граней карты (у моделей-кистей грани в том же массиве), а не адрес в памяти: так поток
 * одинаков на ПК и на ESP. «Диета» 23.11: 16384 записи (917 КБ) → BZ_LMHASH (по умолчанию 4096).
 */
static lment_t  lmhash[LMHASH];      /* LMHASH — bz_int.h */
static uint64_t lm_total;           /* сколько байт записано в кольцо за всё время          */

static uint32_t lm_slot (const msurface_t *surf);     /* bz_hot.c */

/* Lightmap грани для адаптера: байт на клетку, L = t >> 6, t — свет программного Quake после
 * R_BuildLightMap (стили, вспышки). Пересобирается, когда меняются стили или на грани есть
 * динамический свет (так же решает кэш поверхностей id). Если текстуру растянули до степени
 * двойки, клетки пересчитываются на ту же сетку (билинейно), чтобы адаптер мог брать клетку
 * сдвигом.
 * 26.11.2026: проверка «запись годна» — lm_get (bz_hot.c, горячий модуль); сюда — только когда нужно собрать
 * (проверка повторяется — она дешева). */
static lment_t *lm_build_body (msurface_t *surf, bzr fx, bzr fy)
{
	lment_t *e = &lmhash[lm_slot (surf)];
	int      adj[MAXLIGHTMAPS], i, smax, tmax, sw = (int)(fx * 1024 + 0.5), sh = (int)(fy * 1024 + 0.5);
	int      dlit = surf->dlightframe == r_framecount;
	/* клеток у грани id не больше 1024 (blocklights[1024] у R_BuildLightMap); растяжение до степени
	 * двойки — не больше чем в 2 раза по стороне: 4 КБ хватает (было по 64 КБ — «диета» 23.11) */
	static byte lux[4096], tmp[4096];
	uint32_t size;

	int      fits = 1;              /* все яркости влезают в 16 бит записи — иначе не кэшировать */

	for (i = 0; i < MAXLIGHTMAPS; i++)
	{
		adj[i] = r_newrefdef.lightstyles[surf->styles[i]].white * 128;
		if (adj[i] < 0 || adj[i] > 65535)
			fits = 0;
	}
	if (e->surf == surf && fits && !e->dlit && !dlit && e->sw == sw && e->sh == sh &&
	    e->adj[0] == adj[0] && e->adj[1] == adj[1] && e->adj[2] == adj[2] && e->adj[3] == adj[3] &&
	    lm_total - (((uint64_t)e->pos_hi << 32) | e->pos_lo) <= LM_SIZE)
		return e;

	r_drawsurf.surf = surf;
	for (i = 0; i < MAXLIGHTMAPS; i++)
		r_drawsurf.lightadj[i] = adj[i];
	R_BuildLightMap ();
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	for (i = 0; i < smax * tmax; i++)
		lux[i] = (byte)(blocklights[i] >> 6 > 255 ? 255 : blocklights[i] >> 6);

	e->stride = (uint16_t)smax;
	e->rows = (uint16_t)tmax;
	if (sw != 1024 || sh != 1024)
	{
		int s2 = (int)ceil ((smax - 1) * fx) + 1, t2 = (int)ceil ((tmax - 1) * fy) + 1, x, y;
		if (s2 > 1024) s2 = 1024;
		if (t2 * s2 > (int)sizeof(tmp)) t2 = sizeof(tmp) / s2;
		for (y = 0; y < t2; y++)
			for (x = 0; x < s2; x++)
			{
				double u = x / fx, v = y / fy;
				int    iu = (int)u, iv = (int)v, iu1, iv1;
				double fu, fv;
				if (iu > smax - 1) iu = smax - 1;
				if (iv > tmax - 1) iv = tmax - 1;
				iu1 = iu + 1 < smax ? iu + 1 : smax - 1;
				iv1 = iv + 1 < tmax ? iv + 1 : tmax - 1;
				fu = u - iu;
				fv = v - iv;
				if (fu > 1) fu = 1;
				if (fv > 1) fv = 1;
				tmp[y * s2 + x] = (byte)floor ((lux[iv * smax + iu] * (1 - fu) + lux[iv * smax + iu1] * fu) * (1 - fv) +
				                               (lux[iv1 * smax + iu] * (1 - fu) + lux[iv1 * smax + iu1] * fu) * fv + 0.5);
			}
		memcpy (lux, tmp, s2 * t2);
		e->stride = (uint16_t)s2;
		e->rows = (uint16_t)t2;
	}
	size = ((uint32_t)(e->stride * e->rows) + 3) & ~3u;
	if (lm_total % LM_SIZE + size > LM_SIZE)
		lm_total += LM_SIZE - lm_total % LM_SIZE;          /* не рвать lightmap на краю кольца */
	e->surf = surf;
	for (i = 0; i < MAXLIGHTMAPS; i++)
		e->adj[i] = (uint16_t)adj[i];
	e->dlit = (uint8_t)(dlit || !fits);     /* не влезло — запись не годится для следующего кадра */
	e->sw = (uint16_t)sw;
	e->sh = (uint16_t)sh;
	e->pos_lo = (uint32_t)lm_total;
	e->pos_hi = (uint32_t)(lm_total >> 32);
	e->addr = mapa.lm + (uint32_t)(lm_total % LM_SIZE);
	lm_total += size;
	emit_write (e->addr, lux, (uint32_t)(e->stride * e->rows), &fs.lm_bytes);
	fs.lm_builds++;
	return e;
}

/* С меткой Q2M_F_LM (26.11.2026, BZ_PROF_FINE): сколько стоит сборка lightmap — её зовёт и горячий модуль */
static lment_t *lm_build (msurface_t *surf, bzr fx, bzr fy)
{
	lment_t *e;
	BZ_PROF_BEGIN (Q2M_F_LM);
	e = lm_build_body (surf, fx, fy);
	BZ_PROF_END (Q2M_F_LM);
	return e;
}

/* ---------------------------------------------------------------- горячее: bz_hot.c */

static bz_ab_t ab;                 /* пачка треугольников модели (тип — bz_int.h; как рисует md2 — ниже) */

/* Горячие функции — в bz_hot.c: тот же исходник собирается и в горячий модуль Q2HOT.ELF (esp/hot/q2hot.c),
 * который прошивка кладёт во внутреннее ОЗУ. Здесь он подключён как есть — свои копии функций. */
#include "bz_hot.c"

/* Горячие функции, которые зовутся снаружи: свои копии или (есть модуль) копии модуля во внутреннем ОЗУ */
#ifdef BZ_ONLY
#define BZH_LOCAL { sizeof(q2hot_exp_t), Q2HOT_ABI, bz_face, bz_alias_model_tris, bz_particle, hot_final_verts, \
                    hot_project_final_vert }
#else
#define BZH_LOCAL { sizeof(q2hot_exp_t), Q2HOT_ABI, bz_face, bz_alias_model_tris, bz_particle, NULL, NULL }
#endif
static const q2hot_exp_t bzh_local = BZH_LOCAL;
static q2hot_exp_t       bzh = BZH_LOCAL;

#ifdef BZ_ONLY
/* Перевод вершин модели в экран: у ref_esp этих функций id в r_alias.c нет (p06_ref_esp.py) — они в bz_hot.c */
void R_AliasTransformFinalVerts (int numpoints, finalvert_t *fv, dtrivertx_t *oldv, dtrivertx_t *newv)
{
	bzh.final_verts (numpoints, fv, oldv, newv);
}

void R_AliasProjectAndClipTestFinalVert (finalvert_t *fv)
{
	bzh.project_vert (fv);
}
#endif
static q2hot_imp_t       hot_imp;   /* что берёт модуль (BZ_HotTables) */
static q2hot_exp_t       hot_exp;   /* что он отдал                    */

/* Таблицы для горячего модуля (q2_app.c: module_open_ex «Q2HOT.ELF», arg1 — imp, arg2 — exp; на ПК q2esp.exe с
 * Q2M_HOT=1 — сам). Адреса данных и функций этого файла и рендера id. */
void BZ_HotTables (void **imp, void **exp)
{
	static const uint32_t sizes[Q2HOT_NSIZES] = Q2HOT_SIZES;
	q2hot_imp_t *t = &hot_imp;

	memset (t, 0, sizeof(*t));
	t->size = sizeof(*t);
	t->abi = Q2HOT_ABI;
	memcpy (t->sizes, sizes, sizeof(sizes));
	t->cb = &cb;
	t->cb_sky = &cb_sky;
	t->fs = &fs;
	t->mapa = &mapa;
	t->ab = &ab;
	t->texhash = texhash;
	t->tex_page_frame = tex_page_frame;
	t->tex_frame = &tex_frame;
	t->bz_alias_skin = &bz_alias_skin;
	t->put_full = put_full;
	t->tex_load = tex_load;
	t->image_id = image_id;
	t->lm_build = lm_build;
	t->lmhash = lmhash;
	t->lm_total = &lm_total;
	t->r_worldmodel = &r_worldmodel;
	t->r_newrefdef = &r_newrefdef;
	t->r_framecount = &r_framecount;
	t->BZ_CalcGradients = BZ_CalcGradients;
	t->currentmodel = &currentmodel;
	t->r_pcurrentvertbase = &r_pcurrentvertbase;
	t->view_clipplanes = view_clipplanes;
	t->modelorg = modelorg;
	t->vright = vright;
	t->vup = vup;
	t->vpn = vpn;
	t->transformed_modelorg = transformed_modelorg;
	t->xcenter = &xcenter;
	t->ycenter = &ycenter;
	t->xscale = &xscale;
	t->yscale = &yscale;
	t->xscaleinv = &xscaleinv;
	t->yscaleinv = &yscaleinv;
	t->scale_for_mip = &scale_for_mip;
	t->r_refdef = &r_refdef;
	t->d_sdivzorigin = &d_sdivzorigin;
	t->d_sdivzstepu = &d_sdivzstepu;
	t->d_sdivzstepv = &d_sdivzstepv;
	t->d_tdivzorigin = &d_tdivzorigin;
	t->d_tdivzstepu = &d_tdivzstepu;
	t->d_tdivzstepv = &d_tdivzstepv;
	t->sadjust = &sadjust;
	t->tadjust = &tadjust;
	t->r_aliasblendcolor = &r_aliasblendcolor;
	t->r_affinetridesc = &r_affinetridesc;
	t->currententity = &currententity;
#ifdef BZ_ONLY
	t->r_lerp_frontv = r_lerp_frontv;
	t->r_lerp_backv = r_lerp_backv;
	t->r_lerp_move = r_lerp_move;
	t->r_plightvec = r_plightvec;
	t->r_avertexnormals = r_avertexnormals;
	t->aliastransform = aliastransform;
	t->r_ambientlight = &r_ambientlight;
	t->r_shadelight = &r_shadelight;
	t->s_ziscale = &s_ziscale;
#endif
	t->aliasxscale = &aliasxscale;
	t->aliasyscale = &aliasyscale;
	t->aliasxcenter = &aliasxcenter;
	t->aliasycenter = &aliasycenter;
	t->R_TextureAnimation = R_TextureAnimation;
	t->D_MipLevelForScale = D_MipLevelForScale;
	t->R_AliasClipTriangle = R_AliasClipTriangle;
	memset (&hot_exp, 0, sizeof(hot_exp));
	*imp = t;
	*exp = &hot_exp;
}

/* 1 — звать горячие функции модуля (он отдал таблицу exp), 0 — свои копии. Итог: 1 — модуль взят. */
int BZ_HotUse (int on)
{
	if (on && hot_exp.size == sizeof(hot_exp) && hot_exp.abi == Q2HOT_ABI && hot_exp.face && hot_exp.alias_tris &&
	    hot_exp.particle && hot_exp.final_verts && hot_exp.project_vert)
	{
		bzh = hot_exp;
		return 1;
	}
	bzh = bzh_local;
	return 0;
}

/* ---------------------------------------------------------------- врезки 3D */

static float bz_blend[4];
static int   bz_had3d;

void BZ_Begin3D (void)
{
	vrect_t *r = &r_refdef.vrect;

	put ((uint32_t)BZ_TIME << 24);
	put ((uint32_t)((int)(r_newrefdef.time * TURB_SPEED) & (TURB_CYCLE - 1)));
	put ((uint32_t)BZ_SCISSOR << 24);
	put (bz_xy (r->x, r->y));
	put (bz_xy (r->x + r->width, r->y + r->height));
	/* z кадра: 0 — «дальше всего»; небо потом ляжет ровно туда, где z осталось 0 */
	put (((uint32_t)BZ_FILL << 24) | 1u);
	put (bz_xy (r->x, r->y));
	put (bz_xy (r->width, r->height));
	put (0);
	memcpy (bz_blend, r_newrefdef.blend, sizeof(bz_blend));
	bz_had3d = 1;
}

void BZ_End3D (void)
{
	put ((uint32_t)BZ_SCISSOR << 24);
	put (bz_xy (0, 0));
	put (bz_xy (vid.width, vid.height));
}

void BZ_EndWorld (void)
{
	uint32_t i;
	if (cb_sky.n > sky_frame_words)         /* замер: сколько слов неба копится за кадр (bz.csv sky_bytes) */
		sky_frame_words = cb_sky.n;
	for (i = 0; i < cb_sky.n; i++)
		put (cb_sky.w[i]);
	if (cb_sky.n)
		cb_forget (&cb_main);          /* небо выставило свои TEX — наше «последнее» уже не то */
	cb_sky.n = 0;
	cb_forget (&cb_sky);
}

void BZ_RenderFace (msurface_t *fa, int clipflags)
{
	BZ_PROF_BEGIN (Q2M_F_FACE);
	if (fa->flags & SURF_DRAWSKYBOX)
	{
		cb = &cb_sky;
		bzh.face (fa, clipflags, 0);
		cb = &cb_main;
	}
	else
		bzh.face (fa, clipflags, 0);
	BZ_PROF_END (Q2M_F_FACE);
}

void BZ_AlphaSurfaces (void)
{
	msurface_t *s;

	currentmodel = r_worldmodel;
	r_pcurrentvertbase = r_worldmodel->vertexes;
	VectorCopy (r_origin, modelorg);
	for (s = r_alpha_surfaces; s; s = s->nextalphasurface)
		bzh.face (s, 15, (s->texinfo->flags & SURF_TRANS66) ? 66 : 33);
	r_alpha_surfaces = NULL;
}

/* ---------------------------------------------------------------- модели пачками (VBUF, STBUF, TRIS) */

/*
 * Как Quake рисует модель (md2). R_AliasPreparePoints переводит в экран все вершины модели один раз
 * (массив pfinalverts: целые координаты экрана u, v, 1/z и свет каждой вершины), а потом идёт по списку
 * треугольников: каждый треугольник — три номера вершин и три номера точек шкурки (st). Одна вершина
 * входит в несколько треугольников (в среднем ~5).
 *
 * Раньше каждый треугольник уходил командой POLY: 3 вершины и 12 слов градиентов — 16 слов (64 байта);
 * у модели их сотни, и модели занимали 59 % потока LINK. Теперь треугольники одной модели копятся в
 * «пачку»: вершины и st, на которые есть ссылки, уходят по одному разу (VBUF — 3 слова на вершину,
 * STBUF — 1 слово на точку шкурки), а сам треугольник — 2 слова номеров (TRIS). Плоскость треугольника
 * (градиенты) считает ПЛИС по точной целой формуле (SPEC.md, «Треугольники моделей»).
 *
 * Обратные стороны и вырожденные треугольники отбрасываются ещё здесь (та же формула, что у ПЛИС и у
 * R_DrawTriangle): их не надо слать вовсе, и не уходят вершины, нужные только им.
 *
 * Треугольник, который Quake обрезал краем экрана или ближней плоскостью (R_AliasClipTriangle), имеет
 * новые вершины не из pfinalverts — он идёт прежним POLY. Чтобы порядок рисования остался как у id
 * (от него зависят равные z), перед таким треугольником пачка отправляется.
 */
/* Номер точки шкурки {s, t} в пачке: найти в хэше или добавить. */
static int ab_st (uint32_t key)
{
	uint32_t h = (key * 2654435761u) >> 22;            /* 10 бит */
	for (;; h = (h + 1) & (STH_N - 1))
	{
		if (ab.sth_gen[h] != ab.gen)
		{
			ab.sth_gen[h] = ab.gen;
			ab.sth_key[h] = key;
			ab.sth_idx[h] = (short)ab.nst;
			ab.stv[ab.nst] = key;
			return ab.nst++;
		}
		if (ab.sth_key[h] == key)
			return ab.sth_idx[h];
	}
}

/* Все треугольники модели (врезка p04 в R_AliasPreparePoints): bz_alias_model_tris — bz_hot.c */
int BZ_AliasModelTris (void *pmdl, void *pfinalverts, int reverse, int kind)
{
	return bzh.alias_tris (pmdl, pfinalverts, reverse, kind);
}

/* Начало модели: врезка в R_AliasPreparePoints после перевода вершин в экран. */
void BZ_AliasModelBegin (void *pfinalverts, int num)
{
	ab_flush ();
	ab.base = (finalvert_t *)pfinalverts;
	ab.nbase = num;
}

/* Конец модели: врезка в конце R_AliasPreparePoints (pfinalverts ещё жив — он на стеке id). */
void BZ_AliasModelEnd (void)
{
	ab_flush ();
	ab.base = NULL;
}


/* Треугольник модели (md2) после отсечения id: вершины — целые точки экрана id, s, t — 16.16
 * шкурки, l — свет id (строка colormap = l >> 8), zi — 1/z·2^31 (у оружия ·3, RF_DEPTHHACK).
 * kind: 0 — непрозрачно, 1/2 — 33/66 %, 3/4 — оболочка сплошным цветом 33/66 %. */
static void bz_alias_triangle (int kind)
{
	finalvert_t *p[3] = { aliastriangleparms.a, aliastriangleparms.b, aliastriangleparms.c };
	uint32_t xy[3], g[4][3];
	bzr      f[4][3], d, du1, dv1, du2, dv2;
	int      i, dv0_ab, dv1_ab, dv0_ac, dv1_ac;

	/* отбор как в R_DrawTriangle id: вырожденные и обращённые спиной — мимо */
	dv0_ab = p[0]->u - p[1]->u;
	dv1_ab = p[0]->v - p[1]->v;
	if (!(dv0_ab | dv1_ab))
		return;
	dv0_ac = p[0]->u - p[2]->u;
	dv1_ac = p[0]->v - p[2]->v;
	if (!(dv0_ac | dv1_ac))
		return;
	fs.tris++;
	if (dv0_ac * dv1_ab - dv0_ab * dv1_ac >= 0)
	{
		fs.tris_culled++;
		return;
	}

	/* вершины из pfinalverts модели — в пачку */
	if (ab.base && p[0] >= ab.base && p[0] < ab.base + ab.nbase && p[1] >= ab.base && p[1] < ab.base + ab.nbase &&
	    p[2] >= ab.base && p[2] < ab.base + ab.nbase)
	{
		int vi[3], si[3];
		if (ab.ntri && kind != ab.kind)
			ab_flush ();
		if (ab.nv + 3 > AB_MAX || ab.nst + 3 > AB_MAX || ab.ntri == AB_TRIS)
			ab_flush ();
		ab.kind = kind;
		for (i = 0; i < 3; i++)
		{
			const int n = (int)(p[i] - ab.base);
			if (!ab.vmap[n])
			{
				ab.vlist[ab.nv] = n;
				ab.vmap[n] = (short)(++ab.nv);
			}
			vi[i] = ab.vmap[n] - 1;
			si[i] = ab_st (((uint32_t)(uint16_t)(p[i]->s >> 16)) | ((uint32_t)(uint16_t)(p[i]->t >> 16) << 16));
		}
		ab.tw[2 * ab.ntri] = (uint32_t)vi[0] | ((uint32_t)vi[1] << 9) | ((uint32_t)vi[2] << 18);
		ab.tw[2 * ab.ntri + 1] = (uint32_t)si[0] | ((uint32_t)si[1] << 9) | ((uint32_t)si[2] << 18);
		ab.ntri++;
		return;
	}

	/* обрезанный треугольник — прежним POLY, но после уже накопленной пачки (порядок как у id) */
	ab_flush ();
	for (i = 0; i < 3; i++)
	{
		xy[i] = bz_xy (p[i]->u * 16 + 8, p[i]->v * 16 + 8);
		f[0][i] = p[i]->zi * BZR(0.5);     /* Q31 id → Q2.30 */
		f[1][i] = p[i]->s;
		f[2][i] = p[i]->t;
		f[3][i] = p[i]->l * BZR(65536.0);
	}
	/* плоскость через три вершины: значение в точке (x, y) id = C + A·x + B·y */
	du1 = p[1]->u - p[0]->u; dv1 = p[1]->v - p[0]->v;
	du2 = p[2]->u - p[0]->u; dv2 = p[2]->v - p[0]->v;
	d = du1 * dv2 - du2 * dv1;
	for (i = 0; i < 4; i++)
	{
		bzr a = ((f[i][1] - f[i][0]) * dv2 - (f[i][2] - f[i][0]) * dv1) / d;
		bzr b = (du1 * (f[i][2] - f[i][0]) - du2 * (f[i][1] - f[i][0])) / d;
		g[i][0] = fx32 (f[i][0] - a * p[0]->u - b * p[0]->v);
		g[i][1] = fx32 (a);
		g[i][2] = fx32 (b);
	}
	alias_tex (kind);
	emit_poly (xy, 3, alias_mode (kind), (const uint32_t (*)[3])g, kind >= 3 ? 3 : 4);
}

/* Частица (врезка p04 в R_DrawParticle): bz_particle — bz_hot.c */
void BZ_Particle (int u, int v, float zi, int pix, int color, int level)
{
	bzh.particle (u, v, zi, pix, color, level);
}

/* Спрайт или луч: r_polydesc уже отсечён и спроецирован R_ClipAndDrawPoly id. */
void BZ_DrawPoly (int iswater, int blendcolor)
{
	uint32_t xy[MAXFV], g[4][3];
	bzr      kz[3], sd[3], td[3];
	int      i, n = r_polydesc.nump, mode;

	if (n < 3 || n > MAXFV)
		return;
	R_PolygonCalculateGradients ();
	for (i = 0; i < n; i++)
		xy[i] = xy_soft (r_polydesc.pverts[i].u, r_polydesc.pverts[i].v);
	kz[0] = d_ziorigin; kz[1] = d_zistepu; kz[2] = d_zistepv;
	sd[0] = d_sdivzorigin; sd[1] = d_sdivzstepu; sd[2] = d_sdivzstepv;
	td[0] = d_tdivzorigin; td[1] = d_tdivzstepu; td[2] = d_tdivzstepv;
	persp_grads (g, kz, sadjust / BZR(65536.0), tadjust / BZR(65536.0), 1, 1, sd, td);

	if (!bz_poly_textured)
	{
		mode = BZ_M_PERSP | BZ_L_NONE | BZ_M_ZTEST | BZ_T_33;
		put_solid (blendcolor);
	}
	else
	{
		if (bz_poly_alpha == 1)
			mode = BZ_M_PERSP | BZ_L_NONE | BZ_M_ZTEST | BZ_M_ZWRITE | BZ_M_KEY;
		else
			mode = BZ_M_PERSP | BZ_L_NONE | BZ_M_ZTEST | (bz_poly_alpha > 0.33f ? BZ_T_66 : BZ_T_33) |
			       (iswater ? BZ_M_WARP : BZ_M_KEY);
		put_tex (tex_get (bz_poly_image, 0, r_polydesc.pixel_width, r_polydesc.pixel_height, 1));
	}
	emit_poly (xy, n, mode, (const uint32_t (*)[3])g, 3);
}

/* ---------------------------------------------------------------- 2D */

static void put_pic (int x, int y, int w, int h, uint32_t addr, int stride, uint32_t ss, uint32_t ts, int key)
{
	put (((uint32_t)BZ_PIC << 24) | (key ? 1u : 0u));
	put (bz_xy (x, y));
	put ((uint32_t)w | ((uint32_t)h << 16));
	put (addr);
	put ((uint32_t)stride);
	put (ss);
	put (ts);
	fs.polys2d++;
}

void BZ_DrawChar (int x, int y, int num, image_t *chars)
{
	texent_t *te;

	if (!BZ_Enabled () || !chars)
		return;
	num &= 255;
	if (num == 32 || num == 32 + 128 || y <= -8 || y + 8 > vid.height)
		return;
	te = tex_get (chars, 0, chars->width, chars->height, 1);
	put_pic (x, y, 8, 8, te->addr + ((num >> 4) << 10) + ((num & 15) << 3), chars->width, 0x10000, 0x10000, 1);
}

void BZ_StretchPic (int x, int y, int w, int h, image_t *pic)
{
	uint32_t addr;

	if (!BZ_Enabled () || x < 0 || x + w > vid.width || y + h > vid.height || w <= 0 || h <= 0)
		return;
	if (bz_raw)
	{
		/* кадр кинематики меняется каждый раз: грузится заново, без кэша */
		uint32_t bytes = (uint32_t)(pic->width * pic->height);
		if (bytes > 0x40000)
			return;
		emit_write (mapa.raw, pic->pixels[0], bytes, &fs.other_bytes);
		addr = mapa.raw;
	}
	else
		addr = tex_get (pic, 0, pic->width, pic->height, 1)->addr;
	/* s — как у id (fstep = ширина·0x10000/w); t — с округлением вверх, чтобы (j·шаг) >> 16
	 * совпало с целым делением id (j·высота/h) */
	put_pic (x, y, w, h, addr, pic->width, (uint32_t)(pic->width * 0x10000 / w),
	         (uint32_t)(((int64_t)pic->height * 0x10000 + h - 1) / h), 0);
}

void BZ_DrawPic (int x, int y, image_t *pic)
{
	if (!BZ_Enabled ())
		return;
	put_pic (x, y, pic->width, pic->height, tex_get (pic, 0, pic->width, pic->height, 1)->addr,
	         pic->width, 0x10000, 0x10000, pic->transparent);
}

/* Фон вокруг уменьшенного окна: плитка 64×64 по координатам экрана — это аффинный
 * прямоугольник POLY с s = x, t = y и текстурой TILED (повтор по маске). */
void BZ_TileClear (int x, int y, int w, int h, image_t *pic)
{
	uint32_t xy[4], g[4][3];

	if (!BZ_Enabled () || !pic)
		return;
	memset (g, 0, sizeof(g));
	g[1][1] = 0x10000;
	g[2][2] = 0x10000;
	xy[0] = bz_xy (16 * x, 16 * y);
	xy[1] = bz_xy (16 * (x + w), 16 * y);
	xy[2] = bz_xy (16 * (x + w), 16 * (y + h));
	xy[3] = bz_xy (16 * x, 16 * (y + h));
	put ((uint32_t)BZ_SCISSOR << 24);
	put (bz_xy (0, 0));
	put (bz_xy (vid.width, vid.height));
	put_tex (tex_get (pic, 0, pic->width, pic->height, 0));
	emit_poly (xy, 4, BZ_L_NONE, (const uint32_t (*)[3])g, 3);
	fs.polys2d++;
}

void BZ_Fill (int x, int y, int w, int h, int c)
{
	if (!BZ_Enabled ())
		return;
	put ((uint32_t)BZ_FILL << 24);
	put (bz_xy (x, y));
	put ((uint32_t)w | ((uint32_t)h << 16));
	put ((uint32_t)(c & 255));
}

void BZ_FadeScreen (void)
{
	if (!BZ_Enabled ())
		return;
	put (((uint32_t)BZ_FILL << 24) | 2u);
	put (bz_xy (0, 0));
	put ((uint32_t)vid.width | ((uint32_t)vid.height << 16));
	put (0);
}

/* ---------------------------------------------------------------- кадр, сравнение, отчёт */

static bz_t   *model;
static cvar_t *bz_enable_cv, *bz_dump_cv, *bz_clear_cv, *bz_trace_cv, *q2m_out_cv, *q2m_frame_cv;
static FILE   *trace;     /* <q2m_out>.bzcmd: поток команд по кадрам — вход стенда RTL (gpu/rtl/tb) */

#ifndef BZ_ESP
/* Трасса кадров md2 (замер для кэша кадров на ESP): строка «кадр_игры модель кадр_модели байт_кадра». */
static cvar_t *bz_md2trace_cv;
static FILE   *md2trace;

void BZ_Md2Access (const char *model, int frame, int framesize)
{
	if (!bz_md2trace_cv || !bz_md2trace_cv->value || !q2m_out_cv->string[0])
		return;
	if (!md2trace)
	{
		char name[MAX_OSPATH + 16];
		Com_sprintf (name, sizeof(name), "%s.md2trace", q2m_out_cv->string);
		md2trace = fopen (name, "w");
		if (!md2trace)
			return;
	}
	fprintf (md2trace, "%d %s %d %d\n", (int)q2m_frame_cv->value, model, frame, framesize);
}
#else
void BZ_Md2Access (const char *model, int frame, int framesize)
{
	(void)model; (void)frame; (void)framesize;
}
#endif
static byte   *soft_copy;
static int     soft_w, soft_h;
static FILE   *csv;

/*
 * BZ_ESP — сборка для ESP (Q2.ELF, esp/): C-модели адаптера нет (рисует ядро Tang), команды кадра
 * уходят в LINK (esp/main/bz_link.c); трассы, отчётов и BMP нет — у модуля нет файлов на ПК.
 */
#ifdef BZ_ESP
int bz_link_send (const uint32_t *words, uint32_t count);
#endif

int BZ_Enabled (void)
{
#ifdef BZ_ESP
	return 1;
#else
	return model && bz_enable_cv && bz_enable_cv->value;
#endif
}

void BZ_Init (void)
{
	bz_enable_cv = ri.Cvar_Get ("bz_enable", "1", 0);
	bz_dump_cv = ri.Cvar_Get ("bz_dump", "0", 0);
	bz_clear_cv = ri.Cvar_Get ("bz_clear", "251", 0);
	bz_trace_cv = ri.Cvar_Get ("bz_trace", "0", 0);
	q2m_out_cv = ri.Cvar_Get ("q2m_out", "", 0);
	q2m_frame_cv = ri.Cvar_Get ("q2m_frame", "0", 0);
#if defined BZ_ONLY && !defined BZ_ESP
	/* q2esp.exe с Q2M_HOT=1 (26.11.2026): горячие функции — через таблицы, как у модуля Q2HOT.ELF на ESP
	 * (esp/hot/q2hot.c собран сюда же с переназначением имён) — кадры должны совпасть с обычной сборкой */
	if (getenv ("Q2M_HOT") && atoi (getenv ("Q2M_HOT")))
	{
		void *imp, *exp;
		int   r;

		BZ_HotTables (&imp, &exp);
		r = q2hot_entry ((const q2hot_imp_t *)imp, (q2hot_exp_t *)exp);
		/* в stderr, не в консоль игры: строка консоли попала бы в кадр заставки, и кадры разошлись бы */
		fprintf (stderr, "bz: hot module (PC test): entry %d, in use %d\n", r, r == 0 && BZ_HotUse (1));
	}
#endif
#ifndef BZ_ESP
	bz_md2trace_cv = ri.Cvar_Get ("bz_md2trace", "0", 0);
	if (!model)
		model = bz_create (BZ_MEM);
	BZ_InstallCrashHandler (q2m_out_cv->string);
#endif
}

void BZ_SetMode (int width, int height)
{
	layout (width, height);
	tex_flush ();
	fs.tex_flushes = 0;
	memset (lmhash, 0, sizeof(lmhash));
	lm_total = 0;
	need_setup = 1;
	/* хранилище пусто (layout): его списки — тоже (25.11.2026: раньше свободные куски прошлой раскладки
	 * оставались в списке и могли выдаться поверх текстур новой) */
	store_nfree = 0;
	store_bytes = 0;
#ifndef BZ_ESP
	store_nspan = 0;
#endif
	/* кольцо кадров md2 — сейчас, пока куча PSRAM сплошная (раньше — при первом кадре, после загрузки
	 * карты: на плате 24.11 256 КБ одним куском тогда уже не нашлось) */
	if (!md2_ring && BZ_StoreAvailable ())
		md2_ring = malloc (MD2_RING);
#ifndef BZ_ESP
	/* копия кадра эталона id — только двойному проходу ref_bz на ПК (BZ_SaveSoft/BZ_RestoreSoft); у
	 * ref_esp кадра id нет — на ESP эти 100 КБ PSRAM (424×240) не брать (плата 24.11: памяти не хватило) */
	free (soft_copy);
	soft_copy = malloc (width * height);
#endif
	soft_w = width;
	soft_h = height;
}

void BZ_Shutdown (void)
{
	if (csv)
		fclose (csv);
	csv = NULL;
	if (trace)
		fclose (trace);
	trace = NULL;
#ifndef BZ_ESP
	bz_free (model);
#endif
	model = NULL;
	free (soft_copy);
	soft_copy = NULL;
#ifdef BZ_ESP
	bz_link_wait_idle ();             /* буферы могла ещё отправлять задача LINK */
	{
		int i;
		for (i = 0; i < cb_nspare; i++)
		{
			free (cb_spare[i]);
			cb_spare[i] = NULL;
		}
		cb_nspare = cb_spare_failed = 0;
		bz_link_set_depth (1);
	}
#endif
	free (cb_main.w);
	free (cb_sky.w);
	memset (&cb_main, 0, sizeof(cb_main));
	memset (&cb_sky, 0, sizeof(cb_sky));
}

void BZ_SaveSoft (void)
{
	int y;
	for (y = 0; y < soft_h; y++)
		memcpy (soft_copy + y * soft_w, vid.buffer + y * vid.rowbytes, soft_w);
}

void BZ_RestoreSoft (void)
{
	int y;
	for (y = 0; y < soft_h; y++)
		memcpy (vid.buffer + y * vid.rowbytes, soft_copy + y * soft_w, soft_w);
}

/* Таблицы в SDRAM и SETUP: colormap и alphamap id (colormap.pcx), палитра, синус волны id. */
static void emit_setup (void)
{
	byte    pal[768], solid[256], turb[512];
	int     i;
	cbuf_t *keep = cb;

	cb = &cb_main;
	emit_write (mapa.colormap, vid.colormap, 64 * 256, &fs.other_bytes);
	emit_write (mapa.alphamap, vid.alphamap, 256 * 256, &fs.other_bytes);
	for (i = 0; i < 256; i++)
	{
		pal[i * 3 + 0] = ((byte *)d_8to24table)[i * 4 + 0];
		pal[i * 3 + 1] = ((byte *)d_8to24table)[i * 4 + 1];
		pal[i * 3 + 2] = ((byte *)d_8to24table)[i * 4 + 2];
		solid[i] = (byte)i;
	}
	for (i = 0; i < 128; i++)
	{
		turb[i * 4 + 0] = (byte)sintable[i];
		turb[i * 4 + 1] = (byte)(sintable[i] >> 8);
		turb[i * 4 + 2] = (byte)(sintable[i] >> 16);
		turb[i * 4 + 3] = (byte)(sintable[i] >> 24);
	}
	emit_write (mapa.pal, pal, 768, &fs.other_bytes);
	emit_write (mapa.turb, turb, 512, &fs.other_bytes);
	emit_write (mapa.solid, solid, 256, &fs.other_bytes);
	put ((uint32_t)BZ_SETUP << 24);
	put (mapa.fb0);
	put (mapa.fb1);
	put (mapa.z);
	put ((uint32_t)vid.width | ((uint32_t)vid.height << 16));
	put (mapa.colormap);
	put (mapa.alphamap);
	put (mapa.pal);
	put (mapa.turb);
	cb = keep;
}

#ifndef BZ_ESP
static void write_bmp (const char *name, int w, int h, const byte *idx, const byte *pal4, const byte *rgb)
{
	FILE *f = fopen (name, "wb");
	int   y, x, row = (w * 3 + 3) & ~3;
	byte  hdr[54] = { 'B', 'M' };
	byte *line = calloc (1, row);

	if (!f)
	{
		free (line);
		return;
	}
	*(int *)(hdr + 2) = 54 + row * h;
	*(int *)(hdr + 10) = 54;
	*(int *)(hdr + 14) = 40;
	*(int *)(hdr + 18) = w;
	*(int *)(hdr + 22) = h;
	*(short *)(hdr + 26) = 1;
	*(short *)(hdr + 28) = 24;
	fwrite (hdr, 1, 54, f);
	for (y = h - 1; y >= 0; y--)
	{
		for (x = 0; x < w; x++)
		{
			const byte *c = rgb ? rgb + (y * w + x) * 3 : pal4 + idx[y * w + x] * 4;
			line[x * 3 + 0] = c[2];
			line[x * 3 + 1] = c[1];
			line[x * 3 + 2] = c[0];
		}
		fwrite (line, 1, row, f);
	}
	free (line);
	fclose (f);
}
#endif

#ifndef BZ_ESP
/* CRC-32 кадра модели адаптера (строки по soft_w точек, шаг bz_stride) и палитры — столбец mcrc в bz.csv.
 * Два прогона с одинаковыми mcrc показали одну и ту же картинку ядра. Зачем (24.11.2026): crc.txt
 * (swimp_bz.c) считает кадр id, а в ref_esp кадра id нет — там сумма ловила лишь смену палитры. */
static uint32_t model_frame_crc (const byte *mf, int w, int h, int stride, const unsigned char *palette_rgba)
{
	static uint32_t table[256];
	uint32_t        crc = 0xFFFFFFFFu;
	int             x, y;

	if (!table[1])
		for (x = 0; x < 256; x++)
		{
			uint32_t c = (uint32_t)x;
			for (y = 0; y < 8; y++)
				c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[x] = c;
		}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			crc = table[(crc ^ mf[y * stride + x]) & 0xFF] ^ (crc >> 8);
	for (x = 0; x < 256 * 4; x++)
		crc = table[(crc ^ palette_rgba[x]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}
#endif

void BZ_EndFrame (const unsigned char *palette_rgba)
{
	const bz_stats_t *st;
	const byte       *mf;
	uint32_t          err;
	int               frame, i, diff = 0, vis = 0, a;

	tex_frame++;                    /* страницы текстур: давность обращения — в кадрах */

#ifdef BZ_ESP
	if (!BZ_Enabled ())             /* ESP: кадра id нет (vid.buffer пуст) — команды всё равно уходят */
		return;
#else
	if (!BZ_Enabled () || !vid.buffer)
		return;
#endif
	if (need_setup)
	{
		/* таблицы должны лечь до всех команд кадра: SETUP ставится в начало буфера */
		cbuf_t   tmp = cb_main;
		uint32_t k;
		memset (&cb_main, 0, sizeof(cb_main));
		emit_setup ();
		for (k = 0; k < tmp.n; k++)
			put (tmp.w[k]);
		free (tmp.w);
		need_setup = 0;
	}
	a = bz_had3d ? (int)(bz_blend[3] * 255 + 0.5f) : 0;
	if (a > 255) a = 255;
	if (a < 0) a = 0;
	put ((uint32_t)BZ_FRAME << 24);
	put ((uint32_t)(int)(bz_blend[0] * 255) | ((uint32_t)(int)(bz_blend[1] * 255) << 8) |
	     ((uint32_t)(int)(bz_blend[2] * 255) << 16) | ((uint32_t)a << 24));
	bz_had3d = 0;

#ifdef BZ_ESP
	/* ESP: весь кадр — в очередь ядра Tang; ядро рисует его само (сравнивать не с чем) */
	(void)st; (void)mf; (void)err; (void)frame; (void)i; (void)diff; (void)vis; (void)palette_rgba;
	cb_main_ship ();                /* кадр уходит задачей LINK — следующий готовится параллельно */
	cb_forget (&cb_main);
	cb_forget (&cb_sky);
	memset (&fs, 0, sizeof(fs));
	return;
#else
	/* Трасса для стенда RTL: на кадр — слово «сколько слов», цвет отладочной заливки (−1 — нет) и
	 * сами слова команд. Стенд выполняет их и в RTL, и в этой же C-модели и сравнивает кадры. */
	if (bz_trace_cv->value && q2m_out_cv->string[0])
	{
		if (!trace)
		{
			char name[MAX_OSPATH + 16];
			Com_sprintf (name, sizeof(name), "%s.bzcmd", q2m_out_cv->string);
			trace = fopen (name, "wb");
		}
		if (trace)
		{
			int32_t clr = (int32_t)bz_clear_cv->value;
			fwrite (&cb_main.n, 4, 1, trace);
			fwrite (&clr, 4, 1, trace);
			fwrite (cb_main.w, 4, cb_main.n, trace);
		}
	}
	if (bz_clear_cv->value >= 0)
		bz_debug_clear (model, (uint8_t)bz_clear_cv->value);   /* дыры в кадре будут видны */
	err = bz_exec (model, cb_main.w, cb_main.n);
	if (err)
		ri.Con_Printf (PRINT_ALL, "bz: bad command at word %u\n", err);
	store_check ();                 /* хранилище в SDRAM модели цело (кадры md2 — SPEC 0.4) */

	mf = bz_shown (model);
#ifndef BZ_ONLY
	/* сравнение с кадром id — только в ref_bz; в ref_esp (BZ_ONLY) кадра id нет, diff = vis = 0 */
	for (i = 0; i < soft_w * soft_h; i++)
	{
		byte s = vid.buffer[(i / soft_w) * vid.rowbytes + i % soft_w], m = mf[(i / soft_w) * bz_stride (model) + i % soft_w];
		if (s != m)
		{
			int c, dmax = 0;
			diff++;
			for (c = 0; c < 3; c++)
			{
				int dd = abs (((byte *)d_8to24table)[s * 4 + c] - ((byte *)d_8to24table)[m * 4 + c]);
				if (dd > dmax) dmax = dd;
			}
			if (dmax > 32)
				vis++;
		}
	}
#endif

	frame = (int)q2m_frame_cv->value;
	st = bz_stats (model);
	if (!csv && q2m_out_cv->string[0])
	{
		char name[MAX_OSPATH + 16];
		Com_sprintf (name, sizeof(name), "%s.bz.csv", q2m_out_cv->string);
		csv = fopen (name, "w");
		if (csv)
			fprintf (csv, "frame,underwater,diff,diff_vis,cmd_bytes,tex_bytes,lm_bytes,other_bytes,lm_builds,"
			              "tex_uploads,faces,tris,parts,polys,px_raster,px_zfail,px_key,px_written,px_fill,"
			              "tex_reads,tex_miss,lm_fetch,divides,seg,seg_zread,seg_zwrite,seg_cread,seg_cwrite,mcrc,"
			              "tex_used,sky_bytes\n");
	}
	if (csv)
		fprintf (csv, "%d,%d,%d,%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
		              "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%08x,%u,%u\n",
		         frame, (r_newrefdef.rdflags & RDF_UNDERWATER) ? 1 : 0, diff, vis,
		         (unsigned long long)st->cmd_words * 4, (unsigned long long)fs.tex_bytes,
		         (unsigned long long)fs.lm_bytes, (unsigned long long)fs.other_bytes,
		         (unsigned long long)fs.lm_builds, (unsigned long long)fs.tex_uploads,
		         (unsigned long long)fs.faces, (unsigned long long)fs.tris, (unsigned long long)fs.parts,
		         (unsigned long long)st->polys, (unsigned long long)st->px_raster,
		         (unsigned long long)st->px_zfail, (unsigned long long)st->px_key,
		         (unsigned long long)st->px_written, (unsigned long long)st->px_fill,
		         (unsigned long long)st->tex_reads, (unsigned long long)st->tex_miss,
		         (unsigned long long)st->lm_fetch, (unsigned long long)st->divides,
		         (unsigned long long)st->seg, (unsigned long long)st->seg_zread,
		         (unsigned long long)st->seg_zwrite, (unsigned long long)st->seg_cread,
		         (unsigned long long)st->seg_cwrite,
		         (unsigned)model_frame_crc (mf, soft_w, soft_h, bz_stride (model), palette_rgba),
		         (unsigned)(tex_next - mapa.tex),      /* SDRAM под текстурами сейчас (замер 24.11) */
		         (unsigned)sky_frame_words * 4);       /* слов неба в кадре × 4 — буфер cb_sky (замер 24.11) */
	sky_frame_words = 0;

	if (bz_dump_cv->value > 0 && q2m_out_cv->string[0] && frame % (int)bz_dump_cv->value == 0)
	{
		char  name[MAX_OSPATH + 32];
#ifdef BZ_ONLY
		/* ref_esp: только кадр модели адаптера */
		byte *m = malloc (soft_w * soft_h);
		int   y;
		for (y = 0; y < soft_h; y++)
			memcpy (m + y * soft_w, mf + y * bz_stride (model), soft_w);
		Com_sprintf (name, sizeof(name), "%s.%05d.bz.bmp", q2m_out_cv->string, frame);
		write_bmp (name, soft_w, soft_h, m, palette_rgba, NULL);
		free (m);
#else
		byte *d = malloc (soft_w * soft_h * 3);
		for (i = 0; i < soft_w * soft_h; i++)
		{
			byte s = vid.buffer[(i / soft_w) * vid.rowbytes + i % soft_w];
			int  on = s != mf[(i / soft_w) * bz_stride (model) + i % soft_w], c;
			for (c = 0; c < 3; c++)
				d[i * 3 + c] = on ? (c == 0 ? 255 : 0) : (byte)(palette_rgba[s * 4 + c] / 3);
		}
		Com_sprintf (name, sizeof(name), "%s.%05d.soft.bmp", q2m_out_cv->string, frame);
		write_bmp (name, soft_w, soft_h, vid.buffer, palette_rgba, NULL);
		Com_sprintf (name, sizeof(name), "%s.%05d.bz.bmp", q2m_out_cv->string, frame);
		{
			/* у модели строки идут с шагом bz_stride (W вверх до 16) — в BMP подряд */
			byte *m = malloc (soft_w * soft_h);
			int   y;
			for (y = 0; y < soft_h; y++)
				memcpy (m + y * soft_w, mf + y * bz_stride (model), soft_w);
			write_bmp (name, soft_w, soft_h, m, palette_rgba, NULL);
			free (m);
		}
		Com_sprintf (name, sizeof(name), "%s.%05d.diff.bmp", q2m_out_cv->string, frame);
		write_bmp (name, soft_w, soft_h, NULL, NULL, d);
		free (d);
#endif
	}

	cb_main.n = 0;
	cb_forget (&cb_main);
	cb_forget (&cb_sky);
	bz_stats_reset (model);
	memset (&fs, 0, sizeof(fs));
#endif
}

/* Треугольник модели с замером (Q2M_A_TRI, 25.11.2026: где время моделей на плате) */
void BZ_AliasTriangle (int kind)
{
	BZ_PROF_BEGIN (Q2M_A_TRI);
	bz_alias_triangle (kind);
	BZ_PROF_END (Q2M_A_TRI);
}
