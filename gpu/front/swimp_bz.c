/*
 * swimp_bz.c — ref_bz: программный рендер без окна (как swimp_q2m.c) + передний край «бомж-3D».
 * Отличие от swimp_q2m.c — вызовы BZ_*: модель адаптера заводится вместе с видеорежимом, а в
 * конце кадра поток команд выполняется и кадр модели сравнивается с кадром id (bz_front.c).
 *
 * Ниже — исходный заголовок swimp_q2m.c.
 *
 * Заменяет win32/rw_imp.c, rw_dib.c, rw_ddraw.c. Кадр рисуется в обычную память
 * (8 бит на точку, как у id), на экран не выводится. Так меряется чистая цена
 * программного рендера (вариант «всё на ESP»), без вывода в окно Windows.
 */

#include "r_local.h"
#include "bz_front.h"

#include <stdio.h>

static byte *q2m_fb;

/*
 * Контрольная сумма каждого кадра (CRC-32 точек кадра и текущей палитры) — в <q2m_out>.crc.txt,
 * строкой «номер_кадра crc». Два прогона с одинаковыми суммами показали одинаковую картинку: так
 * проверяется, что «диета» памяти в port\ не меняет изображение ни на точку.
 */
static cvar_t  *q2m_out_cv, *q2m_frame_cv;
static FILE    *q2m_crc;
static unsigned q2m_crc_table[256];
static byte     q2m_palette[256 * 4];

static unsigned Q2M_Crc (unsigned crc, const byte *p, int n)
{
	while (n--)
		crc = q2m_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return crc;
}

int SWimp_Init (void *hInstance, void *wndProc)
{
	unsigned i, j, c;

	for (i = 0; i < 256; i++)
	{
		for (c = i, j = 0; j < 8; j++)
			c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		q2m_crc_table[i] = c;
	}
	q2m_out_cv = ri.Cvar_Get ("q2m_out", "", 0);
	q2m_frame_cv = ri.Cvar_Get ("q2m_frame", "0", 0);
	if (q2m_out_cv->string[0] && !q2m_crc)
	{
		char name[MAX_OSPATH + 16];

		Com_sprintf (name, sizeof(name), "%s.crc.txt", q2m_out_cv->string);
		q2m_crc = fopen (name, "w");
	}
	BZ_Init ();
	return true;
}

rserr_t SWimp_SetMode (int *pwidth, int *pheight, int mode, qboolean fullscreen)
{
	if (!ri.Vid_GetModeInfo (pwidth, pheight, mode))
		return rserr_invalid_mode;
	free (q2m_fb);
	q2m_fb = malloc ((*pwidth) * (*pheight));
#ifdef BZ_ONLY
	memset (q2m_fb, 0xA5, (*pwidth) * (*pheight));     /* образец: ref_esp в кадр id писать не должен */
#else
	memset (q2m_fb, 0, (*pwidth) * (*pheight));
#endif
	vid.buffer = q2m_fb;
	vid.rowbytes = *pwidth;
	BZ_SetMode (*pwidth, *pheight);
	ri.Vid_NewWindow (*pwidth, *pheight);
	R_GammaCorrectAndSetPalette ((const unsigned char *)d_8to24table);
	return rserr_ok;
}

void SWimp_SetPalette (const unsigned char *palette)
{
	if (palette)
		memcpy (q2m_palette, palette, sizeof(q2m_palette));
}

void SWimp_BeginFrame (float camera_separation)
{
}

#ifdef BZ_ONLY
/*
 * q2esp.exe (ref_esp на ПК): доказать, что рендер без рисования не трогает кадр id, z-буфер и кэш
 * поверхностей — тогда на ESP (BZ_ESP) их можно не заводить (~1,2 МБ PSRAM). Все три после выделения
 * заполнены образцом 0xA5 (кадр — здесь, z и кэш — в r_main.c/r_surf.c, patches/p06_ref_esp.py);
 * после каждого кадра сверяются. Первое расхождение каждого и итог каждые 500 кадров — в файл
 * <q2m_out>.canary.txt (не в консоль: строка консоли видна на экране, это лишние команды 2D в потоке, и
 * поток q2esp перестал бы совпадать с ref_bz).
 */
extern int   sc_size;           /* r_surf.c: размер кэша поверхностей */
extern int   numr_images;       /* r_image.c: занятых мест в r_images (больше не уменьшается)  */
extern int   mod_numknown;      /* r_model.c: занятых мест в mod_known (так же)               */
int          BZ_TexCount (void);
static int   canary_frames, canary_bad[4];
static int   peak[4];           /* картинки, модели, модели-кисти карты, записи кэша текстур */
static FILE *canary_log;

static int canary_dirty (const byte *p, size_t n, byte v)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (p[i] != v)
			return 1;
	return 0;
}

/*
 * «Диета», шаг 2: ещё r_warpbuffer (буфер подводной волны, 77 КБ; в начале нулевой — должен таким и
 * остаться) и пики таблиц рендера и переднего края — по ним выбираются их размеры на ESP. Пик меняется —
 * строка «peaks: …» в том же файле; прогон по всем картам (mapstats) даёт наибольшие значения.
 */
static void canary_check (void)
{
	static const char *what[4] = { "frame buffer", "z-buffer", "surface cache", "warp buffer" };
	int i, dirty[4], now[4], grew = 0;

	dirty[0] = vid.buffer && canary_dirty (vid.buffer, (size_t)vid.width * vid.height, 0xA5);
	dirty[1] = d_pzbuffer && canary_dirty ((const byte *)d_pzbuffer, (size_t)vid.width * vid.height * 2, 0xA5);
	dirty[2] = sc_base && (sc_rover != sc_base || sc_base->next ||
	                       canary_dirty ((const byte *)(sc_base + 1), sc_size - sizeof(*sc_base), 0xA5));
	dirty[3] = canary_dirty (r_warpbuffer, sizeof(r_warpbuffer), 0);
	now[0] = numr_images;
	now[1] = mod_numknown;
	now[2] = r_worldmodel ? r_worldmodel->numsubmodels : 0;
	now[3] = BZ_TexCount ();
	if (!canary_log && q2m_out_cv->string[0])
	{
		char name[MAX_OSPATH + 16];
		Com_sprintf (name, sizeof(name), "%s.canary.txt", q2m_out_cv->string);
		canary_log = fopen (name, "w");
	}
	canary_frames++;
	for (i = 0; i < 4; i++)
	{
		if (dirty[i] && !canary_bad[i]++ && canary_log)
			fprintf (canary_log, "%s written, frame %d\n", what[i], (int)q2m_frame_cv->value);
		if (now[i] > peak[i])
		{
			peak[i] = now[i];
			grew = 1;
		}
	}
	if (grew && canary_log)
		fprintf (canary_log, "peaks: images %d, models %d, inline models %d, texture cache %d (frame %d)\n",
		         peak[0], peak[1], peak[2], peak[3], (int)q2m_frame_cv->value);
	/* итог — и по ходу: замерочный сценарий кончается exit(0), до SWimp_Shutdown дело не доходит */
	if (canary_log && canary_frames % 500 == 0)
	{
		fprintf (canary_log, "%d frames checked, written: frame buffer %d, z-buffer %d, surface cache %d, "
		         "warp buffer %d\n", canary_frames, canary_bad[0], canary_bad[1], canary_bad[2], canary_bad[3]);
		fflush (canary_log);
	}
}
#endif

void SWimp_EndFrame (void)
{
	unsigned crc = 0xFFFFFFFFu;
	int      y;

#ifdef BZ_ONLY
	canary_check ();
#endif
	BZ_EndFrame (q2m_palette);	// кадр модели адаптера против кадра id
	if (!q2m_crc || !vid.buffer)
		return;
	for (y = 0; y < vid.height; y++)
		crc = Q2M_Crc (crc, vid.buffer + y * vid.rowbytes, vid.width);
	crc = Q2M_Crc (crc, q2m_palette, sizeof(q2m_palette));
	fprintf (q2m_crc, "%d %08x\n", (int)q2m_frame_cv->value, ~crc);
}

void SWimp_Shutdown (void)
{
#ifdef BZ_ONLY
	if (canary_log)
	{
		fprintf (canary_log, "%d frames checked, written: frame buffer %d, z-buffer %d, surface cache %d, "
		         "warp buffer %d\n", canary_frames, canary_bad[0], canary_bad[1], canary_bad[2], canary_bad[3]);
		fclose (canary_log);
		canary_log = NULL;
	}
#endif
	BZ_Shutdown ();
	free (q2m_fb);
	q2m_fb = NULL;
	vid.buffer = NULL;
	if (q2m_crc)
		fclose (q2m_crc);
	q2m_crc = NULL;
}

void SWimp_AppActivate (qboolean active)
{
}
