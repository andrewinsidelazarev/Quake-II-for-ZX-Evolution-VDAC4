/*
 * snd_q2m.c — замерочная сборка Quake 2 для VDAC4: звуковой «DMA» без звуковой карты.
 *
 * Заменяет win32/snd_win.c. Буфер DMA — просто память, а позиция «проигрывания»
 * идёт по виртуальным часам игры (q2m_sim_ms). Поэтому snd_dma.c смешивает ровно
 * столько звука, сколько нужно на 1/30 с кадра, — как на настоящей машине, — и время
 * смешивания попадает в замер Q2M_SND.
 *
 * Частота 22 050 Гц, 16 бит, стерео: как у ESP с ЦАП PCM5102 в разумной настройке.
 */

#include "../client/client.h"
#include "../client/snd_loc.h"

extern int q2m_sim_ms;

static byte *q2m_dmabuf;
static int   q2m_start_ms;

qboolean SNDDMA_Init (void)
{
	memset ((void *)&dma, 0, sizeof(dma));
	dma.channels = 2;
	dma.samplebits = 16;
	dma.speed = 22050;
	dma.samples = 32768;			/* моно-отсчётов в буфере: ~0,74 с стерео */
	dma.submission_chunk = 1;
	q2m_dmabuf = malloc (dma.samples * (dma.samplebits / 8));
	memset (q2m_dmabuf, 0, dma.samples * (dma.samplebits / 8));
	dma.buffer = q2m_dmabuf;
	q2m_start_ms = q2m_sim_ms;
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	long long frames = (long long)(q2m_sim_ms - q2m_start_ms) * dma.speed / 1000;

	dma.samplepos = (int)((frames * dma.channels) % dma.samples);
	return dma.samplepos;
}

void SNDDMA_Shutdown (void)
{
	free (q2m_dmabuf);
	q2m_dmabuf = NULL;
	dma.buffer = NULL;
}

void SNDDMA_BeginPainting (void)
{
}

void SNDDMA_Submit (void)
{
}

/* snd_win.c ещё даёт S_Activate для окна; у нас окна нет */
void S_Activate (qboolean active)
{
}
