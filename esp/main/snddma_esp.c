/*
 * =====================================================================================================
 * snddma_esp.c — звуковой «DMA» Quake 2 на ESP32-S3 (у id — win32/snd_win.c; на ПК — measure/snd_q2m.c).
 * =====================================================================================================
 *
 * КАК У ID
 *   snd_dma.c смешивает звуки в кольцо dma.buffer и спрашивает у драйвера, докуда звуковая карта его
 *   уже сыграла (SNDDMA_GetDMAPos): новое смешивается от этого места на s_mixahead секунды вперёд.
 *
 * ЗДЕСЬ (25.11.2026)
 *   Звуковой карты нет — есть вывод звука прошивки ESP: pcm_open / pcm_write (кадры float −1..1, каналы
 *   чередуются; прошивка сама пересчитывает частоту в 44 100 Гц и пишет в ЦАП). pcm_write ждёт места в
 *   очереди вывода, поэтому зовёт её своя задача (snd_task), а не игра: она берёт из кольца смешанное и
 *   отдаёт прошивке кусками по SND_CHUNK кадров. Позиция «DMA» — сколько кадров задача уже отдала.
 *   Кольцо — 11 025 Гц, 16 бит, стерео (звуки лежат 8 бит, 11 кГц — «диета» PLAN.md; смешивание id — в
 *   стерео при любом выводе).
 *
 * ЕСЛИ ИГРА НЕ УСПЕВАЕТ СМЕШИВАТЬ
 *   Задача никогда не уходит дальше смешанного (snd_painted — paintedtime на SNDDMA_Submit): нечего
 *   играть — отдаёт тишину, а позиция стоит. Иначе после долгого кадра (загрузка уровня — десятки секунд)
 *   позиция ушла бы на несколько кругов кольца, а snd_dma.c считает круги по одному «переходу через ноль»
 *   за вызов — и смешивал бы потом позади того, что уже сыграно (тишина навсегда).
 *
 * Звуки уровня (1,6–2,5 МБ) лежат в SDRAM ядра «бомж-3D» — snd_mem.c / snd_mix.c, Q2_SND_STORE.
 */
#include "q2esp.h"
#include "../../port/client/client.h"
#include "../../port/client/snd_loc.h"

#include <stddef.h>
#include <string.h>

#define SND_RATE   11025
#define SND_RING   8192u        /* моно-отсчётов в кольце: 4096 кадров стерео, 0,37 с (s_mixahead 0,3), 16 КБ */
#define SND_CHUNK  256          /* кадров за один pcm_write — 23 мс при 11 025 Гц                     */
/* Очередь вывода прошивки — 256 КБ (audio_player.cpp): при 11 025 Гц это ~6 с, и pcm_write ждёт, лишь
 * когда она полна. Задача же пишет и тишину, когда смешанного нет: без меры очередь копила бы секунды
 * тишины, и звук игры шёл бы за ними на секунды позже. Поэтому — писать, только пока несыгранного не
 * больше snd_queue_ms. pcm_queued_ms — это очередь плюс ВЕСЬ буфер DMA после каждой записи в I2S
 * (dmaTailMs_ в audio_player.cpp): у источника ниже 16 кГц вывод идёт на удвоенной частоте (22 050 Гц), и
 * обычный DMA (8 × 480 кадров) — 174 мс. Плата 25.11: порог был 120 мс — меньше одного DMA, задача почти не
 * писала (17 записей в секунду вместо 43), ЦАП оставался без звука (udf 25 за 2 с), звук «тормозил и
 * прерывался». Теперь — DMA малой задержки (pcm_low_latency, 1440 кадров, 65 мс) и порог «DMA + SND_LEAD_MS». */
#define SND_LEAD_MS 80          /* своего запаса в очереди прошивки сверх буфера DMA                   */
#define SND_DAC_MIN 16000       /* ниже — прошивка выводит на удвоенной частоте (kMinDacRate)           */
#define SND_STACK  3072

static short         *snd_ring;
static volatile int   snd_run;            /* 1 — задача работает, 0 — просьба выйти                   */
static volatile int   snd_alive;          /* задача ещё не вышла                                       */
static volatile int   snd_painted;        /* до какого кадра смешано (paintedtime)                     */
static volatile int   snd_played;         /* сколько кадров отдано прошивке — позиция «DMA»            */
static volatile uint32_t snd_silence;     /* кадров тишины, отданных вместо несмешанного (недобор)     */
static volatile int   snd_error;          /* pcm_write вернула ошибку — задача вышла                   */
static float          snd_out[SND_CHUNK * 2];
static uint32_t       snd_queue_ms;       /* писать, пока pcm_queued_ms не больше этого (SNDDMA_Init)     */

/* Прошивка с выводом звука и задачами (они в таблице с ранних ABI, но проверим по size, как всё). */
static int snd_ready (void)
{
	return q2_core->size >= offsetof (zm_core_t, pcm_faults) + sizeof(q2_core->pcm_faults);
}

static void snd_task (void *arg)
{
	(void)arg;
	while (snd_run)
	{
		int base, avail, n, i, done;

		if (q2_core->pcm_queued_ms () > snd_queue_ms)
		{
			q2_core->delay (5);
			continue;
		}
		base = snd_played;
		avail = snd_painted - base;

		n = avail <= 0 ? 0 : avail < SND_CHUNK ? avail : SND_CHUNK;
		for (i = 0; i < n; i++)
		{
			unsigned k = ((unsigned)(base + i) * 2u) & (SND_RING - 1u);
			snd_out[2 * i]     = snd_ring[k] * (1.0f / 32768.0f);
			snd_out[2 * i + 1] = snd_ring[k + 1] * (1.0f / 32768.0f);
		}
		for ( ; i < SND_CHUNK; i++)
			snd_out[2 * i] = snd_out[2 * i + 1] = 0.0f;
		for (done = 0; done < SND_CHUNK && snd_run; )
		{
			int r = q2_core->pcm_write (snd_out + 2 * done, (uint32_t)(SND_CHUNK - done));
			if (r < 0)
			{
				snd_error = r;
				snd_run = 0;
				break;
			}
			if (r == 0)
				q2_core->delay (2);
			done += r;
		}
		snd_played = base + n;            /* тишина позицию не двигает (см. шапку) */
		if (n < SND_CHUNK)
			snd_silence += (uint32_t)(SND_CHUNK - n);
	}
	snd_alive = 0;
}

qboolean SNDDMA_Init (void)
{
	zm_pcm_format_t f;
	int             r;

	if (!snd_ready ())
	{
		q2_core->log ("q2: sound: ESP firmware without PCM output (ABI 1.%u)", q2_core->abi_minor);
		return false;
	}
	f.rate = SND_RATE;
	f.channels = 2;
	{
		/* глубина DMA следующего pcm_open — в кадрах вывода; без поля (старая прошивка) — обычный DMA */
		uint32_t depth = 8u * 480u;
		uint32_t out = SND_RATE < SND_DAC_MIN ? 2u * SND_RATE : SND_RATE;

		if (q2_core->size >= offsetof (zm_core_t, pcm_low_latency) + sizeof(q2_core->pcm_low_latency))
			depth = q2_core->pcm_low_latency (1);
		snd_queue_ms = depth * 1000u / out + SND_LEAD_MS;
	}
	r = q2_core->pcm_open (&f);
	if (r != ZM_OK)
	{
		q2_core->log ("q2: sound: pcm_open %d", r);
		return false;
	}
	if (!snd_ring)
		snd_ring = malloc (SND_RING * sizeof(short));
	if (!snd_ring)
	{
		q2_core->pcm_close ();
		q2_core->log ("q2: sound: no memory for the ring");
		return false;
	}
	memset (snd_ring, 0, SND_RING * sizeof(short));
	memset ((void *)&dma, 0, sizeof(dma));
	dma.channels = 2;
	dma.samplebits = 16;
	dma.speed = SND_RATE;
	dma.samples = SND_RING;
	dma.submission_chunk = 1;
	dma.buffer = (byte *)snd_ring;
	snd_painted = snd_played = paintedtime;
	snd_silence = 0;
	snd_error = 0;
	snd_run = snd_alive = 1;
	/* приоритет выше игры: иначе тяжёлый кадр оставит ЦАП без звука; ядро — любое */
	r = q2_core->task_start (snd_task, NULL, SND_STACK, 5, -1);
	if (r != ZM_OK)
		r = q2_core->task_start (snd_task, NULL, SND_STACK | ZM_TASK_STACK_PSRAM, 5, -1);
	if (r != ZM_OK)
	{
		snd_run = snd_alive = 0;
		q2_core->pcm_close ();
		q2_core->log ("q2: sound: task_start %d", r);
		return false;
	}
	q2_core->log ("q2: sound: %d Hz stereo, ring %u KB, chunk %d frames, queue up to %u ms", SND_RATE,
	              (unsigned)(SND_RING * sizeof(short) >> 10), SND_CHUNK, (unsigned)snd_queue_ms);
	return true;
}

int SNDDMA_GetDMAPos (void)
{
	dma.samplepos = (int)(((unsigned)snd_played * 2u) & (SND_RING - 1u));
	return dma.samplepos;
}

void SNDDMA_Shutdown (void)
{
	uint32_t t0 = q2_core->millis ();

	snd_run = 0;
	while (snd_alive && q2_core->millis () - t0 < 500)
		q2_core->delay (5);
	q2_core->pcm_close ();
	q2_core->log ("q2: sound: played %d frames, silence %u frames (mixing late), I2S faults %u%s",
	              snd_played, (unsigned)snd_silence, (unsigned)q2_core->pcm_faults (),
	              snd_error ? ", pcm_write failed" : "");
	dma.buffer = NULL;
}

void SNDDMA_BeginPainting (void)
{
}

/* Раз в 5 с (q2_app.c, report_times): сколько кадров звука ушло, сколько из них — тишина вместо
 * несмешанного, смешано наперёд, в очереди прошивки и самый громкий отсчёт кольца (0 — звучит тишина). */
void snd_esp_report (void)
{
	static int      last_played;
	static uint32_t last_silence;
	int             played = snd_played, peak = 0;
	uint32_t        silence = snd_silence, i;

	if (!dma.buffer || !snd_ring)
		return;
	for (i = 0; i < SND_RING; i++)
	{
		int v = snd_ring[i] < 0 ? -snd_ring[i] : snd_ring[i];
		if (v > peak)
			peak = v;
	}
	q2_core->log ("q2: sound: %d frames out, %u silence, painted ahead %d, queued %u ms, ring peak %d",
	              played - last_played, (unsigned)(silence - last_silence), snd_painted - played,
	              (unsigned)q2_core->pcm_queued_ms (), peak);
	last_played = played;
	last_silence = silence;
}

/* Смешанное до paintedtime — задаче (порядок записи: сначала кольцо, потом граница). */
void SNDDMA_Submit (void)
{
	snd_painted = paintedtime;
}
