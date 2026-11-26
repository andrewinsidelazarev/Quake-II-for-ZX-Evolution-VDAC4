/*
 * vid_q2m.c — замерочная сборка Quake 2 для VDAC4: «видео» без окна.
 *
 * Заменяет win32/vid_dll.c. Рендер грузится из DLL рядом с программой:
 *   vid_ref cnt  → ref_cnt.dll  — рендер GL с заглушками, считает поток для ПЛИС;
 *   vid_ref soft → ref_soft.dll — программный рендер id без ассемблера, рисует в память.
 * Окна, режимов экрана и меню видео нет.
 */

#include "../client/client.h"
#include <windows.h>

viddef_t    viddef;
refexport_t re;

static HINSTANCE reflib;
#ifdef REF_HARD_LINKED
/* рендер в этой же программе: в VID_Init имя GetRefAPI занято переменной, поэтому адрес функции
 * берём здесь, на уровне файла */
refexport_t GetRefAPI (refimport_t rimp);
static GetRefAPI_t q2esp_get_ref = GetRefAPI;
#endif
cvar_t *vid_ref;
cvar_t *vid_fullscreen;
cvar_t *vid_gamma;

#define MAXPRINTMSG 4096

void VID_Printf (int print_level, char *fmt, ...)
{
	va_list argptr;
	char    msg[MAXPRINTMSG];

	va_start (argptr, fmt);
	vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);
	if (print_level == PRINT_ALL)
		Com_Printf ("%s", msg);
	else
		Com_DPrintf ("%s", msg);
}

void VID_Error (int err_level, char *fmt, ...)
{
	va_list argptr;
	char    msg[MAXPRINTMSG];

	va_start (argptr, fmt);
	vsnprintf (msg, sizeof(msg), fmt, argptr);
	va_end (argptr);
	Com_Error (err_level, "%s", msg);
}

void VID_NewWindow (int width, int height)
{
	viddef.width = width;
	viddef.height = height;
}

typedef struct
{
	int width, height;
} q2m_mode_t;

/* режимы id; 10 и 11 добавлены для 16:9 (640×360 и 320×180), 12 — 424×240 (16:9 адаптера VDAC4, ×3 = 1272×720) */
static q2m_mode_t vid_modes[] =
{
	{ 320, 240 }, { 400, 300 }, { 512, 384 }, { 640, 480 }, { 800, 600 },
	{ 960, 720 }, { 1024, 768 }, { 1152, 864 }, { 1280, 960 }, { 1600, 1200 },
	{ 640, 360 }, { 320, 180 }, { 424, 240 }
};

qboolean VID_GetModeInfo (int *width, int *height, int mode)
{
	if (mode < 0 || mode >= (int)(sizeof(vid_modes) / sizeof(vid_modes[0])))
		return false;
	*width = vid_modes[mode].width;
	*height = vid_modes[mode].height;
	return true;
}

void VID_MenuInit (void)
{
}

void VID_MenuDraw (void)
{
}

const char *VID_MenuKey (int k)
{
	return NULL;
}

void VID_Init (void)
{
	refimport_t ri;
	char        name[MAX_OSPATH];
	char       *slash;
	GetRefAPI_t GetRefAPI;

	vid_ref = Cvar_Get ("vid_ref", "cnt", CVAR_ARCHIVE);
	vid_fullscreen = Cvar_Get ("vid_fullscreen", "0", CVAR_ARCHIVE);
	vid_gamma = Cvar_Get ("vid_gamma", "1", CVAR_ARCHIVE);
	viddef.width = 320;
	viddef.height = 240;

#ifdef REF_HARD_LINKED
	/* Рендер собран в ту же программу (сборка q2esp — как на ESP, где DLL нет): GetRefAPI — его
	 * обычная функция (ref_soft/r_main.c), vid_ref не нужен. */
	strcpy (name, "ref (hard linked)");
	(void)slash;
	GetRefAPI = q2esp_get_ref;
#else
	GetModuleFileNameA (NULL, name, sizeof(name));
	slash = strrchr (name, '\\');
	if (slash)
		Com_sprintf (slash + 1, (int)(sizeof(name) - (slash + 1 - name)), "ref_%s.dll", vid_ref->string);
	reflib = LoadLibraryA (name);
	if (!reflib)
		Com_Error (ERR_FATAL, "нет %s", name);
	GetRefAPI = (void *)GetProcAddress (reflib, "GetRefAPI");
	if (!GetRefAPI)
		Com_Error (ERR_FATAL, "в %s нет GetRefAPI", name);
#endif

	ri.Cmd_AddCommand = Cmd_AddCommand;
	ri.Cmd_RemoveCommand = Cmd_RemoveCommand;
	ri.Cmd_Argc = Cmd_Argc;
	ri.Cmd_Argv = Cmd_Argv;
	ri.Cmd_ExecuteText = Cbuf_ExecuteText;
	ri.Con_Printf = VID_Printf;
	ri.Sys_Error = VID_Error;
	ri.FS_LoadFile = FS_LoadFile;
	ri.FS_FreeFile = FS_FreeFile;
	ri.FS_Gamedir = FS_Gamedir;
	ri.Cvar_Get = Cvar_Get;
	ri.Cvar_Set = Cvar_Set;
	ri.Cvar_SetValue = Cvar_SetValue;
	ri.Vid_GetModeInfo = VID_GetModeInfo;
	ri.Vid_MenuInit = VID_MenuInit;
	ri.Vid_NewWindow = VID_NewWindow;
#ifdef SCR_HAS_LOADBAR		// наш движок (port): полоса загрузки (cl_scrn.c, 26.11.2026); у id её нет
	ri.LoadingProgress = SCR_LoadingWorldProgress;
#endif

	re = GetRefAPI (ri);
	if (re.api_version != API_VERSION)
		Com_Error (ERR_FATAL, "%s: api_version %d вместо %d", name, re.api_version, API_VERSION);
	if (re.Init (GetModuleHandleA (NULL), NULL) == -1)
		Com_Error (ERR_FATAL, "%s: R_Init не прошёл", name);
	cls.disable_screen = 0;
}

void VID_Shutdown (void)
{
	if (re.Shutdown)
		re.Shutdown ();
	if (reflib)
		FreeLibrary (reflib);
	reflib = NULL;
	memset (&re, 0, sizeof(re));
}

void VID_CheckChanges (void)
{
}
