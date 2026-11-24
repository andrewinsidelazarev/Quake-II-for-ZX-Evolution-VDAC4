/*
 * =====================================================================================================
 * swimp_esp.c — то, что у программного рендера id делает окно (у id — win32/rw_imp.c; образец —
 * gpu/front/swimp_bz.c): режим экрана и конец кадра. Собирается в группе рендера (ref_esp).
 * =====================================================================================================
 *
 * Окна нет: картинку рисует ядро Tang. В конце кадра BZ_EndFrame (bz_front.c) дописывает к командам
 * кадра FRAME и отдаёт их в LINK (bz_link.c).
 *
 * Кадра id (vid.buffer, W×H байт) нет — «диета» 23.11: ref_esp в него не пишет (2D только командой,
 * волны и кинематики в кадр id нет — patches/p06_ref_esp.py), и это проверено на ПК: q2esp.exe
 * заполняет кадр, z-буфер и кэш поверхностей образцом и сверяет после каждого кадра всего демо
 * (gpu/front/swimp_bz.c). Если бы что-то всё же записало — обращение по пустому указателю, ESP
 * остановится с ошибкой, а не испортит чужую память молча.
 */
#include "r_local.h"
#include "bz_front.h"

static byte  esp_palette[256 * 4];

int SWimp_Init (void *hInstance, void *wndProc)
{
	(void)hInstance;
	(void)wndProc;
	BZ_Init ();
	return true;
}

rserr_t SWimp_SetMode (int *pwidth, int *pheight, int mode, qboolean fullscreen)
{
	(void)fullscreen;
	if (!ri.Vid_GetModeInfo (pwidth, pheight, mode))
		return rserr_invalid_mode;
	vid.buffer = NULL;              /* кадра id нет: рисует ядро Tang */
	vid.rowbytes = *pwidth;
	BZ_SetMode (*pwidth, *pheight);
	ri.Vid_NewWindow (*pwidth, *pheight);
	R_GammaCorrectAndSetPalette ((const unsigned char *)d_8to24table);
	return rserr_ok;
}

void SWimp_SetPalette (const unsigned char *palette)
{
	if (palette)
		memcpy (esp_palette, palette, sizeof(esp_palette));
}

void SWimp_BeginFrame (float camera_separation)
{
	(void)camera_separation;
}

void SWimp_EndFrame (void)
{
	BZ_EndFrame (esp_palette);      /* команды кадра — в очередь ядра Tang */
}

void SWimp_Shutdown (void)
{
	BZ_Shutdown ();
	vid.buffer = NULL;
}

void SWimp_AppActivate (qboolean active)
{
	(void)active;
}
