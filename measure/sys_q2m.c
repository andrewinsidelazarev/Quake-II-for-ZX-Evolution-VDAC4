/*
 * sys_q2m.c — замерочная сборка Quake 2 для VDAC4: платформа и сценарии.
 *
 * Что делает:
 *  - заменяет win32/sys_win.c: функции Sys_*, загрузка gamex86.dll из папки программы;
 *  - ведёт ВИРТУАЛЬНЫЕ часы: Sys_Milliseconds возвращает игровое время, и каждый
 *    вызов Qcommon_Frame — ровно 1/30 с (33, 33, 34 мс). Игра, демо и звук идут так,
 *    будто машина держит ровно 30 кадров/с, а процессорное время каждой части кадра
 *    мерится настоящими часами (q2m.h);
 *  - гонит сценарий из командной строки и пишет результаты в файлы.
 *
 * Запуск:  q2m.exe <префикс_вывода> <сценарий> [параметры] -- <параметры Quake 2>
 *   mapstats <карта,карта,...>  — загрузить каждую карту, записать память по видам
 *   demo <файл.dm2>             — проиграть демо, по кадрам записать время частей кадра
 *   fight <карта> <секунды> [сложность] — бой: бессмертие, всё оружие, бег и стрельба
 *                               (сложность 0–3, по умолчанию 1; на 3 больше всего монстров)
 *
 * Файлы: <префикс>.frames.csv — время частей кадра по кадрам (мкс);
 *        <префикс>.engine.jsonl — память движка по картам (сценарий mapstats);
 *        <префикс>.console.log — всё, что движок напечатал в консоль.
 * Считающий рендер (ref_cnt) пишет свои файлы с тем же префиксом (cvar q2m_out).
 */

#include "../client/client.h"
#include "../client/snd_loc.h"
#include "q2m.h"

#include <windows.h>
#include <mmsystem.h>
#include <direct.h>
#include <stdio.h>

/* ---------------------------------------------------------------- время */

long long q2m_t0[Q2M_N];
long long q2m_acc[Q2M_N];

static LARGE_INTEGER q2m_freq;

long long Q2M_Now (void)
{
	LARGE_INTEGER t;
	QueryPerformanceCounter (&t);
	return t.QuadPart;
}

static double Q2M_Us (long long ticks)
{
	return (double)ticks * 1e6 / (double)q2m_freq.QuadPart;
}

/* Виртуальные часы игры, мс. Двигает только главный цикл. */
int q2m_sim_ms = 0x10000;
int curtime;

int Sys_Milliseconds (void)
{
	curtime = q2m_sim_ms;
	return curtime;
}

/* -------------------------------------------------------------- вывод */

static char  q2m_prefix[MAX_OSPATH];
static FILE *q2m_console;
static FILE *q2m_frames;
static FILE *q2m_engine;

static FILE *Q2M_Open (const char *suffix)
{
	char  name[MAX_OSPATH + 32];
	FILE *f;

	Com_sprintf (name, sizeof(name), "%s%s", q2m_prefix, suffix);
	f = fopen (name, "w");
	if (!f)
	{
		printf ("не открыть %s\n", name);
		exit (2);
	}
	return f;
}

static void Q2M_CloseAll (void)
{
	if (q2m_console) fclose (q2m_console);
	if (q2m_frames)  fclose (q2m_frames);
	if (q2m_engine)  fclose (q2m_engine);
	q2m_console = q2m_frames = q2m_engine = NULL;
}

/* ------------------------------------------------------- функции Sys_* */

unsigned sys_frame_time;

void Sys_Error (char *error, ...)
{
	va_list argptr;
	char    text[4096];

	va_start (argptr, error);
	vsnprintf (text, sizeof(text), error, argptr);
	va_end (argptr);

	printf ("Sys_Error: %s\n", text);
	if (q2m_console)
		fprintf (q2m_console, "Sys_Error: %s\n", text);
	Q2M_CloseAll ();
	exit (1);
}

void Sys_Quit (void)
{
	Q2M_CloseAll ();
	exit (0);
}

#ifdef GAME_HARD_LINKED
/*
 * Игра собрана в ту же программу (сборка q2esp — как будет на ESP: один файл ELF, библиотек DLL там
 * нет). GetGameAPI — обычная функция игры (game/g_main.c), её просто зовём; выгружать нечего.
 */
void *GetGameAPI (void *import);

void Sys_UnloadGame (void)
{
}

void *Sys_GetGameAPI (void *parms)
{
	return GetGameAPI (parms);
}
#else
static HINSTANCE game_library;

void Sys_UnloadGame (void)
{
	if (game_library)
		FreeLibrary (game_library);
	game_library = NULL;
}

/* gamex86.dll берётся только из папки самой программы: собран тем же сценарием. */
void *Sys_GetGameAPI (void *parms)
{
	void *(*GetGameAPI) (void *);
	char  name[MAX_OSPATH];
	char *slash;

	if (game_library)
		Com_Error (ERR_FATAL, "Sys_GetGameAPI without Sys_UnloadingGame");

	GetModuleFileNameA (NULL, name, sizeof(name));
	slash = strrchr (name, '\\');
	if (slash)
		strcpy (slash + 1, "gamex86.dll");
	game_library = LoadLibraryA (name);
	if (!game_library)
		return NULL;
	GetGameAPI = (void *)GetProcAddress (game_library, "GetGameAPI");
	if (!GetGameAPI)
	{
		Sys_UnloadGame ();
		return NULL;
	}
	return GetGameAPI (parms);
}
#endif

char *Sys_ConsoleInput (void)
{
	return NULL;
}

void Sys_ConsoleOutput (char *string)
{
	if (q2m_console)
		fputs (string, q2m_console);
}

void Sys_SendKeyEvents (void)
{
	sys_frame_time = q2m_sim_ms;
}

void Sys_AppActivate (void)
{
}

void Sys_CopyProtect (void)
{
}

char *Sys_GetClipboardData (void)
{
	return NULL;
}

cvar_t *in_joystick;	/* в id живёт в in_win.c; нужен меню настроек */

void Sys_Init (void)
{
	in_joystick = Cvar_Get ("in_joystick", "0", CVAR_ARCHIVE);
}

static void Q2M_Copy (char *dst, const char *src, int size)
{
	strncpy (dst, src, size - 1);
	dst[size - 1] = 0;
}

/* -------------------------------------------- сведения о памяти движка */

void Q2M_ZoneStats (FILE *f);		/* дописана в конец common.c */
void Q2M_CmodelStats (FILE *f);		/* дописана в конец cmodel.c */
int  Q2M_NumEdicts (void);			/* дописана в конец sv_game.c: объектов игры занято */

static int q2m_edicts_peak;			/* пик числа объектов с загрузки карты (сбрасывает q2m_stats) */

extern sfx_t known_sfx[];
extern int   num_sfx;
extern int   s_registration_sequence;

static void Q2M_JsonString (FILE *f, const char *s)
{
	fputc ('"', f);
	for ( ; *s; s++)
	{
		if (*s == '"' || *s == '\\')
			fputc ('\\', f);
		if ((unsigned char)*s >= 32)
			fputc (*s, f);
	}
	fputc ('"', f);
}

/* Консольная команда «q2m_stats <имя>»: строка JSON с памятью движка на этой карте. */
static void Q2M_Stats_f (void)
{
	int   i, first = 1;
	FILE *f = q2m_engine;

	if (!f)
		return;
	fprintf (f, "{\"map\":");
	Q2M_JsonString (f, Cmd_Argv (1));
	fprintf (f, ",");
	Q2M_ZoneStats (f);
	fprintf (f, ",");
	Q2M_CmodelStats (f);
	fprintf (f, ",\"edicts\":%d,\"edicts_peak\":%d", Q2M_NumEdicts (), q2m_edicts_peak);
	q2m_edicts_peak = 0;
	fprintf (f, ",\"dma_speed\":%d,\"sounds\":[", dma.speed);
	for (i = 0; i < num_sfx; i++)
	{
		sfx_t *sfx = &known_sfx[i];

		if (!sfx->name[0] || sfx->registration_sequence != s_registration_sequence)
			continue;
		fprintf (f, first ? "[" : ",[");
		first = 0;
		Q2M_JsonString (f, sfx->name);
		if (sfx->cache)
			fprintf (f, ",%d,%d,%d,%d]", sfx->cache->length, sfx->cache->width,
				sfx->cache->speed, sfx->cache->stereo);
		else
			fprintf (f, ",0,0,0,0]");
	}
	fprintf (f, "]}\n");
	fflush (f);
}

/* --------------------------------------------------------- сценарии */

enum { SC_MAPSTATS, SC_DEMO, SC_FIGHT };

static int   sc_kind;
static char  sc_maps[64][MAX_QPATH];
static int   sc_nummaps;
static char  sc_demo[MAX_QPATH];
static int   sc_seconds;
static int   sc_skill = 1;	/* сложность боя (fight) */

static int   sc_state;		/* шаг сценария */
static int   sc_index;		/* номер карты (mapstats) */
static int   sc_wait;		/* кадров в текущем шаге */
static int   sc_measuring;	/* 1 — кадры идут в итог */
static int   sc_fight_frame;

static cvar_t *q2m_frame_cvar;

static qboolean Q2M_InGame (void)
{
	return cls.state == ca_active && cl.refresh_prepped;
}

/* Один шаг сценария после очередного кадра. Возвращает 0, когда сценарий закончен. */
static int Q2M_Scenario (int frame)
{
	sc_wait++;
	if (frame < 10)
		return 1;	/* дать движку подняться: консоль, звук, рендер */

	switch (sc_kind)
	{
	case SC_MAPSTATS:
		if (sc_state == 0)
		{
			if (sc_index >= sc_nummaps)
				return 0;
			printf ("карта %s\n", sc_maps[sc_index]);
			Cbuf_AddText (va ("map %s\n", sc_maps[sc_index]));
			/* пик объектов — с этой карты: до сброса в q2m_stats шли ещё три кадра прежней карты,
			   и её пик попадал в запись следующей (24.11) */
			q2m_edicts_peak = 0;
			sc_state = 1;
			sc_wait = 0;
		}
		else if (sc_state == 1)
		{
			static int active;

			active = Q2M_InGame () ? active + 1 : 0;
			if (active >= 15)
			{
				Cbuf_AddText (va ("q2m_stats %s\nq2m_refstats %s\n",
					sc_maps[sc_index], sc_maps[sc_index]));
				sc_state = 2;
				sc_wait = 0;
				active = 0;
			}
			else if (sc_wait > 3000)
			{
				printf ("  карта %s не загрузилась\n", sc_maps[sc_index]);
				sc_index++;
				sc_state = 0;
			}
		}
		else if (sc_wait >= 3)
		{
			sc_index++;
			sc_state = 0;
		}
		return 1;

	case SC_DEMO:
		if (sc_state == 0)
		{
			Cbuf_AddText (va ("demomap %s\n", sc_demo));
			sc_state = 1;
			sc_wait = 0;
		}
		else if (sc_state == 1)
		{
			if (Q2M_InGame ())
			{
				sc_measuring = 1;
				sc_state = 2;
			}
			else if (sc_wait > 3000)
			{
				printf ("демо %s не пошло\n", sc_demo);
				return 0;
			}
		}
		else if (!Q2M_InGame () || Com_ServerState () == 0)
		{
			sc_measuring = 0;
			return 0;	/* демо кончилось: сервер погашен, клиент отключён */
		}
		return 1;

	case SC_FIGHT:
		if (sc_state == 0)
		{
			Cbuf_AddText (va ("skill %d\nmap %s\n", sc_skill, sc_maps[0]));
			sc_state = 1;
			sc_wait = 0;
		}
		else if (sc_state == 1)
		{
			static int active;

			active = Q2M_InGame () ? active + 1 : 0;
			if (active >= 30)
			{
				/* бессмертие и всё оружие, бег вперёд и стрельба без остановки */
				Cbuf_AddText ("god\ngive all\nuse rocket launcher\n+forward\n+attack\n");
				sc_measuring = 1;
				sc_fight_frame = 0;
				sc_state = 2;
			}
			else if (sc_wait > 3000)
			{
				printf ("карта %s не загрузилась\n", sc_maps[0]);
				return 0;
			}
		}
		else
		{
			int t = sc_fight_frame++;

			/* каждые 1,5 с полсекунды поворачивать: не стоять лицом в стену */
			if (t % 45 == 0)
				Cbuf_AddText ("+left\n");
			if (t % 45 == 15)
				Cbuf_AddText ("-left\n");
			/* каждые 10 с менять оружие: ракеты, гипербластер, пулемёт */
			if (t % 300 == 100)
				Cbuf_AddText ("use hyperblaster\n");
			if (t % 300 == 200)
				Cbuf_AddText ("use chaingun\n");
			if (t % 300 == 0 && t)
				Cbuf_AddText ("use rocket launcher\n");
			if (!Q2M_InGame () && t > 30)
			{
				printf ("бой прервался на кадре %d\n", t);
				return 0;
			}
			if (t >= sc_seconds * 30)
				return 0;
		}
		return 1;
	}
	return 0;
}

static void Q2M_SplitMaps (const char *list)
{
	const char *p = list;

	while (*p && sc_nummaps < 64)
	{
		int n = 0;

		while (*p && *p != ',' && n < MAX_QPATH - 1)
			sc_maps[sc_nummaps][n++] = *p++;
		sc_maps[sc_nummaps][n] = 0;
		if (n)
			sc_nummaps++;
		if (*p == ',')
			p++;
	}
}

static void Q2M_Usage (void)
{
	printf ("q2m.exe <префикс> mapstats <карты через запятую> -- <параметры Quake 2>\n"
	        "q2m.exe <префикс> demo <файл.dm2> -- ...\n"
	        "q2m.exe <префикс> fight <карта> <секунды> [сложность 0-3] -- ...\n");
	exit (2);
}

/* --------------------------------------------------------------- main */

int main (int argc, char **argv)
{
	static char *qargv[64];
	static char  outarg[MAX_OSPATH + 16];
	int   qargc = 0, i, frame, dash = -1;
	static const int steps[3] = { 33, 33, 34 };

	QueryPerformanceFrequency (&q2m_freq);
	SetPriorityClass (GetCurrentProcess (), HIGH_PRIORITY_CLASS);
	/* одно ядро: ровнее замер. Q2M_FREE_CPU=1 — без привязки (прогоны не на время, например пик
	   объектов по всем картам: часы игры виртуальные, итог от ядра не зависит) */
	if (!getenv ("Q2M_FREE_CPU"))
		SetThreadAffinityMask (GetCurrentThread (), 1 << 2);

	for (i = 1; i < argc; i++)
		if (!strcmp (argv[i], "--"))
		{
			dash = i;
			break;
		}
	if (argc < 4 || dash < 0)
		Q2M_Usage ();

	Q2M_Copy (q2m_prefix, argv[1], sizeof(q2m_prefix));
	if (!strcmp (argv[2], "mapstats") && dash >= 4)
	{
		sc_kind = SC_MAPSTATS;
		Q2M_SplitMaps (argv[3]);
	}
	else if (!strcmp (argv[2], "demo") && dash >= 4)
	{
		sc_kind = SC_DEMO;
		Q2M_Copy (sc_demo, argv[3], sizeof(sc_demo));
	}
	else if (!strcmp (argv[2], "fight") && dash >= 5)
	{
		sc_kind = SC_FIGHT;
		Q2M_SplitMaps (argv[3]);
		sc_seconds = atoi (argv[4]);
		if (dash >= 6)
			sc_skill = atoi (argv[5]);
	}
	else
		Q2M_Usage ();

	q2m_console = Q2M_Open (".console.log");
	q2m_frames = Q2M_Open (".frames.csv");
	if (sc_kind == SC_MAPSTATS)
		q2m_engine = Q2M_Open (".engine.jsonl");

	/* параметры Quake 2: всё после «--», плюс префикс для считающего рендера */
	qargv[qargc++] = "q2m";
	for (i = dash + 1; i < argc && qargc < 56; i++)
		qargv[qargc++] = argv[i];
	qargv[qargc++] = "+set";
	qargv[qargc++] = "q2m_out";
	Com_sprintf (outarg, sizeof(outarg), "%s", q2m_prefix);
	qargv[qargc++] = outarg;
	/* поздняя команда: без неё Qcommon_Init запустит заставку «d1» */
	qargv[qargc++] = "+echo";
	qargv[qargc++] = "q2m";

	Qcommon_Init (qargc, qargv);
	Cmd_AddCommand ("q2m_stats", Q2M_Stats_f);
	q2m_frame_cvar = Cvar_Get ("q2m_frame", "0", 0);

	fprintf (q2m_frames, "frame,sim_ms,measuring,state,total_us,sv_us,game_us,cl_us,scr_us,rf_us,snd_us,edicts\n");

	for (frame = 0; ; frame++)
	{
		long long before[Q2M_N], t0, t1;
		int msec = steps[frame % 3];

		q2m_sim_ms += msec;
		q2m_frame_cvar->value = (float)frame;	/* номер кадра видит рендер */
		memcpy (before, q2m_acc, sizeof(before));
		t0 = Q2M_Now ();
		Qcommon_Frame (msec);
		t1 = Q2M_Now ();

		{
			int edicts = Q2M_NumEdicts ();

			if (edicts > q2m_edicts_peak)
				q2m_edicts_peak = edicts;
		}
		fprintf (q2m_frames, "%d,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d\n",
			frame, q2m_sim_ms, sc_measuring, cls.state, Q2M_Us (t1 - t0),
			Q2M_Us (q2m_acc[Q2M_SV] - before[Q2M_SV]),
			Q2M_Us (q2m_acc[Q2M_GAME] - before[Q2M_GAME]),
			Q2M_Us (q2m_acc[Q2M_CL] - before[Q2M_CL]),
			Q2M_Us (q2m_acc[Q2M_SCR] - before[Q2M_SCR]),
			Q2M_Us (q2m_acc[Q2M_RF] - before[Q2M_RF]),
			Q2M_Us (q2m_acc[Q2M_SND] - before[Q2M_SND]), q2m_edicts_peak);

		if (!Q2M_Scenario (frame))
			break;
		if (frame > 30 * 60 * 30)
		{
			printf ("сценарий не кончился за 30 минут игрового времени\n");
			break;
		}
	}

	printf ("готово: %d кадров\n", frame + 1);
	Q2M_CloseAll ();
	exit (0);	/* все модули на общей CRT (/MD): exit допишет и файлы рендера */
	return 0;
}
