/*
 * swimp_q2m.c — замерочная сборка Quake 2 для VDAC4: программный рендер без окна.
 *
 * Заменяет win32/rw_imp.c, rw_dib.c, rw_ddraw.c. Кадр рисуется в обычную память
 * (8 бит на точку, как у id), на экран не выводится. Так меряется чистая цена
 * программного рендера (вариант «всё на ESP»), без вывода в окно Windows.
 */

#include "../ref_soft/r_local.h"

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
	return true;
}

rserr_t SWimp_SetMode (int *pwidth, int *pheight, int mode, qboolean fullscreen)
{
	if (!ri.Vid_GetModeInfo (pwidth, pheight, mode))
		return rserr_invalid_mode;
	free (q2m_fb);
	q2m_fb = malloc ((*pwidth) * (*pheight));
	memset (q2m_fb, 0, (*pwidth) * (*pheight));
	vid.buffer = q2m_fb;
	vid.rowbytes = *pwidth;
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

void SWimp_EndFrame (void)
{
	unsigned crc = 0xFFFFFFFFu;
	int      y;

	if (!q2m_crc || !vid.buffer)
		return;
	for (y = 0; y < vid.height; y++)
		crc = Q2M_Crc (crc, vid.buffer + y * vid.rowbytes, vid.width);
	crc = Q2M_Crc (crc, q2m_palette, sizeof(q2m_palette));
	fprintf (q2m_crc, "%d %08x\n", (int)q2m_frame_cv->value, ~crc);
}

void SWimp_Shutdown (void)
{
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
