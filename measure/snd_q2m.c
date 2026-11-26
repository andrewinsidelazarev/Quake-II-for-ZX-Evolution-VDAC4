/*
 * snd_q2m.c — замерочная сборка Quake 2 для VDAC4: звуковой «DMA» без звуковой карты.
 *
 * Заменяет win32/snd_win.c. Буфер DMA — просто память, а позиция «проигрывания»
 * идёт по виртуальным часам игры (q2m_sim_ms). Поэтому snd_dma.c смешивает ровно
 * столько звука, сколько нужно на 1/30 с кадра, — как на настоящей машине, — и время
 * смешивания попадает в замер Q2M_SND.
 *
 * Частота 22 050 Гц, 16 бит, стерео: как у ESP с ЦАП PCM5102 в разумной настройке.
 * Q2M_SND_RATE=11025 — как на ESP (esp/main/snddma_esp.c, 25.11.2026).
 *
 * Сверка (25.11.2026, звук в хранилище SDRAM ядра): CRC-32 всего, что смешано (SNDDMA_Submit), и число
 * кадров — в stdout при выходе: «snd: mixed N frames, crc X». s_store 1 и s_store 0 обязаны дать одно.
 */

#include "../client/client.h"
#include "../client/snd_loc.h"

extern int q2m_sim_ms;

static byte *q2m_dmabuf;
static int   q2m_start_ms;
static int   q2m_crc_done;            /* до какого paintedtime CRC уже посчитана */
static unsigned q2m_crc = 0xFFFFFFFFu;
static long long q2m_crc_frames;
static int   q2m_peak;                /* самый громкий смешанный отсчёт (0 — звучала тишина) */
static void q2m_snd_report (void);

qboolean SNDDMA_Init (void)
{
	memset ((void *)&dma, 0, sizeof(dma));
	dma.channels = 2;
	dma.samplebits = 16;
	dma.speed = getenv ("Q2M_SND_RATE") ? atoi (getenv ("Q2M_SND_RATE")) : 22050;
	dma.samples = 32768;			/* моно-отсчётов в буфере: ~0,74 с стерео */
	dma.submission_chunk = 1;
	q2m_dmabuf = malloc (dma.samples * (dma.samplebits / 8));
	memset (q2m_dmabuf, 0, dma.samples * (dma.samplebits / 8));
	dma.buffer = q2m_dmabuf;
	q2m_start_ms = q2m_sim_ms;
	q2m_crc_done = paintedtime;
	{
		static int reg;
		if (!reg)
			atexit (q2m_snd_report);
		reg = 1;
	}
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	long long frames = (long long)(q2m_sim_ms - q2m_start_ms) * dma.speed / 1000;

	dma.samplepos = (int)((frames * dma.channels) % dma.samples);
	return dma.samplepos;
}

/* итог сверки: прогон q2m выходит через exit без S_Shutdown — поэтому atexit */
static void q2m_snd_report (void)
{
	printf ("snd: mixed %lld frames at %d Hz, crc %08x, peak %d\n", q2m_crc_frames, dma.speed, ~q2m_crc,
	        q2m_peak);
#ifdef Q2_SND_STORE
	{
		extern int s_store_count, s_store_bytes, s_store_streamed;   /* snd_mem.c */
		printf ("snd: %d sounds (%d streamed), %d KB put to the core SDRAM store\n", s_store_count,
		        s_store_streamed, s_store_bytes >> 10);
	}
#endif
	fflush (stdout);
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
	int bytes = dma.channels * (dma.samplebits / 8);

	if (paintedtime - q2m_crc_done > dma.samples / dma.channels)   /* не бывает: смешано больше буфера */
		q2m_crc_done = paintedtime - dma.samples / dma.channels;
	for ( ; q2m_crc_done < paintedtime; q2m_crc_done++)
	{
		const byte *p = q2m_dmabuf + ((q2m_crc_done * dma.channels) & (dma.samples - 1)) * (dma.samplebits / 8);
		int i, k;

		if (dma.samplebits == 16)
			for (i = 0; i < bytes; i += 2)
			{
				int v = (short)(p[i] | (p[i + 1] << 8));
				if (v < 0)
					v = -v;
				if (v > q2m_peak)
					q2m_peak = v;
			}
		for (i = 0; i < bytes; i++)
		{
			q2m_crc ^= p[i];
			for (k = 0; k < 8; k++)
				q2m_crc = (q2m_crc >> 1) ^ (0xEDB88320u & (0u - (q2m_crc & 1u)));
		}
		q2m_crc_frames++;
	}
}

/* snd_win.c ещё даёт S_Activate для окна; у нас окна нет */
void S_Activate (qboolean active)
{
}
