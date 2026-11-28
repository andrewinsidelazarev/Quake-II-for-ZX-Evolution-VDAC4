/*
 * =====================================================================================================
 * bz_int.h — внутреннее переднего края «бомж-3D»: типы и таблицы, общие для bz_front.c и горячего модуля.
 * =====================================================================================================
 *
 * ЗАЧЕМ (26.11.2026, пользователь: «Да, переноси»)
 *   На плате ядро игры ~22 % тактов ждёт выборки команд из PSRAM (кэш команд ESP32-S3 — 16 КБ, а код
 *   Q2.ELF — 400 КБ; board_log15, счётчики Xtensa). Самые горячие функции переднего края (грань мира,
 *   треугольники моделей, частицы — bz_hot.c) прошивка может положить во внутреннее ОЗУ (code_ram, 16 КБ),
 *   но только целым модулем ELF: Q2HOT.ELF (esp/hot/q2hot.c). Модулю нельзя ссылаться на символы Q2.ELF
 *   (загрузчик связывает его лишь с прошивкой), поэтому всё, что ему нужно от основного модуля, — данные
 *   и функции — он получает таблицей q2hot_imp_t, а свои функции отдаёт таблицей q2hot_exp_t.
 *
 * ОДИН ИСХОДНИК — ДВЕ СБОРКИ
 *   bz_front.c подключает bz_hot.c у себя (#include) — функции и данные те же, что были, без лишних
 *   обращений. q2hot.c подключает тот же bz_hot.c, но перед ним — q2hot_remap.h: имена данных и функций
 *   основного модуля там переназначены на поля таблицы импорта. Нет модуля (старая прошивка, нет файла,
 *   Unreal NG) — bz_front.c зовёт свои копии.
 *
 * Здесь — только то, что нужно обеим сборкам: типы, постоянные и таблицы. Сами данные объявлены в bz_front.c.
 */
#ifndef BZ_INT_H
#define BZ_INT_H

#include <stdint.h>

/* Перевод вещественного в 32-битное слово: округление, лишние старшие биты — по модулю 2^32
 * (модель и RTL считают градиенты по модулю, так что это не ошибка, а договор). */
/* Вещественный тип горячей математики (26.11.2026). У ESP32-S3 FPU только для float: double считается
 * программно (профиль Unreal NG: ~40 % времени кадра — __muldf3, __adddf3 … из ПЗУ). На ESP и в q2esp.exe
 * (BZ_ONLY) — float; у ref_bz для q2m.exe — double, как было: его кадр сверяется с кадром id, и по нему
 * видно, на сколько точек float меняет картинку. */
#ifdef BZ_ONLY
typedef float bzr;
#define BZR(x)     x##f
#define bz_floor   floorf
#else
typedef double bzr;
#define BZR(x)     x
#define bz_floor   floor
#endif

/* Метки времени внутри рендера (measure/q2m.h, 25.11.2026) — только у сборок ESP и q2esp.exe (BZ_ONLY):
 * у ref_bz для q2m.exe своих меток нет */
/* 26.11.2026: мелкие метки (грань, lightmap, текстура, треугольник — тысячи за кадр) — только с BZ_PROF_FINE:
 * сами метки стоили заметную долю кадра. */
/* В горячем модуле (Q2HOT_MODULE) меток нет: q2m_acc и Q2M_Now — символы Q2.ELF, модулю они недоступны. */
#if defined BZ_ONLY && defined BZ_PROF_FINE && !defined Q2HOT_MODULE
#define BZ_PROF_BEGIN(i) Q2M_BEGIN (i)
#define BZ_PROF_END(i)   Q2M_END (i)
#else
#define BZ_PROF_BEGIN(i) ((void)0)
#define BZ_PROF_END(i)   ((void)0)
#endif

static inline uint32_t fx32 (bzr v)
{
#ifdef BZ_ONLY
	/* floor без вызова floorf (26.11.2026: профиль Unreal NG — floorf из ПЗУ ~6 % времени кадра): в пределах
	 * ±2^31 — целое с поправкой вниз у отрицательных (то же, что floor); дальше — прежний путь по модулю 2^32 */
	bzr f = v + BZR(0.5);

	if (f > -2147483520.0f && f < 2147483520.0f)
	{
		int32_t i = (int32_t)f;
		if ((bzr)i > f)
			i--;
		return (uint32_t)i;
	}
#endif
	return (uint32_t)(int64_t)bz_floor (v + BZR(0.5));
}

/* Чей вызов (28.11.2026, модели на втором ядре ESP — bz_front.c, «МОДЕЛИ НА ЯДРЕ 0»): 0 — игра (ядро ESP 1: мир,
 * небо, 2D, всё прочее), 1 — модели (задача LINK на ядре ESP 0). У сторон свои буферы команд (cb) и счётчики
 * (fs); текстуры моделей берутся из таблицы кадра (ent_tex), а не из общей таблицы — её в это время меняет мир.
 * На ПК сторона 1 — поток моделей q2esp.exe (bz_ent 2) или их проход заранее (bz_ent 1). */
#if defined __XTENSA__
/* ESP: сторона 1 — это код на стеке задачи LINK (bz_e_sp: {начало, длина}, ставит её задача, bz_link.c). Не по номеру
 * ядра (PRID): Unreal NG эмулирует одно ядро с PRID 0xCDCD у всех задач — там игра сочла бы себя моделями. */
static inline int bz_ectx_sp (const volatile uint32_t *r)
{
	uint32_t sp;

	__asm__ volatile ("mov %0, a1" : "=a"(sp));
	return sp - r[0] < r[1];
}
extern volatile uint32_t bz_e_sp[2];      /* bz_front.c; в горячем модуле — q2h.e_sp (q2hot_remap.h) */
#define bz_ectx() bz_ectx_sp (bz_e_sp)
#elif defined _MSC_VER
extern __declspec(thread) int bz_ectx_pc;
#define bz_ectx() bz_ectx_pc
#else
extern __thread int bz_ectx_pc;
#define bz_ectx() bz_ectx_pc
#endif

#define BZ_NEAR     0.6f            /* ближняя плоскость: zi ≤ 1,67 влезает в Q2.30          */
#define MAXFV       80              /* вершин у грани после отсечения                        */

/* Раскладка SDRAM модели (bz_front.c, layout) */
typedef struct
{
	uint32_t fb0, fb1, z;           /* кадры и z                                            */
	uint32_t colormap, alphamap, pal, turb, solid;
	uint32_t raw;                   /* кадр кинематики (Draw_StretchRaw)                   */
	uint32_t lm;                    /* кольцо lightmap                                      */
	uint32_t tex, tex_end;          /* текстуры, картинки, шкурки                           */
} bz_mapa_t;

/*
 * Буфер команд одного кадра. Кроме слов он помнит последнюю выданную TEX и LMAP: если следующий
 * многоугольник рисуется той же текстурой и тем же lightmap, команда не повторяется — адаптер помнит
 * их сам (состояние в регистрах ПЛИС). Это экономит LINK: модели шлют TEX перед каждым треугольником,
 * а у треугольников одной модели шкурка одна (замер tools/bz_cmdstat.py: −11 КБ на кадр).
 * Состояние сбрасывается, когда буфер начинается заново (новый кадр) и когда к нему дописывается
 * чужой буфер (небо) — после этого адаптер помнит уже не то, что помним мы.
 */
typedef struct
{
	uint32_t *w;
	uint32_t  n, cap;
	uint32_t  tex[3], lm[5];     /* последние TEX и LMAP в этом буфере */
	int       tex_ok, lm_ok;     /* 1 — значения выше действительны   */
} cbuf_t;

/* Счётчики переднего края */
typedef struct
{
	uint64_t tex_bytes, lm_bytes, other_bytes;   /* байт загрузок по видам               */
	uint64_t tex_uploads, lm_builds, tex_flushes;
	uint64_t faces, faces_clipped_away, tris, tris_culled, parts, polys2d;
} bz_fstat_t;

/* Запись таблицы текстур в SDRAM (bz_front.c — о ключе и страницах) */
typedef struct
{
	int32_t     id;                /* ключ: номер картинки (0 — место пусто)                */
	uint32_t    name_hash;         /* ключ: свёртка имени                                    */
	int16_t     w, h;
	int16_t     w2, h2;            /* размер в SDRAM (TILED — степени двойки)               */
	uint8_t     lin, mip;
	uint32_t    addr;
} texent_t;

#define TEXHASH    4096
#define TEX_PAGE   (256u << 10)           /* самая большая картинка Quake 2 — 320×240, 75 КБ            */
#define TEX_PAGES  32                     /* 32 × 256 КБ = вся SDRAM — с запасом                        */

#define LM_SIZE     0x100000u       /* кольцо lightmap в SDRAM ядра — 1 МБ                    */
#ifndef BZ_LMHASH
#define BZ_LMHASH 4096
#endif
#define LMHASH BZ_LMHASH            /* мест в таблице lightmap (bz_front.c — почему прямое отображение) */

/* Запись — 36 байт вместо 56 («диета» 23.11, шаг 2): те же значения в узких полях — яркость стиля
 * (white·128, у Quake 2 до ~270), растяжение (до 1449/1024), шаг и строки (до 1024) влезают в 16 бит;
 * место в кольце — 64 бита двумя словами (запись выравнивается по 4, а не по 8 байт). */
typedef struct
{
	msurface_t *surf;
	uint32_t    pos_lo, pos_hi;    /* место в кольце (сквозной счёт байт)                 */
	uint32_t    addr;
	uint16_t    adj[MAXLIGHTMAPS];
	uint16_t    sw, sh;            /* растяжение текстуры (w2/w и h2/h в 1/1024)           */
	uint16_t    stride, rows;
	uint8_t     dlit;
} lment_t;

/* Пачка треугольников модели (bz_front.c — как Quake рисует md2 и зачем пачки) */
#define AB_MAX   512                    /* мест в буферах вершин и st адаптера (SPEC.md)            */
#define AB_TRIS  4096                   /* треугольников в пачке (у md2 их не больше 4096)          */
#define STH_N    1024                   /* хэш st: в 2 раза больше AB_MAX — короткие цепочки поиска */
#define STMAP_N  4096                   /* точек шкурки у md2 (у id MAX_VERTS 2048 — с запасом)      */

typedef struct
{
	finalvert_t *base;                  /* pfinalverts модели, что сейчас рисуется (NULL — нет)     */
	int          nbase;
	int          kind;                  /* вид закраски пачки (как у BZ_AliasTriangle)             */
	short        vmap[MAXALIASVERTS];   /* номер вершины id → её номер в пачке + 1 (0 — не взята)   */
	int          vlist[AB_MAX], nv;     /* номера вершин id в порядке пачки                         */
	uint32_t     stv[AB_MAX];  int nst; /* точки шкурки пачки: {s, t} по 16 бит                     */
	uint32_t     sth_key[STH_N];        /* хэш «точка шкурки → номер в пачке»                       */
	short        sth_idx[STH_N];
	int          sth_gen[STH_N], gen;   /* поколение: запись действительна, если gen совпадает      */
	uint32_t     tw[2 * AB_TRIS];       /* слова TRIS                                               */
	int          ntri;
	short        smap[STMAP_N];         /* номер точки шкурки md2 → её номер в пачке + 1 (BZ_AliasModelTris) */
	short        slist[AB_MAX];         /* номера точек шкурки md2 в порядке пачки (для сброса smap) */
	int          nsl;
} bz_ab_t;

/* Данные id для перевода вершин модели в экран (r_alias.c; у ref_esp они в bz_hot.c — p06_ref_esp.py) */
#ifdef BZ_ONLY
extern vec3_t   r_lerp_frontv, r_lerp_backv, r_lerp_move, r_plightvec;
extern float    r_avertexnormals[162][3];
extern float    aliastransform[3][4];
extern int      r_ambientlight;
extern float    r_shadelight;
extern float    s_ziscale;
#endif

/* ---------------------------------------------------------------- таблицы горячего модуля */

/* Меняется при любой правке таблиц или типов выше: модуль другой версии не подключится. */
#define Q2HOT_ABI 5   /* 4 (27.11.2026): bz_guard_left; 5 (28.11.2026): стороны cb_ctx/fs_ctx, ent_tex, ent_tick */

/* Размеры типов, которые видят обе сборки: модуль сверяет их со своими — разойтись они могут, только если
 * Q2.ELF и Q2HOT.ELF собраны из разных исходников (тогда модуль отказывается, игра идёт без него). */
#define Q2HOT_SIZES { sizeof(cbuf_t), sizeof(bz_fstat_t), sizeof(texent_t), sizeof(lment_t), sizeof(bz_ab_t), \
                      sizeof(bz_mapa_t), sizeof(image_t), sizeof(msurface_t), sizeof(mtexinfo_t), sizeof(model_t), \
                      sizeof(oldrefdef_t), sizeof(finalvert_t), sizeof(affinetridesc_t), sizeof(clipplane_t), \
                      sizeof(dmdl_t), sizeof(mplane_t) }
#define Q2HOT_NSIZES 16

/* Что модуль берёт у Q2.ELF: адреса данных и функций (заполняет bz_front.c, модуль копирует себе) */
typedef struct
{
	uint32_t      size, abi;
	uint32_t      sizes[Q2HOT_NSIZES];

	/* данные переднего края (bz_front.c) */
	cbuf_t      **cb_ctx;          /* [bz_ectx ()] — текущий буфер стороны */
	cbuf_t       *cb_sky;
	bz_fstat_t   *fs_ctx;          /* [bz_ectx ()] */
	bz_mapa_t    *mapa;
	bz_ab_t      *ab;
	texent_t     *texhash;
	uint32_t     *tex_page_frame;
	uint32_t     *tex_frame;
	image_t     **bz_alias_skin;
	int          *bz_guard_left;

	/* функции переднего края (bz_front.c) */
	void        (*put_full) (uint32_t v);
	texent_t   *(*tex_load) (image_t *img, int mip, int w, int h, int lin);
	int         (*image_id) (image_t *img);
	lment_t    *(*lm_build) (msurface_t *surf, bzr fx, bzr fy);
	lment_t      *lmhash;
	uint64_t     *lm_total;
	void        (*BZ_CalcGradients) (msurface_t *pface, int mip);
	texent_t   *(*ent_tex) (image_t *img, int mip, int w, int h, int lin);   /* сторона 1: текстура из таблицы кадра */
	volatile uint32_t *e_sp;      /* ESP: {начало, длина} стека задачи LINK — по нему bz_ectx (сторона 1) */
	void        (*ent_tick) (void);   /* сторона 1: дать задаче LINK отправить своё (не чаще ~150 мкс) */

	/* данные рендера id */
	model_t     **currentmodel;
	model_t     **r_worldmodel;
	refdef_t     *r_newrefdef;
	int          *r_framecount;
	mvertex_t   **r_pcurrentvertbase;
	clipplane_t  *view_clipplanes;
	float        *modelorg, *vright, *vup, *vpn, *transformed_modelorg;
	float        *xcenter, *ycenter, *xscale, *yscale, *xscaleinv, *yscaleinv, *scale_for_mip;
	oldrefdef_t  *r_refdef;
	float        *d_sdivzorigin, *d_sdivzstepu, *d_sdivzstepv;
	float        *d_tdivzorigin, *d_tdivzstepu, *d_tdivzstepv;
	int          *sadjust, *tadjust;
	int          *r_aliasblendcolor;
	affinetridesc_t *r_affinetridesc;
	entity_t    **ecur;             /* модель, что рисуется (копия currententity у моделей — bz_ent.h) */
	float        *r_lerp_frontv, *r_lerp_backv, *r_lerp_move, *r_plightvec;
	float       (*r_avertexnormals)[3];
	float       (*aliastransform)[4];
	int          *r_ambientlight;
	float        *r_shadelight, *s_ziscale;
	float        *aliasxscale, *aliasyscale, *aliasxcenter, *aliasycenter;

	/* функции рендера id */
	image_t    *(*R_TextureAnimation) (mtexinfo_t *tex);
	int         (*D_MipLevelForScale) (float scale);
	void        (*R_AliasClipTriangle) (finalvert_t *index0, finalvert_t *index1, finalvert_t *index2);
} q2hot_imp_t;

/* Что модуль отдаёт: горячие функции (у bz_front.c — те же, свои копии) */
typedef struct
{
	uint32_t size, abi;
	void (*face) (msurface_t *fa, int clipflags, int alpha);
	int  (*alias_tris) (void *pmdl, void *pfinalverts, int reverse, int kind);
	void (*particle) (int u, int v, float zi, int pix, int color, int level);
	void (*final_verts) (int numpoints, finalvert_t *fv, dtrivertx_t *oldv, dtrivertx_t *newv);
	void (*project_vert) (finalvert_t *fv);
} q2hot_exp_t;

/* Точка входа модуля (q2hot.c): main (3, {имя, imp, exp}) зовёт её; 0 — таблица заполнена. На ПК q2esp.exe
 * зовёт её сам (Q2M_HOT=1) — так сверяется сборка через таблицы. */
int q2hot_entry (const q2hot_imp_t *imp, q2hot_exp_t *exp);

#endif
