/*
 * =====================================================================================================
 * bz_hot.c — горячие функции переднего края «бомж-3D» (26.11.2026).
 * =====================================================================================================
 *
 * Перенесены из bz_front.c как есть. Не отдельная единица сборки: его подключают
 *   - bz_front.c (#include) — Q2.ELF, q2esp.exe, ref_bz.dll: функции и данные — те же, что были;
 *   - esp/hot/q2hot.c — горячий модуль Q2HOT.ELF (внутреннее ОЗУ ESP): перед этим файлом q2hot_remap.h
 *     переназначает имена данных и функций bz_front.c и рендера id на таблицу импорта (bz_int.h).
 * Поэтому здесь — только static-функции и никаких своих глобальных данных: всё общее — у bz_front.c.
 * Что сюда входит — по профилю Unreal NG (доля команд кадра): грань мира с отсечением и градиентами
 * (~37 %), треугольники моделей пачками (~23 %), поиск текстуры (~4 %), частицы (~1 %).
 */

/* Слово команды. Быстрый путь — место в буфере есть (26.11.2026: профиль Unreal NG — put 8 % времени кадра,
 * ~12 тыс. вызовов за кадр с проверками отправки и роста буфера на каждом); полный буфер — put_full. */
static inline void put (uint32_t v)
{
	if (cb->n < cb->cap)
		cb->w[cb->n++] = v;
	else
		put_full (v);
}

/* Место под count слов подряд в текущем буфере (27.11.2026: профиль Unreal NG — put в ab_flush и emit_poly ~3 %
 * команд кадра: на каждое слово — проверка места и чтение cb через таблицу модуля). Есть — указатель на них,
 * слова считаются записанными (писать все count); нет — NULL, писать по одному put (поток тот же). */
static inline uint32_t *put_span (uint32_t count)
{
	cbuf_t *c = cb;

	if (c->cap - c->n >= count)
	{
		uint32_t *w = c->w + c->n;
		c->n += count;
		return w;
	}
	return NULL;
}

static int ilog2 (int v)
{
	int l = 0;
	while ((1 << l) < v)
		l++;
	return l;
}

/* Есть ли у картинки мип-уровень mip: точки на месте или (отданы ядру) их было столько. */
static int image_has_mip (const image_t *img, int mip)
{
	return img->pixels[mip] != NULL || mip < img->bz_mips;
}

/* Отметить обращение к странице. Грань неба рисуется после всего мира (буфер неба), поэтому её страница
 * получает «следующий кадр» — давно не нужной она не окажется до конца этого кадра. */
static void tex_page_mark (int p)
{
	uint32_t f = cb == &cb_sky ? tex_frame + 1 : tex_frame;

	if (tex_page_frame[p] < f)
		tex_page_frame[p] = f;
}

/* Найти текстуру в таблице SDRAM ядра (её место и вид); нет — загрузить (tex_load в bz_front.c). */
static texent_t *tex_get_shared (image_t *img, int mip, int w, int h, int lin)
{
	int       id = img->bz_id ? img->bz_id : image_id (img);
	uint32_t  hsh = ((uint32_t)id * 2654435761u + (unsigned)(mip * 2 + lin)) & (TEXHASH - 1);
	uint32_t  nh = img->bz_name_hash;
	texent_t *e;

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
	return tex_load (img, mip, w, h, lin);
}

/* 28.11.2026: модели на ядре 0 (сторона 1) общую таблицу не трогают — её в это время меняет мир; их текстуры игра
 * загрузила заранее (BZ_EntBegin) и положила в таблицу кадра (ent_tex). */
static texent_t *tex_get (image_t *img, int mip, int w, int h, int lin)
{
	if (bz_ectx ())
		return ent_tex (img, mip, w, h, lin);
	return tex_get_shared (img, mip, w, h, lin);
}

/* Выдать TEX, только если она отличается от последней в этом буфере (см. cbuf_t). */
static void put_tex_words (uint32_t w0, uint32_t w1, uint32_t w2)
{
	if (cb->tex_ok && cb->tex[0] == w0 && cb->tex[1] == w1 && cb->tex[2] == w2)
		return;
	put (w0);
	put (w1);
	put (w2);
	cb->tex[0] = w0;
	cb->tex[1] = w1;
	cb->tex[2] = w2;
	cb->tex_ok = 1;
}

static void put_tex (const texent_t *e)
{
	if (e->lin)
		put_tex_words (((uint32_t)BZ_TEX << 24) | (1u << 16) | (uint32_t)e->w, e->addr,
		               (uint32_t)e->h | ((uint32_t)e->w << 16));
	else
		put_tex_words ((uint32_t)BZ_TEX << 24, e->addr,
		               (uint32_t)ilog2 (e->w2) | ((uint32_t)ilog2 (e->h2) << 4));
}

/* Сплошной цвет — «текстура» 1×1 в таблице solid (байт i = i). */
static void put_solid (int color)
{
	put_tex_words (((uint32_t)BZ_TEX << 24) | (1u << 16) | 1u, mapa.solid + (uint32_t)(color & 255),
	               1u | (1u << 16));
}

/* Вершина в координатах экрана id: точка (x, y) id — это «целая» координата x, у адаптера центр
 * точки x — это x + ½. Поэтому к координате id прибавляется ½. */
static uint32_t xy_soft (float u, float v)
{
	/* во float — те же числа, что в double: u, v < 1024, сложения и ·16 без округления */
#ifdef BZ_ONLY
	/* точки экрана не отрицательны (зажаты к окну): floor = отбросить дробь — одна команда FPU, без floorf */
	if (u >= 0 && v >= 0)
		return bz_xy ((int)((u + BZR(0.5)) * 16 + BZR(0.5)), (int)((v + BZR(0.5)) * 16 + BZR(0.5)));
#endif
	return bz_xy ((int)bz_floor ((u + BZR(0.5)) * 16 + BZR(0.5)), (int)bz_floor ((v + BZR(0.5)) * 16 + BZR(0.5)));
}

/* POLY с n вершинами (больше 16 — веером по 16: выпуклый многоугольник режется по диагоналям из
 * вершины 0, правила края у модели не дают ни щелей, ни двойных точек). g — градиенты
 * {C, A, B} уже в словах; ng — 3 или 4 (свет по вершинам). */
static void emit_poly (const uint32_t *xy, int n, int mode, const uint32_t g[4][3], int ng)
{
	int s = 1;

	while (s < n - 1)
	{
		int e = s + 14 < n - 1 ? s + 14 : n - 1, k, a, b;
		int m = e - s + 2;
		uint32_t *w = put_span ((uint32_t)(1 + m + 3 * ng));

		if (w)
		{
			*w++ = ((uint32_t)BZ_POLY << 24) | ((uint32_t)mode << 8) | (uint32_t)m;
			*w++ = xy[0];
			for (k = s; k <= e; k++)
				*w++ = xy[k];
			for (a = 0; a < ng; a++)
			{
				w[0] = g[a][0];
				w[1] = g[a][1];
				w[2] = g[a][2];
				w += 3;
			}
		}
		else
		{
			put (((uint32_t)BZ_POLY << 24) | ((uint32_t)mode << 8) | (uint32_t)m);
			put (xy[0]);
			for (k = s; k <= e; k++)
				put (xy[k]);
			for (a = 0; a < ng; a++)
				for (b = 0; b < 3; b++)
					put (g[a][b]);
		}
		s = e;
	}
}

/* Градиенты перспективы в слова: zi (Q2.30) и su = (s/z + K·zi)·f (Q12.20). sdivz — то, что
 * считает D_CalcGradients id, K — постоянная часть s (sadjust/65536 + сдвиг до текстуры), f —
 * растяжение до степени двойки. */
static void persp_grads (uint32_t g[4][3], bzr kz[3], bzr ks, bzr kt, bzr fx, bzr fy,
                         bzr sd[3], bzr td[3])
{
	int i;
	for (i = 0; i < 3; i++)
	{
		g[0][i] = fx32 (kz[i] * BZR(1073741824.0));
		g[1][i] = fx32 ((sd[i] + ks * kz[i]) * fx * BZR(1048576.0));
		g[2][i] = fx32 ((td[i] + kt * kz[i]) * fy * BZR(1048576.0));
	}
}

/* Отсечение многоугольника плоскостью: остаётся сторона dot(v, n) − d ≥ 0. */
static int clip_plane (vec3_t *in, int n, vec3_t *out, const float *normal, float dist)
{
	int   i, m = 0;
	float d0, d1, dfirst;

	/* 26.11.2026: расстояние каждой вершины — один раз (было по два на вершину и деление на n в каждом шаге;
	 * профиль Unreal NG — clip_plane 8 % времени кадра). Числа те же: то же выражение для той же вершины. */
	dfirst = d1 = DotProduct (in[0], normal) - dist;
	for (i = 0; i < n; i++)
	{
		float *p = in[i], *q = i + 1 < n ? in[i + 1] : in[0];
		d0 = d1;
		d1 = i + 1 < n ? DotProduct (q, normal) - dist : dfirst;
		if (d0 >= 0)
		{
			VectorCopy (p, out[m]);	/* макрос id: m++ внутри подставился бы трижды */
			m++;
		}
		if ((d0 >= 0) != (d1 >= 0))
		{
			float f = d0 / (d0 - d1);
			out[m][0] = p[0] + f * (q[0] - p[0]);
			out[m][1] = p[1] + f * (q[1] - p[1]);
			out[m][2] = p[2] + f * (q[2] - p[2]);
			m++;
		}
	}
	return m;
}

/* Метки времени (BZ_PROF_BEGIN/END) — bz_int.h */

/* Место грани в таблице lightmap (bz_front.c — почему прямое отображение) */
static uint32_t lm_slot (const msurface_t *surf)
{
	if (r_worldmodel && surf >= r_worldmodel->surfaces &&
	    surf < r_worldmodel->surfaces + r_worldmodel->numsurfaces)
		return (uint32_t)(surf - r_worldmodel->surfaces) & (LMHASH - 1);
	return (uint32_t)(((uintptr_t)surf >> 3) * 2654435761u) & (LMHASH - 1);
}

/* Lightmap грани: запись годна (тот же стиль света, нет вспышек, растяжение то же, ещё в кольце) — она;
 * иначе — собрать (lm_build, bz_front.c: та же проверка и сборка). */
static lment_t *lm_get (msurface_t *surf, bzr fx, bzr fy)
{
	lment_t *e = &lmhash[lm_slot (surf)];
	int      adj[MAXLIGHTMAPS], i, sw = (int)(fx * 1024 + 0.5f), sh = (int)(fy * 1024 + 0.5f);
	int      dlit = surf->dlightframe == r_framecount;
	int      fits = 1;              /* все яркости влезают в 16 бит записи — иначе не кэшировать */
	/* 28.11.2026: «+ 0.5f» вместо «+ 0.5» (double у ESP — программный): fx·1024 < 2^22 — сумма во float точна */

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
	return lm_build (surf, fx, fy);
}

/* Поворот в пространство вида — TransformVector id (r_misc.c), то же выражение: своя копия, чтобы горячий
 * модуль не ходил за ней в код основного (PSRAM). */
static void hot_transform (const float *in, float *out)
{
	out[0] = DotProduct (in, vright);
	out[1] = DotProduct (in, vup);
	out[2] = DotProduct (in, vpn);
}

/* 28.11.2026 («разгрузить тяжёлые кадры»): вид — оси, точка взгляда, центр и масштаб экрана — в локальной копии на
 * грань. В горячем модуле это чтения через таблицу импорта, и с -fno-strict-aliasing компилятор перечитывал их после
 * каждой записи вершины. Выражения те же: (w − modelorg) и DotProduct с vright, vup, vpn в том же порядке. */
typedef struct
{
	float o[3], r[3], u[3], f[3];
	float xc, xs, yc, ys;
} hot_view_t;

static inline void hot_view_load (hot_view_t *hv)
{
	int k;

	for (k = 0; k < 3; k++)
	{
		hv->o[k] = modelorg[k];
		hv->r[k] = vright[k];
		hv->u[k] = vup[k];
		hv->f[k] = vpn[k];
	}
	hv->xc = xcenter; hv->xs = xscale;
	hv->yc = ycenter; hv->ys = yscale;
}

/* точка мира → пространство вида: VectorSubtract (w, modelorg) и hot_transform */
static inline void hot_view_point (const hot_view_t *hv, const float *w, float *out)
{
	const float l0 = w[0] - hv->o[0], l1 = w[1] - hv->o[1], l2 = w[2] - hv->o[2];

	out[0] = l0 * hv->r[0] + l1 * hv->r[1] + l2 * hv->r[2];
	out[1] = l0 * hv->u[0] + l1 * hv->u[1] + l2 * hv->u[2];
	out[2] = l0 * hv->f[0] + l1 * hv->f[1] + l2 * hv->f[2];
}

/*
 * Грань, задетая краями окна (clipflags ≠ 0), — без отсечения плоскостями пирамиды вида (27.11.2026, «нужно хотя
 * бы 30 fps»; профиль Unreal NG: clip_plane — 6 % команд кадра). Вершины переводятся в пространство вида, режутся
 * только ближней плоскостью и проецируются без зажима к окну; остальное отрежет ядро ножницами окна вида
 * (BZ_Begin3D — те же края). Как у треугольников моделей: вершины — в пределах ±FACE_GUARD точек (у ядра
 * u·16 + 8 — int16), иначе — прежний путь.
 * Уровень текстуры id выбирает по ближней видимой точке грани (nearzi — по вершинам после отсечения). Без
 * отсечения ближняя вершина может лежать за краем окна — грань взяла бы более мелкий уровень (другая картинка и
 * лишняя работа ядра). Поэтому две оценки: сверху — по всем вершинам (видимая часть внутри грани), снизу — по
 * вершинам внутри окна и углам окна внутри грани (1/z на экране линейна, z угла — по плоскости грани, как в
 * bz_face). Уровни разные или видимой точки не нашлось — прежний путь: там всё как у id.
 * 1 — xy[], *nout и *nearzi готовы; 0 — грань вести прежним путём.
 */
#define FACE_GUARD 2000.0f
static int face_guard (const hot_view_t *hv, msurface_t *fa, vec3_t *wv, int n, int needs_mip, uint32_t *xy, int *nout,
                       float *nearzi)
{
	static vec3_t ga[MAXFV], gb[MAXFV];
	static const float zn[3] = { 0, 0, 1 };
	vec3_t *in = ga, *out = gb, *t;
	float   su[MAXFV][2];
	float   hi = 0, lo = 0;
	int     have_lo = 0, all_front = 1, i, k;
	const float x0 = r_refdef.fvrectx_adj, x1 = r_refdef.fvrectright_adj;
	const float y0 = r_refdef.fvrecty_adj, y1 = r_refdef.fvrectbottom_adj;
	float   area = 0;
	const float gl = (float)bz_guard_left;

	for (i = 0; i < n; i++)
	{
		hot_view_point (hv, wv[i], in[i]);
		if (!(in[i][2] - BZ_NEAR >= 0))
			all_front = 0;
	}
	if (!all_front)
	{
		n = clip_plane (in, n, out, zn, BZ_NEAR);
		t = in; in = out; out = t;
		if (n < 3)
			return 0;
	}
	for (i = 0; i < n; i++)
	{
		float zi = 1.0f / in[i][2], u, v;
		u = hv->xc + hv->xs * zi * in[i][0];
		v = hv->yc - hv->ys * zi * in[i][1];
		/* x ≥ bz_guard_left (bz_front.c): ядро 0.03 левый край отрезка зажимало к ножницам беззнаковым сравнением
		 * (bz_raster.sv: «l < scx0[15:0]» — выборка части беззнаковая) и отрезки левее окна теряло (стенд tb_raster,
		 * трасса demo1 с этим путём: кадр 124, l = −51; на плате — пропавшие стены, board_log30). Исправлено в 0.04. */
		if (!(u >= gl && u < FACE_GUARD && v > -FACE_GUARD && v < FACE_GUARD))
			return 0;
		su[i][0] = u;
		su[i][1] = v;
		if (zi > hi)
			hi = zi;
		if (u >= x0 && u <= x1 && v >= y0 && v <= y1)
		{
			if (zi > lo)
				lo = zi;
			have_lo = 1;
		}
	}
	/* углы окна внутри грани: грань выпуклая, обход любой — знак площади, затем все рёбра по одну сторону */
	for (i = 0; i < n; i++)
	{
		const float *p = su[i], *q = su[i + 1 < n ? i + 1 : 0];
		area += p[0] * q[1] - q[0] * p[1];
	}
	{
		bzr kz[3] = { 0, 0, 0 };

		if (needs_mip)
		{
			vec3_t p_normal;
			bzr    distinv;

			hot_transform (fa->plane->normal, p_normal);
			distinv = BZR(1.0) / (fa->plane->dist - DotProduct (modelorg, fa->plane->normal));
			kz[1] = p_normal[0] * xscaleinv * distinv;
			kz[2] = -p_normal[1] * yscaleinv * distinv;
			kz[0] = p_normal[2] * distinv - xcenter * kz[1] - ycenter * kz[2];
		}
		for (k = 0; k < 4; k++)
		{
			const float cx = (k & 1) ? x1 : x0, cy = (k & 2) ? y1 : y0;
			int         inside = 1;

			for (i = 0; i < n && inside; i++)
			{
				const float *p = su[i], *q = su[i + 1 < n ? i + 1 : 0];
				const float  c = (q[0] - p[0]) * (cy - p[1]) - (q[1] - p[1]) * (cx - p[0]);
				if (area > 0 ? c < 0 : c > 0)
					inside = 0;
			}
			if (inside)
			{
				const float zi = needs_mip ? (float)(kz[0] + kz[1] * cx + kz[2] * cy) : 0;
				if (zi > lo)
					lo = zi;
				have_lo = 1;
			}
		}
	}
	if (!have_lo)
		return 0;                               /* видимой точки не нашлось — пусть решит прежний путь */
	if (needs_mip)
	{
		const float s = scale_for_mip * fa->texinfo->mipadjust;
		if (D_MipLevelForScale (hi * s) != D_MipLevelForScale (lo * s))
			return 0;
	}
	for (i = 0; i < n; i++)
		xy[i] = xy_soft (su[i][0], su[i][1]);
	*nout = n;
	*nearzi = lo;
	return 1;
}

/* (int)(v + 0.5) id, где сумма — в double (v — float): то же число без double. |v| < 2^23 — сумма во float точна;
 * больше — v целое: v ≥ 0 — само v, v < 0 — округление к нулю даёт v + 1. */
static inline int hot_round_half (float v)
{
	if (v < 8388608.0f && v > -8388608.0f)
		return (int)(v + 0.5f);
	return (int)v + (v < 0);
}

/*
 * D_CalcGradients id (r_edge.c) — для грани и уровня mip: s/z, t/z по экрану и сдвиги s, t (28.11.2026). Своя копия
 * в горячем модуле: у id она в коде основного (PSRAM) и считает в double — «1.0 / (1 << mip)» и два «+ 0.5» с
 * переводом в целое на каждую грань; у ESP32-S3 double программный. Числа те же: 1/2^mip — степень двойки,
 * «+ 0.5» — hot_round_half. Поворот осей — hot_transform (то же выражение, что TransformVector id).
 */
static void hot_calc_gradients (msurface_t *pface, int mip, bzr sd[3], bzr td[3], int *psadj, int *ptadj)
{
	const mtexinfo_t *ti = pface->texinfo;
	const float       mipscale = 1.0f / (float)(1 << mip);
	vec3_t            p_temp1, p_saxis, p_taxis;
	float             t, sstepu, tstepu, sstepv, tstepv;
	int               sadj, tadj;

	hot_transform (ti->vecs[0], p_saxis);
	hot_transform (ti->vecs[1], p_taxis);

	t = xscaleinv * mipscale;
	sstepu = p_saxis[0] * t;
	tstepu = p_taxis[0] * t;

	t = yscaleinv * mipscale;
	sstepv = -p_saxis[1] * t;
	tstepv = -p_taxis[1] * t;

	sd[0] = p_saxis[2] * mipscale - xcenter * sstepu - ycenter * sstepv;
	td[0] = p_taxis[2] * mipscale - xcenter * tstepu - ycenter * tstepv;
	sd[1] = sstepu; sd[2] = sstepv;
	td[1] = tstepu; td[2] = tstepv;

	/* VectorScale id — функция основного (горячему модулю недоступна): то же выражение */
	p_temp1[0] = transformed_modelorg[0] * mipscale;
	p_temp1[1] = transformed_modelorg[1] * mipscale;
	p_temp1[2] = transformed_modelorg[2] * mipscale;

	t = 0x10000 * mipscale;
	sadj = (hot_round_half (DotProduct (p_temp1, p_saxis) * 0x10000) - ((pface->texturemins[0] << 16) >> mip))
	       + ti->vecs[0][3] * t;
	tadj = (hot_round_half (DotProduct (p_temp1, p_taxis) * 0x10000) - ((pface->texturemins[1] << 16) >> mip))
	       + ti->vecs[1][3] * t;

	// PGM - changing flow speed for non-warping textures.
	if (ti->flags & SURF_FLOWING)
	{
		if (ti->flags & SURF_WARP)
			sadj += 0x10000 * (-128 * ( (r_newrefdef.time * 0.25) - (int)(r_newrefdef.time * 0.25) ));
		else
			sadj += 0x10000 * (-128 * ( (r_newrefdef.time * 0.77) - (int)(r_newrefdef.time * 0.77) ));
	}
	*psadj = sadj;
	*ptadj = tadj;
}

/* Грань мира, модели-кисти или неба: alpha 0, 33 или 66. */
static void bz_face (msurface_t *fa, int clipflags, int alpha)
{
	static vec3_t va[MAXFV], vb[MAXFV];
	vec3_t   *in = va, *out = vb, *t;
	float     su[MAXFV][2];
	uint32_t  xy[MAXFV], g[4][3];
	int       n, i, mip, mode, light;
	float     nearzi = 0;
	image_t  *image;
	medge_t  *pedges = currentmodel->edges;
	texent_t *te;
	vec3_t    p_normal;
	bzr       distinv, kz[3], sd[3], td[3], ks, kt, fx, fy;
	int       offs = 0, offt = 0, sadj, tadj;
	hot_view_t hv;

	fs.faces++;
	n = fa->numedges;
	if (n < 3 || n > MAXFV - 8)
		return;
	hot_view_load (&hv);
	for (i = 0; i < n; i++)
	{
		int li = currentmodel->surfedges[fa->firstedge + i];
		float *pos = li > 0 ? r_pcurrentvertbase[pedges[li].v[0]].position
		                    : r_pcurrentvertbase[pedges[-li].v[1]].position;
		VectorCopy (pos, in[i]);
	}
	/* 27.11.2026: задетая краями окна — сначала без отсечения пирамидой вида (face_guard, выше). Первая версия
	 * (board_log30) пускала вершины и левее окна — ядро 0.03 теряло такие отрезки (пропавшие стены); теперь x ≥ 0. */
	if (clipflags && face_guard (&hv, fa, in, n, !(fa->flags & (SURF_DRAWSKYBOX | SURF_DRAWTURB)) && !alpha,
	                             xy, &n, &nearzi))
		goto projected;
	/* 1. плоскости пирамиды вида, как R_RenderFace (бит i — плоскость i) */
	for (i = 0; i < 4 && n >= 3; i++)
		if (clipflags & (1 << i))
		{
			n = clip_plane (in, n, out, view_clipplanes[i].normal, view_clipplanes[i].dist);
			t = in; in = out; out = t;
		}
	if (n < 3)
	{
		fs.faces_clipped_away++;
		return;
	}
	/* 2. в пространство вида и ближняя плоскость */
	{
		int all_front = 1;

		for (i = 0; i < n; i++)
		{
			hot_view_point (&hv, in[i], out[i]);
			if (!(out[i][2] - BZ_NEAR >= 0))
				all_front = 0;
		}
		t = in; in = out; out = t;
		/* 26.11.2026: все вершины перед ближней плоскостью (почти всегда) — clip_plane вернул бы их же в том
		 * же порядке (то же сравнение: 0·x + 0·y + 1·z − d ≥ 0), её можно не звать */
		if (!all_front)
		{
			static const float zn[3] = { 0, 0, 1 };
			n = clip_plane (in, n, out, zn, BZ_NEAR);
			t = in; in = out; out = t;
		}
	}
	if (n < 3)
	{
		fs.faces_clipped_away++;
		return;
	}
	/* 3. проекция и зажим к краям окна, как R_EmitEdge */
	{
	const float ex0 = r_refdef.fvrectx_adj, ex1 = r_refdef.fvrectright_adj;
	const float ey0 = r_refdef.fvrecty_adj, ey1 = r_refdef.fvrectbottom_adj;
	for (i = 0; i < n; i++)
	{
		float zi = 1.0f / in[i][2], u, v;
		u = hv.xc + hv.xs * zi * in[i][0];
		v = hv.yc - hv.ys * zi * in[i][1];
		if (u < ex0) u = ex0;
		if (u > ex1) u = ex1;
		if (v < ey0) v = ey0;
		if (v > ey1) v = ey1;
		if (zi > nearzi)
			nearzi = zi;
		su[i][0] = u;
		su[i][1] = v;
		xy[i] = xy_soft (u, v);
	}
	}

projected:
	/* 4. вид грани: как выбирают D_DrawSurfaces и R_BuildPolygonFromSurface id */
	if (fa->flags & SURF_DRAWSKYBOX)
	{
		mip = 0;
		image = fa->texinfo->image;
		mode = BZ_M_PERSP | BZ_L_NONE | BZ_M_SKY;
	}
	else if (alpha)
	{
		mip = 0;
		mode = BZ_M_PERSP | BZ_M_ZTEST | (alpha == 66 ? BZ_T_66 : BZ_T_33);
		if (fa->texinfo->flags & SURF_WARP)
		{
			/* прозрачная вода id берёт координаты текстуры, а не кэша поверхности */
			image = fa->texinfo->image;
			mode |= BZ_L_NONE | BZ_M_WARP;
			offs = fa->texturemins[0] & 127;
			offt = fa->texturemins[1] & 127;
		}
		else
		{
			image = R_TextureAnimation (fa->texinfo);
			mode |= BZ_L_LMAP;
		}
	}
	else if (fa->flags & SURF_DRAWTURB)
	{
		mip = 0;
		image = fa->texinfo->image;
		mode = BZ_M_PERSP | BZ_L_NONE | BZ_M_ZTEST | BZ_M_ZWRITE | BZ_M_ZGT;
		if (fa->texinfo->flags & SURF_WARP)
			mode |= BZ_M_WARP;
	}
	else
	{
		mip = D_MipLevelForScale (nearzi * scale_for_mip * fa->texinfo->mipadjust);
		image = R_TextureAnimation (fa->texinfo);
		mode = BZ_M_PERSP | BZ_L_LMAP | BZ_M_ZTEST | BZ_M_ZWRITE | BZ_M_ZGT;
	}
	if (!image || !image_has_mip (image, mip))
		return;
	light = mode & BZ_M_LIGHT;

	/* 5. градиенты id: 1/z — из плоскости (как R_RenderFace), s/z и t/z — D_CalcGradients */
	hot_transform (fa->plane->normal, p_normal);
	distinv = BZR(1.0) / (fa->plane->dist - DotProduct (modelorg, fa->plane->normal));
	kz[1] = p_normal[0] * xscaleinv * distinv;
	kz[2] = -p_normal[1] * yscaleinv * distinv;
	kz[0] = p_normal[2] * distinv - xcenter * kz[1] - ycenter * kz[2];
	hot_transform (modelorg, transformed_modelorg);
	hot_calc_gradients (fa, mip, sd, td, &sadj, &tadj);

	BZ_PROF_BEGIN (Q2M_F_TEX);
	te = tex_get (image, mip, image->width >> mip, image->height >> mip, 0);
	BZ_PROF_END (Q2M_F_TEX);
	fx = (bzr)te->w2 / te->w;
	fy = (bzr)te->h2 / te->h;
	if (light == BZ_L_LMAP)
	{
		/* s кэша id отсчитан от texturemins; сдвиг до текстуры — по модулю её ширины */
		offs = ((fa->texturemins[0] >> mip) % te->w + te->w) % te->w;
		offt = ((fa->texturemins[1] >> mip) % te->h + te->h) % te->h;
	}
	ks = sadj / BZR(65536.0) + offs;
	kt = tadj / BZR(65536.0) + offt;
	persp_grads (g, kz, ks, kt, fx, fy, sd, td);

	if (light == BZ_L_LMAP)
	{
		lment_t *lm;
		uint32_t w[5], k;

		BZ_PROF_BEGIN (Q2M_F_LM);
		lm = lm_get (fa, fx, fy);
		BZ_PROF_END (Q2M_F_LM);
		w[0] = (uint32_t)BZ_LMAP << 24;
		w[1] = lm->addr;
		w[2] = (uint32_t)lm->stride | ((uint32_t)lm->rows << 16) | ((uint32_t)(4 - mip) << 24);
		w[3] = fx32 (offs * fx * BZR(65536.0));
		w[4] = fx32 (offt * fy * BZR(65536.0));
		/* LMAP — тоже только при смене (грань из нескольких кусков веера, повтор после неба) */
		if (!cb->lm_ok || memcmp (cb->lm, w, sizeof(w)))
		{
			for (k = 0; k < 5; k++)
				put (w[k]);
			memcpy (cb->lm, w, sizeof(w));
			cb->lm_ok = 1;
		}
	}
	put_tex (te);
	emit_poly (xy, n, mode, (const uint32_t (*)[3])g, 3);
}

static int alias_mode (int kind)
{
	switch (kind)
	{
	case 1:  return BZ_L_VERT | BZ_M_ZTEST | BZ_T_33;
	case 2:  return BZ_L_VERT | BZ_M_ZTEST | BZ_M_ZWRITE | BZ_T_66;
	case 3:  return BZ_L_NONE | BZ_M_ZTEST | BZ_T_33;
	case 4:  return BZ_L_NONE | BZ_M_ZTEST | BZ_T_66;
	default: return BZ_L_VERT | BZ_M_ZTEST | BZ_M_ZWRITE;
	}
}

/* Текстура для треугольников модели: шкурка или (оболочка «quad damage» и т. п.) сплошной цвет. */
static void alias_tex (int kind)
{
	if (kind >= 3)
		put_solid (r_aliasblendcolor);
	else
		put_tex (tex_get (bz_alias_skin, 0, r_affinetridesc.skinwidth, r_affinetridesc.skinheight, 1));
}

/* Отправить накопленную пачку: TEX, VBUF, STBUF, TRIS (по 255 треугольников в команде). */
static void ab_flush (void)
{
	int k, s;

	if (ab.ntri)
	{
		/* 27.11.2026: вся пачка — одним куском буфера, если он есть (тот же поток слов, что ниже по одному) */
		bz_ab_t        *a = &ab;
		const int       nv = a->nv, nst = a->nst, ntri = a->ntri;
		const uint32_t  need = 2u + 3u * (uint32_t)nv + (uint32_t)nst + 2u * (uint32_t)ntri + (uint32_t)(ntri + 254) / 255u;
		uint32_t       *w;

		alias_tex (a->kind);
		w = put_span (need);
		if (w)
		{
			const finalvert_t *base = a->base;
			const uint32_t     tris = ((uint32_t)BZ_TRIS << 24) | ((uint32_t)alias_mode (a->kind) << 8);

			*w++ = ((uint32_t)BZ_VBUF << 24) | (uint32_t)nv;
			for (k = 0; k < nv; k++)
			{
				const finalvert_t *fv = base + a->vlist[k];
				w[0] = bz_xy (fv->u, fv->v);
				w[1] = (uint32_t)(fv->zi >> 1);
				w[2] = (uint32_t)fv->l;
				w += 3;
			}
			*w++ = ((uint32_t)BZ_STBUF << 24) | (uint32_t)nst;
			memcpy (w, a->stv, (size_t)nst * sizeof(uint32_t));
			w += nst;
			for (s = 0; s < ntri; s += 255)
			{
				const int m = ntri - s < 255 ? ntri - s : 255;
				*w++ = tris | (uint32_t)m;
				memcpy (w, &a->tw[2 * s], (size_t)(2 * m) * sizeof(uint32_t));
				w += 2 * m;
			}
			goto reset;
		}
		put (((uint32_t)BZ_VBUF << 24) | (uint32_t)ab.nv);
		for (k = 0; k < ab.nv; k++)
		{
			const finalvert_t *fv = ab.base + ab.vlist[k];
			put (bz_xy (fv->u, fv->v));
			put ((uint32_t)(fv->zi >> 1));        /* 1/z·2^31 у id → Q2.30 */
			put ((uint32_t)fv->l);
		}
		put (((uint32_t)BZ_STBUF << 24) | (uint32_t)ab.nst);
		for (k = 0; k < ab.nst; k++)
			put (ab.stv[k]);
		for (s = 0; s < ab.ntri; s += 255)
		{
			const int m = ab.ntri - s < 255 ? ab.ntri - s : 255;
			put (((uint32_t)BZ_TRIS << 24) | ((uint32_t)alias_mode (ab.kind) << 8) | (uint32_t)m);
			for (k = 0; k < 2 * m; k++)
				put (ab.tw[2 * s + k]);
		}
	}
reset:
	for (k = 0; k < ab.nv; k++)
		ab.vmap[ab.vlist[k]] = 0;
	for (k = 0; k < ab.nsl; k++)
		ab.smap[ab.slist[k]] = 0;
	ab.nv = ab.nst = ab.ntri = ab.nsl = 0;
	ab.gen++;
	ent_tick ();                         /* сторона 1: задача LINK — своё (28.11.2026) */
}

/*
 * Все треугольники модели — врезка в R_AliasPreparePoints вместо цикла id (26.11.2026). Цикл id писал s, t в
 * вершины каждого треугольника и звал R_DrawTriangle → BZ_AliasTriangle, а тот искал вершину в пачке по
 * указателю, а точку шкурки — хэшем по {s, t}; профиль Unreal NG: эта цепочка ~30 % команд кадра. Здесь
 * вершина и точка шкурки берутся в пачку по своим номерам md2 (index_xyz, index_st): две таблицы без поиска.
 * Отбор и порядок треугольников — как у цикла id и bz_alias_triangle: полностью за краем — мимо; задетый
 * краем — s, t в вершины и R_AliasClipTriangle id (он отправит пачку и пойдёт прежним POLY); у оружия в левой
 * руке (reverse) вершины в обратном порядке. Точки шкурки с равными {s, t}, но разными номерами теперь идут
 * в STBUF раздельно — картинка та же (ПК: mcrc кадров).
 * 0 — не взялись (точек шкурки больше таблицы): треугольники ведёт цикл id, как раньше.
 */
#define AB_GUARD 2000
static inline int ab_in_guard (const finalvert_t *v)
{
	/* u ≥ bz_guard_left: левее окна ядро 0.03 отрезок теряло (см. face_guard) */
	return v->u >= bz_guard_left && v->u < AB_GUARD && v->v > -AB_GUARD && v->v < AB_GUARD;
}

static int bz_alias_model_tris (void *pmdl, void *pfinalverts, int reverse, int kind)
{
	dmdl_t            *mdl = (dmdl_t *)pmdl;
	finalvert_t       *fv = (finalvert_t *)pfinalverts;
	const dstvert_t   *st = (const dstvert_t *)((byte *)mdl + mdl->ofs_st);
	const dtriangle_t *tri = (const dtriangle_t *)((byte *)mdl + mdl->ofs_tris);
	int                t, n = mdl->num_tris;
	/* 27.11.2026 (профиль Unreal NG: эта функция — 19 % команд кадра): состояние пачки — в локальных переменных
	 * (в модуле ab — через таблицу импорта, и компилятор перечитывал поля на каждом треугольнике), счётчики
	 * кадра — локально, порядок вершин у оружия в левой руке — номерами, без ветвления. Перед вызовами, которые
	 * сами трогают пачку (ab_flush, R_AliasClipTriangle), состояние пишется в ab и потом читается заново. */
	bz_ab_t           *a = &ab;
	short             *vmap = a->vmap, *smap = a->smap, *slist = a->slist;
	int               *vlist = a->vlist;
	uint32_t          *stv = a->stv, *tw = a->tw;
	int                nv, nst, ntri, nsl, ntris = 0, nculled = 0;
	const int          o0 = reverse ? 2 : 0, o2 = reverse ? 0 : 2;

	if (mdl->num_st > STMAP_N || mdl->num_xyz > MAXALIASVERTS)
		return 0;
	ab_flush ();
	a->kind = kind;
	nv = a->nv; nst = a->nst; ntri = a->ntri; nsl = a->nsl;
	for (t = 0; t < n; t++, tri++)
	{
		const int    x0 = tri->index_xyz[o0], x1 = tri->index_xyz[1], x2 = tri->index_xyz[o2];
		const int    s0 = tri->index_st[o0], s1 = tri->index_st[1], s2 = tri->index_st[o2];
		finalvert_t *p0 = fv + x0, *p1 = fv + x1, *p2 = fv + x2;
		const int    f0 = p0->flags, f1 = p1->flags, f2 = p2->flags;
		int          v0, v1, v2, q0, q1, q2;

		if (!(t & 255))
			ent_tick ();                         /* сторона 1: задача LINK — своё (28.11.2026) */
		if (f0 & f1 & f2)
			continue;                           /* весь за одним краем */
		/* 26.11.2026 («нужно хотя бы 30 fps»; плата: треугольники моделей ~6,5 мс кадра): задетый только краем
		 * экрана (не ближней плоскостью) — в пачку целиком, ядро режет его ножницами окна вида (BZ_Begin3D: те
		 * же края, что aliasvrect у id). Так чаще всего — оружие у нижнего края. Раньше — отсечение id и POLY
		 * по 16 слов на кусок. Вершины — в пределах ±AB_GUARD точек: у ядра u·16 + 8 — int16, делитель TRIS —
		 * 48 бит (gpu/rtl/bz_core2.sv); дальше — прежний путь. */
		if ((f0 | f1 | f2) &&
		    (((f0 | f1 | f2) & ALIAS_Z_CLIP) || !ab_in_guard (p0) || !ab_in_guard (p1) || !ab_in_guard (p2)))
		{
			/* задет краем: как цикл id — s, t в вершины, отсечение id (его треугольники — через R_DrawTriangle) */
			p0->s = st[s0].s << 16;
			p0->t = st[s0].t << 16;
			p1->s = st[s1].s << 16;
			p1->t = st[s1].t << 16;
			p2->s = st[s2].s << 16;
			p2->t = st[s2].t << 16;
			a->nv = nv; a->nst = nst; a->ntri = ntri; a->nsl = nsl;
			R_AliasClipTriangle (p0, p1, p2);
			a->kind = kind;                     /* пачку R_AliasClipTriangle уже отправил */
			nv = a->nv; nst = a->nst; ntri = a->ntri; nsl = a->nsl;
			continue;
		}
		/* отбор как в R_DrawTriangle id (и bz_alias_triangle): вырожденные и обращённые спиной — мимо */
		{
			const int dv0_ab = p0->u - p1->u, dv1_ab = p0->v - p1->v;
			const int dv0_ac = p0->u - p2->u, dv1_ac = p0->v - p2->v;

			if (!(dv0_ab | dv1_ab) || !(dv0_ac | dv1_ac))
				continue;
			ntris++;
			if (dv0_ac * dv1_ab - dv0_ab * dv1_ac >= 0)
			{
				nculled++;
				continue;
			}
		}
		if (nv + 3 > AB_MAX || nst + 3 > AB_MAX || ntri == AB_TRIS)
		{
			a->nv = nv; a->nst = nst; a->ntri = ntri; a->nsl = nsl;
			ab_flush ();
			nv = a->nv; nst = a->nst; ntri = a->ntri; nsl = a->nsl;
		}
		/* вершина и точка шкурки — в пачку по номеру md2 (порядок — как прежний цикл по i = 0, 1, 2) */
#define AB_TAKE(x, sx, v, q) \
		do { \
			if (!vmap[x]) { vlist[nv] = (x); vmap[x] = (short)(++nv); } \
			v = vmap[x] - 1; \
			if (!smap[sx]) \
			{ \
				slist[nsl++] = (short)(sx); \
				stv[nst] = (uint32_t)(uint16_t)st[sx].s | ((uint32_t)(uint16_t)st[sx].t << 16); \
				smap[sx] = (short)(++nst); \
			} \
			q = smap[sx] - 1; \
		} while (0)
		AB_TAKE (x0, s0, v0, q0);
		AB_TAKE (x1, s1, v1, q1);
		AB_TAKE (x2, s2, v2, q2);
#undef AB_TAKE
		tw[2 * ntri] = (uint32_t)v0 | ((uint32_t)v1 << 9) | ((uint32_t)v2 << 18);
		tw[2 * ntri + 1] = (uint32_t)q0 | ((uint32_t)q1 << 9) | ((uint32_t)q2 << 18);
		ntri++;
	}
	a->nv = nv; a->nst = nst; a->ntri = ntri; a->nsl = nsl;
	fs.tris += (uint64_t)ntris;
	fs.tris_culled += (uint64_t)nculled;
	return 1;
}

/* Частица: u, v, размер и уровень прозрачности посчитал R_DrawParticle id. */
static void bz_particle (int u, int v, float zi, int pix, int color, int level)
{
	int trans = level == 0 ? 1 : level == 1 ? 2 : 0;        /* PARTICLE_33, _66, _OPAQUE */

	if (pix > 15)
		pix = 15;
	put (((uint32_t)BZ_POINT << 24) | ((uint32_t)trans << 12) | ((uint32_t)pix << 8) | (uint32_t)(color & 255));
	put (bz_xy (u, v));
	put (fx32 (zi * BZR(1073741824.0)));
	fs.parts++;
}

#ifdef BZ_ONLY
/*
 * Перевод вершин модели в экран — R_AliasTransformFinalVerts и R_AliasProjectAndClipTestFinalVert id (r_alias.c,
 * C-версия), перенесены сюда тем же текстом (26.11.2026; у ref_esp в r_alias.c их больше нет — p06_ref_esp.py).
 * Профиль Unreal NG: ~11 % команд кадра. Имена id даёт bz_front.c: своя копия или копия горячего модуля.
 */
static void hot_project_final_vert( finalvert_t *fv )
{
	float	zi;
	float	x, y, z;

	// project points
	x = fv->xyz[0];
	y = fv->xyz[1];
	z = fv->xyz[2];
	zi = 1.0f / z;          /* 28.11.2026: во float — то же частное, что у id в double (см. hot_final_verts) */

	fv->zi = zi * s_ziscale;

	fv->u = (x * aliasxscale * zi) + aliasxcenter;
	fv->v = (y * aliasyscale * zi) + aliasycenter;

	if (fv->u < r_refdef.aliasvrect.x)
		fv->flags |= ALIAS_LEFT_CLIP;
	if (fv->v < r_refdef.aliasvrect.y)
		fv->flags |= ALIAS_TOP_CLIP;
	if (fv->u > r_refdef.aliasvrectright)
		fv->flags |= ALIAS_RIGHT_CLIP;
	if (fv->v > r_refdef.aliasvrectbottom)
		fv->flags |= ALIAS_BOTTOM_CLIP;
}

/*
 * 28.11.2026 («разгрузить тяжёлые кадры», плата board_log38/39: модели — 9–14 мс тяжёлого кадра; профиль Unreal NG:
 * этот цикл и проекция — ~10 % команд кадра). Тот же расчёт, что у id, но:
 *  - 1/z — во float: у id «1.0 / z» делилось в double, а у ESP32-S3 double — программный (__divdf3). Частное
 *    двух float, посчитанное в double и округлённое к float, равно частному во float: 53 ≥ 2·24 + 2, двойное
 *    округление безвредно (деление, сложение, умножение — правильно округлённые операции);
 *  - всё, что от вершины не зависит (сдвиг и веса кадров, поворот, свет, масштаб, края окна), — в локальных до
 *    цикла: в горячем модуле это чтения через таблицу импорта, и с -fno-strict-aliasing компилятор перечитывал их
 *    после каждой записи в fv. Выражения и их порядок — те же;
 *  - три прохода: поворот и свет → 1/z → проекция. Деление у ESP32-S3 — вызов __divsf3 (ПЗУ), а вызов у Xtensa не
 *    сохраняет регистры FPU: внутри общего цикла компилятор выгружал вокруг него в стек все живые float (поворот,
 *    свет, масштаб). Во втором проходе живого — только указатель и счётчик. Частное пока лежит в fv->zi битами
 *    float, третий проход кладёт туда же целое id.
 */
static void hot_final_verts( int numpoints, finalvert_t *fv, dtrivertx_t *oldv, dtrivertx_t *newv )
{
	const float mv0 = r_lerp_move[0], mv1 = r_lerp_move[1], mv2 = r_lerp_move[2];
	const float bk0 = r_lerp_backv[0], bk1 = r_lerp_backv[1], bk2 = r_lerp_backv[2];
	const float fr0 = r_lerp_frontv[0], fr1 = r_lerp_frontv[1], fr2 = r_lerp_frontv[2];
	const float m00 = aliastransform[0][0], m01 = aliastransform[0][1], m02 = aliastransform[0][2], m03 = aliastransform[0][3];
	const float m10 = aliastransform[1][0], m11 = aliastransform[1][1], m12 = aliastransform[1][2], m13 = aliastransform[1][3];
	const float m20 = aliastransform[2][0], m21 = aliastransform[2][1], m22 = aliastransform[2][2], m23 = aliastransform[2][3];
	const float lv0 = r_plightvec[0], lv1 = r_plightvec[1], lv2 = r_plightvec[2];
	const int   ambient = r_ambientlight;
	const float shade = r_shadelight;
	// PMM - added double damage shell
	const int   shell = ( e_currententity->flags & ( RF_SHELL_RED | RF_SHELL_GREEN | RF_SHELL_BLUE | RF_SHELL_DOUBLE | RF_SHELL_HALF_DAM) ) != 0;
	const float xs = aliasxscale, ys = aliasyscale, xc = aliasxcenter, yc = aliasycenter, zs = s_ziscale;
	const int   ed_l = r_refdef.aliasvrect.x, ed_t = r_refdef.aliasvrect.y;
	const int   ed_r = r_refdef.aliasvrectright, ed_b = r_refdef.aliasvrectbottom;
	float     (*normals)[3] = r_avertexnormals;
	finalvert_t *const fv0 = fv;
	int i;

	/* 1. поворот, свет, признак ближней плоскости */
	for ( i = 0; i < numpoints; i++, fv++, oldv++, newv++ )
	{
		int		temp;
		float	lightcos, *plightnormal;
		float   x, y, z;

		x = mv0 + oldv->v[0]*bk0 + newv->v[0]*fr0;
		y = mv1 + oldv->v[1]*bk1 + newv->v[1]*fr1;
		z = mv2 + oldv->v[2]*bk2 + newv->v[2]*fr2;

		plightnormal = normals[newv->lightnormalindex];

		if ( shell )
		{
			x += plightnormal[0] * POWERSUIT_SCALE;
			y += plightnormal[1] * POWERSUIT_SCALE;
			z += plightnormal[2] * POWERSUIT_SCALE;
		}

		{
			const float ez = x*m20 + y*m21 + z*m22 + m23;

			fv->xyz[0] = x*m00 + y*m01 + z*m02 + m03;
			fv->xyz[1] = x*m10 + y*m11 + z*m12 + m13;
			fv->xyz[2] = ez;
			fv->flags = ez < ALIAS_Z_CLIP_PLANE ? ALIAS_Z_CLIP : 0;
		}

		// lighting
		lightcos = plightnormal[0]*lv0 + plightnormal[1]*lv1 + plightnormal[2]*lv2;
		temp = ambient;

		if (lightcos < 0)
		{
			temp += (int)(shade * lightcos);

			// clamp; because we limited the minimum ambient and shading light, we
			// don't have to clamp low light, just bright
			if (temp < 0)
				temp = 0;
		}

		fv->l = temp;
	}

	/* 2. 1/z (R_AliasProjectAndClipTestFinalVert id) — биты float в fv->zi */
	for ( i = 0, fv = fv0; i < numpoints; i++, fv++ )
		if ( !fv->flags )
		{
			union { float f; int i; } c;     /* memcpy в горячем модуле — вызов, не встраивается */
			c.f = 1.0f / fv->xyz[2];
			fv->zi = c.i;
		}

	/* 3. проекция и края окна */
	for ( i = 0, fv = fv0; i < numpoints; i++, fv++ )
		if ( !fv->flags )
		{
			union { float f; int i; } c;
			float zi;
			int   u, v, flags = 0;

			c.i = fv->zi;
			zi = c.f;
			fv->zi = zi * zs;

			u = (fv->xyz[0] * xs * zi) + xc;
			v = (fv->xyz[1] * ys * zi) + yc;
			fv->u = u;
			fv->v = v;

			if (u < ed_l)
				flags |= ALIAS_LEFT_CLIP;
			if (v < ed_t)
				flags |= ALIAS_TOP_CLIP;
			if (u > ed_r)
				flags |= ALIAS_RIGHT_CLIP;
			if (v > ed_b)
				flags |= ALIAS_BOTTOM_CLIP;
			fv->flags = flags;
		}
}
#endif
