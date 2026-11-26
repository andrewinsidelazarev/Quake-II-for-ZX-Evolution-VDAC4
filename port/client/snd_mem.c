/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_mem.c: sound caching

#include "client.h"
#include "snd_loc.h"

int			cache_full_cycle;

byte *S_Alloc (int size);

/*
================
ResampleSfx
================
*/
void ResampleSfx (sfx_t *sfx, int inrate, int inwidth, byte *data)
{
	int		outcount;
	int		srcsample;
	float	stepscale;
	int		i;
	int		sample, samplefrac, fracstep;
	sfxcache_t	*sc;
	
	sc = sfx->cache;
	if (!sc)
		return;

	stepscale = (float)inrate / dma.speed;	// this is usually 0.5, 1, or 2

	outcount = sc->length / stepscale;
	sc->length = outcount;
	if (sc->loopstart != -1)
		sc->loopstart = sc->loopstart / stepscale;

	sc->speed = dma.speed;
	if (s_loadas8bit->value)
		sc->width = 1;
	else
		sc->width = inwidth;
	sc->stereo = 0;

// resample / decimate to the current source rate

	if (stepscale == 1 && inwidth == 1 && sc->width == 1)
	{
// fast special case
		for (i=0 ; i<outcount ; i++)
			((signed char *)sc->data)[i]
			= (int)( (unsigned char)(data[i]) - 128);
	}
	else
	{
// general case
		samplefrac = 0;
		fracstep = stepscale*256;
		for (i=0 ; i<outcount ; i++)
		{
			srcsample = samplefrac >> 8;
			samplefrac += fracstep;
			if (inwidth == 2)
				sample = LittleShort ( ((short *)data)[srcsample] );
			else
				sample = (int)( (unsigned char)(data[srcsample]) - 128) << 8;
			if (sc->width == 2)
				((short *)sc->data)[i] = sample;
			else
				((signed char *)sc->data)[i] = sample >> 8;
		}
	}
}

//=============================================================================

#ifdef Q2_SND_STORE
/*
==============
S_StoreSfx

порт VDAC4 (25.11.2026): отсчёты загруженного звука — в хранилище SDRAM ядра, в зоне остаётся заголовок.
PSRAM ESP на уровень звуков (1,6–2,5 МБ при 11 кГц / 8 бит) не хватает; смешивание (snd_mix.c,
S_SfxData) читает по LINK только кусок, который рисует. Места в SDRAM нет — звук остаётся в зоне, как у id.
s_store 0 — всё в зоне (сверка на ПК: смешанный звук обязан совпасть бит в бит).
==============
*/
int		s_store_count, s_store_bytes;	// сколько звуков и байт ушло в хранилище (отчёты)
int		s_store_streamed;		// из них потоком (S_LoadSoundStream), без wav в памяти

static sfxcache_t *S_StoreSfx (sfx_t *s)
{
	static cvar_t	*s_store;
	sfxcache_t	*sc = s->cache, *h;
	uint32_t	bytes, addr;

	if (!s_store)
		s_store = Cvar_Get ("s_store", "1", 0);
	sc->store = 0;
	bytes = (uint32_t)(sc->length * sc->width);
	if (!s_store->value || !bytes)
		return sc;
	addr = BZ_StoreAlloc (bytes);
	if (!addr)
		return sc;
	BZ_StoreWrite (addr, sc->data, bytes);
	BZ_StoreMark ();			// чтение ждёт этот MARK: отсчёты уже в SDRAM
	s_store_count++;
	s_store_bytes += (int)bytes;
	h = Z_Malloc (sizeof(sfxcache_t));
	memcpy (h, sc, sizeof(sfxcache_t));
	h->store = addr;
	h->store_gen = BZ_StoreGeneration ();
	Z_Free (sc);
	s->cache = h;
	return h;
}
#endif

#ifdef Q2_SND_STORE
/*
==============
S_LoadSoundStream

порт VDAC4 (25.11.2026): звук — прямо из pak в хранилище SDRAM ядра, без файла wav в памяти целиком.
У id S_LoadSound читает весь wav (FS_LoadFile) и пересчитывает его в кэш — два больших куска сразу; в PSRAM
ESP на загрузке уровня их нет (Unreal NG с памятью как на плате: не нашлось 141 КБ одним куском при
свободных 256). Здесь заголовок разбирается чтением по месту — так же, как GetWavinfo (те же чанки, в том
же порядке), — а отсчёты идут блоками по SND_STREAM_OUT выходных: каждый пересчитан, как ResampleSfx
(тот же шаг samplefrac, то же округление), и сразу записан в хранилище. NULL — этот путь не годится (нет
хранилища, места или wav необычный): тогда S_LoadSound идёт путём id. Сверка на ПК: смешанный звук с s_store 1
и s_store 0 (путь id) совпадает бит в бит.
==============
*/
#define SND_STREAM_OUT	1024				// выходных отсчётов за шаг
#define SND_STREAM_IN	(SND_STREAM_OUT * 8 * 2 + 8)	// вход шага: частота до 8× выходной, 16 бит

static byte	snd_stream_in[SND_STREAM_IN];
static short	snd_stream_out[SND_STREAM_OUT];	// 16 бит или 8 (тогда как байты)

static int S_StreamLong (const byte *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// n байт файла с позиции off; что за концом файла — нули (у id там была бы чужая память)
static void S_StreamRead (FILE *f, int base, int filelen, int off, void *buf, int n)
{
	int	have = filelen - off;

	if (have > n)
		have = n;
	if (have > 0)
	{
		fseek (f, base + off, SEEK_SET);
		FS_Read (buf, have, f);
	}
	else
		have = 0;
	if (have < n)
		memset ((byte *)buf + have, 0, n - have);
}

static sfxcache_t *S_LoadSoundStream (sfx_t *s, char *namebuffer)
{
	static cvar_t	*s_store;
	FILE		*f;
	byte		h[16];
	int		filelen, base, off, len;
	int		fmt_off = -1, cue_off = -1, data_off = -1, data_len = 0, list_seen = 0, mark_len = 0;
	int		format, channels, rate, inwidth, loopstart, samples, outcount, width, fracstep, maxin;
	int		o0, o1, i, s0, s1;
	float		stepscale;
	uint32_t	bytes, addr;
	sfxcache_t	*sc;

	if (!s_store)
		s_store = Cvar_Get ("s_store", "1", 0);
	if (!s_store->value)
		return NULL;
	filelen = FS_FOpenFile (namebuffer, &f);
	if (!f)
		return NULL;
	base = ftell (f);
	if (filelen < 12)
		goto fail;
	S_StreamRead (f, base, filelen, 0, h, 12);
	if (strncmp ((char *)h, "RIFF", 4) || strncmp ((char *)h + 8, "WAVE", 4))
		goto fail;			// у id RIFF ищется и не в начале — такой wav пусть грузит id

	// чанки — как FindChunk / FindNextChunk: первый fmt, первый cue, первый LIST после него, первый data
	for (off = 12; off + 8 <= filelen; off += 8 + ((len + 1) & ~1))
	{
		S_StreamRead (f, base, filelen, off, h, 8);
		len = S_StreamLong (h + 4);
		if (len < 0)
			break;
		if (!strncmp ((char *)h, "fmt ", 4) && fmt_off < 0)
			fmt_off = off;
		else if (!strncmp ((char *)h, "cue ", 4) && cue_off < 0)
			cue_off = off;
		else if (!strncmp ((char *)h, "LIST", 4) && cue_off >= 0 && !list_seen)
		{
			list_seen = 1;
			S_StreamRead (f, base, filelen, off + 24, h, 8);
			if (!strncmp ((char *)h + 4, "mark", 4))
				mark_len = S_StreamLong (h);	// отсчётов в петле
		}
		else if (!strncmp ((char *)h, "data", 4) && data_off < 0)
		{
			data_off = off;
			data_len = len;
		}
	}
	if (fmt_off < 0 || data_off < 0)
		goto fail;
	S_StreamRead (f, base, filelen, fmt_off + 8, h, 16);
	format = h[0] | (h[1] << 8);
	channels = h[2] | (h[3] << 8);
	rate = S_StreamLong (h + 4);
	inwidth = (h[14] | (h[15] << 8)) / 8;
	if (format != 1 || channels != 1 || (inwidth != 1 && inwidth != 2) || rate <= 0)
		goto fail;			// сообщения об ошибке — у пути id
	loopstart = -1;
	if (cue_off >= 0)
	{
		S_StreamRead (f, base, filelen, cue_off + 32, h, 4);
		loopstart = S_StreamLong (h);
	}
	samples = data_len / inwidth;
	if (cue_off >= 0 && list_seen && mark_len)
	{
		if (samples < loopstart + mark_len)
			goto fail;		// id: «bad loop length»
		samples = loopstart + mark_len;
	}

	// как ResampleSfx
	stepscale = (float)rate / dma.speed;
	outcount = samples / stepscale;
	if (loopstart != -1)
		loopstart = loopstart / stepscale;
	width = s_loadas8bit->value ? 1 : inwidth;
	fracstep = stepscale * 256;
	maxin = (((SND_STREAM_OUT - 1) * fracstep) >> 8) + 1;
	bytes = (uint32_t)(outcount * width);
	if (!bytes || maxin * inwidth > SND_STREAM_IN)
		goto fail;
	addr = BZ_StoreAlloc (bytes);
	if (!addr)
		goto fail;			// SDRAM полна — путь id (звук в зоне)

	for (o0 = 0; o0 < outcount; o0 = o1)
	{
		o1 = o0 + SND_STREAM_OUT < outcount ? o0 + SND_STREAM_OUT : outcount;
		s0 = (o0 * fracstep) >> 8;	// у id samplefrac = i * fracstep: тот же отсчёт
		s1 = ((o1 - 1) * fracstep) >> 8;
		S_StreamRead (f, base, filelen, data_off + 8 + s0 * inwidth, snd_stream_in, (s1 - s0 + 1) * inwidth);
		for (i = o0; i < o1; i++)
		{
			int	src = ((i * fracstep) >> 8) - s0, sample;

			if (inwidth == 2)
				sample = (short)(snd_stream_in[2 * src] | (snd_stream_in[2 * src + 1] << 8));
			else
				sample = (int)(snd_stream_in[src] - 128) << 8;
			if (width == 2)
				snd_stream_out[i - o0] = (short)sample;
			else
				((signed char *)snd_stream_out)[i - o0] = sample >> 8;
		}
		BZ_StoreWrite (addr + (uint32_t)(o0 * width), snd_stream_out, (uint32_t)((o1 - o0) * width));
	}
	BZ_StoreMark ();			// чтение ждёт этот MARK: отсчёты уже в SDRAM
	FS_FCloseFile (f);

	sc = Z_Malloc (sizeof(sfxcache_t));
	sc->length = outcount;
	sc->loopstart = loopstart;
	sc->speed = dma.speed;
	sc->width = width;
	sc->stereo = 0;
	sc->store = addr;
	sc->store_gen = BZ_StoreGeneration ();
	s->cache = sc;
	s_store_count++;
	s_store_streamed++;
	s_store_bytes += (int)bytes;
	return sc;

fail:
	FS_FCloseFile (f);
	return NULL;
}
#endif

/*
==============
S_LoadSound
==============
*/
sfxcache_t *S_LoadSound (sfx_t *s)
{
    char	namebuffer[MAX_QPATH];
	byte	*data;
	wavinfo_t	info;
	int		len;
	float	stepscale;
	sfxcache_t	*sc;
	int		size;
	char	*name;

	if (s->name[0] == '*')
		return NULL;

// see if still in memory
	sc = s->cache;
#ifdef Q2_SND_STORE
	// порт VDAC4: хранилище SDRAM стёрто сменой режима экрана — отсчёты перечитать из pak
	if (sc && sc->store && sc->store_gen != BZ_StoreGeneration ())
	{
		Z_Free (sc);
		s->cache = sc = NULL;
	}
#endif
	if (sc)
		return sc;

//Com_Printf ("S_LoadSound: %x\n", (int)stackbuf);
// load it in
	if (s->truename)
		name = s->truename;
	else
		name = s->name;

	if (name[0] == '#')
		strcpy(namebuffer, &name[1]);
	else
		Com_sprintf (namebuffer, sizeof(namebuffer), "sound/%s", name);

//	Com_Printf ("loading %s\n",namebuffer);

#ifdef Q2_SND_STORE
	sc = S_LoadSoundStream (s, namebuffer);	// порт VDAC4: потоком в SDRAM; NULL — путь id
	if (sc)
		return sc;
#endif

	size = FS_LoadFile (namebuffer, (void **)&data);

	if (!data)
	{
		Com_DPrintf ("Couldn't load %s\n", namebuffer);
		return NULL;
	}

	info = GetWavinfo (s->name, data, size);
	if (info.channels != 1)
	{
		Com_Printf ("%s is a stereo sample\n",s->name);
		FS_FreeFile (data);
		return NULL;
	}

	stepscale = (float)info.rate / dma.speed;	
	len = info.samples / stepscale;

	len = len * info.width * info.channels;

	sc = s->cache = Z_Malloc (len + sizeof(sfxcache_t));
	if (!sc)
	{
		FS_FreeFile (data);
		return NULL;
	}
	
	sc->length = info.samples;
	sc->loopstart = info.loopstart;
	sc->speed = info.rate;
	sc->width = info.width;
	sc->stereo = info.channels;

	ResampleSfx (s, sc->speed, sc->width, data + info.dataofs);

	FS_FreeFile (data);

#ifdef Q2_SND_STORE
	sc = S_StoreSfx (s);
#endif
	return sc;
}

/*
==============
S_FreeSfxCache

порт VDAC4: отпустить кэш звука — и его отсчёты в хранилище SDRAM, если они там
==============
*/
void S_FreeSfxCache (sfx_t *sfx)
{
	sfxcache_t	*sc = sfx->cache;

	if (!sc)
		return;
#ifdef Q2_SND_STORE
	if (sc->store && sc->store_gen == BZ_StoreGeneration ())
		BZ_StoreFree (sc->store, (uint32_t)(sc->length * sc->width));
#endif
	Z_Free (sc);
	sfx->cache = NULL;
}



/*
===============================================================================

WAV loading

===============================================================================
*/


byte	*data_p;
byte 	*iff_end;
byte 	*last_chunk;
byte 	*iff_data;
int 	iff_chunk_len;


short GetLittleShort(void)
{
	short val = 0;
	val = *data_p;
	val = val + (*(data_p+1)<<8);
	data_p += 2;
	return val;
}

int GetLittleLong(void)
{
	int val = 0;
	val = *data_p;
	val = val + (*(data_p+1)<<8);
	val = val + (*(data_p+2)<<16);
	val = val + (*(data_p+3)<<24);
	data_p += 4;
	return val;
}

void FindNextChunk(char *name)
{
	while (1)
	{
		data_p=last_chunk;

		if (data_p >= iff_end)
		{	// didn't find the chunk
			data_p = NULL;
			return;
		}
		
		data_p += 4;
		iff_chunk_len = GetLittleLong();
		if (iff_chunk_len < 0)
		{
			data_p = NULL;
			return;
		}
//		if (iff_chunk_len > 1024*1024)
//			Sys_Error ("FindNextChunk: %i length is past the 1 meg sanity limit", iff_chunk_len);
		data_p -= 8;
		last_chunk = data_p + 8 + ( (iff_chunk_len + 1) & ~1 );
		if (!strncmp(data_p, name, 4))
			return;
	}
}

void FindChunk(char *name)
{
	last_chunk = iff_data;
	FindNextChunk (name);
}


void DumpChunks(void)
{
	char	str[5];
	
	str[4] = 0;
	data_p=iff_data;
	do
	{
		memcpy (str, data_p, 4);
		data_p += 4;
		iff_chunk_len = GetLittleLong();
		Com_Printf ("0x%x : %s (%d)\n", (int)(data_p - 4), str, iff_chunk_len);
		data_p += (iff_chunk_len + 1) & ~1;
	} while (data_p < iff_end);
}

/*
============
GetWavinfo
============
*/
wavinfo_t GetWavinfo (char *name, byte *wav, int wavlength)
{
	wavinfo_t	info;
	int     i;
	int     format;
	int		samples;

	memset (&info, 0, sizeof(info));

	if (!wav)
		return info;
		
	iff_data = wav;
	iff_end = wav + wavlength;

// find "RIFF" chunk
	FindChunk("RIFF");
	if (!(data_p && !strncmp(data_p+8, "WAVE", 4)))
	{
		Com_Printf("Missing RIFF/WAVE chunks\n");
		return info;
	}

// get "fmt " chunk
	iff_data = data_p + 12;
// DumpChunks ();

	FindChunk("fmt ");
	if (!data_p)
	{
		Com_Printf("Missing fmt chunk\n");
		return info;
	}
	data_p += 8;
	format = GetLittleShort();
	if (format != 1)
	{
		Com_Printf("Microsoft PCM format only\n");
		return info;
	}

	info.channels = GetLittleShort();
	info.rate = GetLittleLong();
	data_p += 4+2;
	info.width = GetLittleShort() / 8;

// get cue chunk
	FindChunk("cue ");
	if (data_p)
	{
		data_p += 32;
		info.loopstart = GetLittleLong();
//		Com_Printf("loopstart=%d\n", sfx->loopstart);

	// if the next chunk is a LIST chunk, look for a cue length marker
		FindNextChunk ("LIST");
		if (data_p)
		{
			if (!strncmp (data_p + 28, "mark", 4))
			{	// this is not a proper parse, but it works with cooledit...
				data_p += 24;
				i = GetLittleLong ();	// samples in loop
				info.samples = info.loopstart + i;
//				Com_Printf("looped length: %i\n", i);
			}
		}
	}
	else
		info.loopstart = -1;

// find data chunk
	FindChunk("data");
	if (!data_p)
	{
		Com_Printf("Missing data chunk\n");
		return info;
	}

	data_p += 4;
	samples = GetLittleLong () / info.width;

	if (info.samples)
	{
		if (samples < info.samples)
			Com_Error (ERR_DROP, "Sound %s has a bad loop length", name);
	}
	else
		info.samples = samples;

	info.dataofs = data_p - wav;
	
	return info;
}

