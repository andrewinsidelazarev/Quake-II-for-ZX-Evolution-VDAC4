/*
 * =====================================================================================================
 * compat.c — недостающая libc для Q2.ELF (почему её нет у модуля — в compat.h).
 * =====================================================================================================
 *
 * ПАМЯТЬ
 *   malloc и компания — через zm_core_t::alloc в PSRAM: ядро помнит каждый кусок и при выгрузке
 *   приложения освобождает всё само (даже если игра упала посреди уровня).
 *   Памяти нет — сразу Sys_Error с отчётом (q2_mem_report): код id почти нигде не проверяет, что
 *   malloc вернул, и дальше падал бы на нулевом указателе (Unreal NG 24.11: memcpy в 0 на карте base2).
 *
 * ФАЙЛЫ
 *   Quake 2 работает с файлами через stdio: fopen/fread/fseek. Здесь своя маленькая «система файлов»:
 *     - файл, записанный игрой (config.cfg, сохранения, save/current/<уровень>.sav при смене уровня), живёт в
 *       памяти до выгрузки модуля — его можно прочитать обратно (переходы между уровнями без этого не
 *       работают: игра пишет состояние уровня и тут же его читает);
 *     - остальное читается из хранилища pak (q2_store_*, store_esp.c).
 *   Сохранить файлы на карту — отдельная задача (куда и когда писать — решит пользователь).
 *
 * Указатель FILE*, который видит Quake 2, — на самом деле наш q2file_t: id в его поля не смотрит,
 * только передаёт обратно в fread/fclose (а stdin/stdout/stderr Quake 2 здесь не трогает).
 */
#define Q2ESP_COMPAT_IMPL
#include "compat.h"
#include "q2esp.h"

#include <sys/time.h>

/* ---------------------------------------------------------------- память */

void Sys_Error (char *error, ...);

/* Памяти нет: в журнал — сколько просили и отчёт о PSRAM, игра — на выход. */
static void out_of_memory (const char *what, size_t n)
{
	q2_mem_report (what);
	Sys_Error ("%s: no memory for %u bytes", what, (unsigned)n);
}

/* 26.11.2026 (плата board_log22): после моделей прошлой карты — запасные буферы кадра (bz_front.c), по одному. */
int q2_mem_reclaim (void)
{
	return R_EspPurgeStale () || BZ_ReleaseSpare ();
}

void *q2_malloc (size_t n)
{
	void *p = q2_core->alloc (n ? n : 1, ZM_MEM_PSRAM);

	/* Не хватило: пока идёт загрузка карты, рендер может отдать модель прошлой карты
	 * (R_EspPurgeStale, p06) — по одной, самую большую, и снова попробовать. */
	while (!p && R_EspPurgeStale ())
		p = q2_core->alloc (n ? n : 1, ZM_MEM_PSRAM);
	/* 27.11.2026 (плата board_log35): куча раздроблена — одолжить кусок у запасных буферов кадра (bz_front.c,
	 * до 64 КБ подряд; вернётся в q2_free), и только потом отдать их насовсем. */
	if (!p)
		p = BZ_Borrow ((uint32_t)n);
	while (!p && BZ_ReleaseSpare ())
		p = q2_core->alloc (n ? n : 1, ZM_MEM_PSRAM);
	if (!p)
		out_of_memory ("malloc", n);
	return p;
}

void *q2_calloc (size_t n, size_t size)
{
	void *p;
	if (size && n > SIZE_MAX / size)
		return NULL;
	p = q2_malloc (n * size);
	if (p)
		memset (p, 0, n * size);
	return p;
}

void *q2_realloc (void *p, size_t n)
{
	void *q;
	if (!p)
		return q2_malloc (n);
	if (BZ_BorrowedSize (p))
	{
		/* одолженный у буферов кадра (q2_malloc) — в новый кусок, одолженный — вернуть */
		uint32_t had = BZ_BorrowedSize (p);
		q = q2_malloc (n);
		memcpy (q, p, had < n ? had : n);
		BZ_Unborrow (p);
		return q;
	}
	q = q2_core->realloc (p, n ? n : 1);
	while (!q && q2_mem_reclaim ())	/* как в q2_malloc: модели прошлой карты — прочь */
		q = q2_core->realloc (p, n ? n : 1);
	if (!q)
		out_of_memory ("realloc", n);
	return q;
}

void q2_free (void *p)
{
	if (p && !BZ_Unborrow (p))
		q2_core->free (p);
}

char *q2_strdup (const char *s)
{
	size_t n = strlen (s) + 1;
	char  *p = q2_malloc (n);
	if (p)
		memcpy (p, s, n);
	return p;
}

/* ---------------------------------------------------------------- файлы в памяти */

#define MEM_FILES 64            /* столько файлов в памяти разом (сохранения уровней — по 2–3 на уровень) */
#define NAME_MAX_Q2 128

typedef struct
{
	char     name[NAME_MAX_Q2]; /* путь, как его дал Quake 2; пустой — место свободно */
	uint8_t *data;
	uint32_t size, cap;
} memfile_t;

typedef struct
{
	int        used;
	memfile_t *mem;             /* файл в памяти — или…                    */
	void      *store;           /* …файл хранилища (pak)                   */
	uint32_t   size, pos;
	int        write;
} q2file_t;

static memfile_t mem_files[MEM_FILES];
static q2file_t  open_files[16];

/* Кусок упреждающего чтения файла хранилища (q2_fread, store_read_ra) */
#define RA_SIZE 8192u
static struct
{
	q2file_t *file;             /* чей кусок (NULL — нет)        */
	uint32_t  off, len;         /* с какого места файла и сколько */
	uint8_t  *buf;
} ra;

/* Сколько PSRAM держат файлы в памяти (то, что Quake 2 записал: сохранения уровня и т. п.) и сколько их —
 * для отчёта о памяти (sys_esp.c q2_mem_report). */
unsigned q2_memfile_bytes (int *count)
{
	unsigned bytes = 0;
	int      i, n = 0;

	for (i = 0; i < MEM_FILES; i++)
		if (mem_files[i].name[0])
		{
			bytes += mem_files[i].cap;
			n++;
		}
	*count = n;
	return bytes;
}

/* Пути Quake 2 сравниваются без учёта регистра и с «/» и «\» как одним знаком. */
static int path_eq (const char *a, const char *b)
{
	for (;; a++, b++)
	{
		int x = q2_tolower ((unsigned char)*a), y = q2_tolower ((unsigned char)*b);
		if (x == '\\') x = '/';
		if (y == '\\') y = '/';
		if (x != y)
			return 0;
		if (!x)
			return 1;
	}
}

static memfile_t *mem_find (const char *name)
{
	int i;
	for (i = 0; i < MEM_FILES; i++)
		if (mem_files[i].name[0] && path_eq (mem_files[i].name, name))
			return &mem_files[i];
	return NULL;
}

FILE *q2_fopen (const char *name, const char *mode)
{
	q2file_t  *f = NULL;
	memfile_t *m;
	int        i, writing = mode[0] == 'w' || mode[0] == 'a';

	for (i = 0; i < (int)(sizeof(open_files) / sizeof(open_files[0])); i++)
		if (!open_files[i].used)
		{
			f = &open_files[i];
			break;
		}
	if (!f)
		return NULL;
	memset (f, 0, sizeof(*f));
	m = mem_find (name);
	if (writing)
	{
		if (!m)
		{
			if (strlen (name) >= NAME_MAX_Q2)
				return NULL;
			for (i = 0; i < MEM_FILES && mem_files[i].name[0]; i++)
				;
			if (i == MEM_FILES)
			{
				q2_core->log ("q2: no room for file %s", name);
				return NULL;
			}
			m = &mem_files[i];
			strcpy (m->name, name);
		}
		if (mode[0] == 'w')
			m->size = 0;
		f->mem = m;
		f->write = 1;
		f->pos = mode[0] == 'a' ? m->size : 0;
	}
	else if (m)
		f->mem = m;
	else
	{
		f->store = q2_store_open (name, &f->size);
		if (!f->store)
			return NULL;
	}
	f->used = 1;
	return (FILE *)f;
}

int q2_fclose (FILE *stream)
{
	q2file_t *f = (q2file_t *)stream;
	if (!f || !f->used)
		return EOF;
	if (f->store)
		q2_store_close (f->store);
	if (ra.file == f)
		ra.file = NULL;             /* кусок упреждающего чтения — этого файла */
	memset (f, 0, sizeof(*f));
	return 0;
}

static uint32_t file_size (q2file_t *f)
{
	return f->mem ? f->mem->size : f->size;
}

/*
 * Упреждающее чтение файлов хранилища (26.11.2026). Каждый data_read — запрос к прошивке, а без флешки —
 * обмен с программой Z80 на Evo. Мелкие чтения подряд (демо: длина сообщения и само сообщение — два чтения
 * за кадр; плата board_log13: sv ~7 мс за кадр) берутся из одного куска RA_SIZE, прочитанного за раз. Чтения
 * больше половины куска (загрузка уровня, картинки) идут прямо. Кусок — одного открытого файла; при его
 * закрытии забывается (тот же дескриптор прошивки может достаться другому файлу).
 */
static uint32_t store_read_ra (q2file_t *f, void *data, uint32_t bytes)
{
	uint32_t want;

	if (bytes > RA_SIZE / 2)
		return q2_store_read (f->store, f->pos, data, bytes);
	if (ra.file != f || f->pos < ra.off || f->pos + bytes > ra.off + ra.len)
	{
		if (!ra.buf)
			ra.buf = q2_malloc (RA_SIZE);
		if (!ra.buf)
			return q2_store_read (f->store, f->pos, data, bytes);
		want = f->size - f->pos < RA_SIZE ? f->size - f->pos : RA_SIZE;
		ra.file = NULL;
		ra.len = q2_store_read (f->store, f->pos, ra.buf, want);
		ra.off = f->pos;
		ra.file = f;
		if (bytes > ra.len)
			bytes = ra.len;         /* короткое чтение — как у прямого */
	}
	memcpy (data, ra.buf + (f->pos - ra.off), bytes);
	return bytes;
}

size_t q2_fread (void *data, size_t size, size_t count, FILE *stream)
{
	q2file_t *f = (q2file_t *)stream;
	uint32_t  len, bytes;

	if (!f || !f->used || !size || !count || count > SIZE_MAX / size)
		return 0;
	len = file_size (f);
	if (f->pos >= len)
		return 0;
	bytes = (uint32_t)(size * count);
	if (bytes > len - f->pos)
		bytes = len - f->pos;
	bytes -= bytes % size;
	if (f->mem)
		memcpy (data, f->mem->data + f->pos, bytes);
	else
		bytes = store_read_ra (f, data, bytes);
	f->pos += bytes;
	return bytes / size;
}

size_t q2_fwrite (const void *data, size_t size, size_t count, FILE *stream)
{
	q2file_t  *f = (q2file_t *)stream;
	memfile_t *m;
	uint32_t   bytes, end;

	if (!f || !f->used || !f->write || !size || !count || count > SIZE_MAX / size)
		return 0;
	m = f->mem;
	bytes = (uint32_t)(size * count);
	end = f->pos + bytes;
	if (end > m->cap)
	{
		uint32_t cap = (end + 4095) & ~4095u;      /* растёт кусками по 4 КБ */
		uint8_t *p = q2_realloc (m->data, cap);
		if (!p)
			return 0;
		m->data = p;
		m->cap = cap;
	}
	if (f->pos > m->size)
		memset (m->data + m->size, 0, f->pos - m->size);
	memcpy (m->data + f->pos, data, bytes);
	f->pos = end;
	if (end > m->size)
		m->size = end;
	return count;
}

int q2_fseek (FILE *stream, long offset, int origin)
{
	q2file_t *f = (q2file_t *)stream;
	long      base;

	if (!f || !f->used)
		return -1;
	base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? (long)f->pos : (long)file_size (f);
	if (base + offset < 0)
		return -1;
	f->pos = (uint32_t)(base + offset);
	return 0;
}

long q2_ftell (FILE *stream)
{
	q2file_t *f = (q2file_t *)stream;
	return f && f->used ? (long)f->pos : -1;
}

int q2_fprintf (FILE *stream, const char *fmt, ...)
{
	char    text[1024];
	va_list ap;
	int     n;

	va_start (ap, fmt);
	n = vsnprintf (text, sizeof(text), fmt, ap);
	va_end (ap);
	if (n < 0)
		return n;
	if (n >= (int)sizeof(text))
		n = sizeof(text) - 1;
	return (int)q2_fwrite (text, 1, (size_t)n, stream);
}

int q2_fflush (FILE *stream)
{
	(void)stream;
	return 0;
}

int q2_remove (const char *name)
{
	memfile_t *m = mem_find (name);
	if (!m)
		return -1;
	q2_free (m->data);
	memset (m, 0, sizeof(*m));
	return 0;
}

int q2_rename (const char *from, const char *to)
{
	memfile_t *m = mem_find (from);
	if (!m || strlen (to) >= NAME_MAX_Q2)
		return -1;
	q2_remove (to);
	strcpy (m->name, to);
	return 0;
}

/* Образец Sys_FindFirst: «*» — любые знаки, кроме «/». */
static int wild_match (const char *p, const char *s)
{
	for (; *p; p++, s++)
	{
		if (*p == '*')
		{
			for (;; s++)
			{
				if (wild_match (p + 1, s))
					return 1;
				if (!*s || *s == '/' || *s == '\\')
					return 0;
			}
		}
		if (q2_tolower ((unsigned char)*p) != q2_tolower ((unsigned char)*s) &&
		    !((*p == '/' || *p == '\\') && (*s == '/' || *s == '\\')))
			return 0;
	}
	return !*s;
}

const char *q2_mem_find (const char *pattern, int *iter)
{
	for (; *iter < MEM_FILES; (*iter)++)
		if (mem_files[*iter].name[0] && wild_match (pattern, mem_files[*iter].name))
			return mem_files[(*iter)++].name;
	return NULL;
}

/* ---------------------------------------------------------------- строки */

int q2_vsprintf (char *out, const char *fmt, va_list ap)
{
	/* у id буферы 1024…4096 байт; vsnprintf прошивки с запасом — как vsprintf без предела */
	return vsnprintf (out, 32768, fmt, ap);
}

/* sscanf у Quake 2 — только «%f %f %f» (оси неба, векторы из строки сущностей); здесь %f, %d и %i. */
int q2_sscanf (const char *s, const char *fmt, ...)
{
	va_list ap;
	int     got = 0;

	va_start (ap, fmt);
	while (*fmt)
	{
		if (isspace ((unsigned char)*fmt))
		{
			while (isspace ((unsigned char)*s))
				s++;
			fmt++;
			continue;
		}
		if (*fmt == '%' && (fmt[1] == 'f' || fmt[1] == 'd' || fmt[1] == 'i'))
		{
			char *end;
			while (isspace ((unsigned char)*s))
				s++;
			if (fmt[1] == 'f')
			{
				float v = (float)strtod (s, &end);
				if (end == s)
					break;
				*va_arg (ap, float *) = v;
			}
			else
			{
				long v = strtol (s, &end, fmt[1] == 'i' ? 0 : 10);
				if (end == s)
					break;
				*va_arg (ap, int *) = (int)v;
			}
			s = end;
			got++;
			fmt += 2;
			continue;
		}
		if (*fmt != *s)
			break;
		fmt++;
		s++;
	}
	va_end (ap);
	return got;
}

double q2_atof (const char *s)
{
	return strtod (s, NULL);
}

char *q2_strtok (char *s, const char *delim)
{
	static char *next;
	char        *start;

	if (s)
		next = s;
	if (!next)
		return NULL;
	while (*next && strchr (delim, *next))      /* strspn прошивка не даёт */
		next++;
	if (!*next)
	{
		next = NULL;
		return NULL;
	}
	start = next;
	next += strcspn (next, delim);
	if (*next)
		*next++ = 0;
	else
		next = NULL;
	return start;
}

char *q2_strlwr (char *s)
{
	char *p;
	for (p = s; *p; p++)
		*p = (char)q2_tolower ((unsigned char)*p);
	return s;
}

int q2_toupper (int c)
{
	return c >= 'a' && c <= 'z' ? c - 32 : c;
}

int q2_tolower (int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

char *q2_getenv (const char *name)
{
	(void)name;
	return NULL;
}

/* команда консоли setenv — переменных окружения у модуля нет */
int q2_putenv (char *s)
{
	(void)s;
	return 0;
}

/* stpcpy (копия строки, итог — её конец) GCC подставляет сам вместо пары strcpy + strlen;
 * прошивка его не даёт — своя */
char *stpcpy (char *d, const char *s)
{
	while ((*d = *s++))
		d++;
	return d;
}

int q2_printf (const char *fmt, ...)
{
	char    text[256];
	va_list ap;
	int     n;

	va_start (ap, fmt);
	n = vsnprintf (text, sizeof(text), fmt, ap);
	va_end (ap);
	q2_core->log ("%s", text);
	return n;
}

time_t q2_time (time_t *t)
{
	struct timeval tv;
	gettimeofday (&tv, NULL);
	if (t)
		*t = tv.tv_sec;
	return tv.tv_sec;
}

struct tm *q2_localtime (const time_t *t)
{
	static struct tm tm;
	return localtime_r (t, &tm);
}

void q2_exit (int code)
{
	q2_esp_exit (code);
}
