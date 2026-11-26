/*
 * =====================================================================================================
 * vid_esp.c — «видео» Quake 2 на ESP (у id — win32/vid_dll.c; образец — measure/vid_q2m.c).
 * =====================================================================================================
 *
 * У id рендер — отдельная библиотека (ref_soft.dll, ref_gl.dll), её выбирает cvar vid_ref. Здесь рендер
 * один — ref_esp (программный рендер id без рисования, многоугольники уходят командами в ядро Tang) —
 * и собран в этот же модуль: GetRefAPI — его обычная функция. Окна нет; картинку выводит ядро Tang по
 * HDMI.
 *
 * Режимы — как у id, плюс 10 (640×360), 11 (320×180) и 12 (424×240 — 16:9 «бомж-3D», вывод ×3 в 720p).
 */
#include "q2esp.h"
#include "../../port/client/client.h"

viddef_t    viddef;
refexport_t re;

cvar_t *vid_ref;
cvar_t *vid_fullscreen;
cvar_t *vid_gamma;

refexport_t GetRefAPI (refimport_t rimp);

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

static const struct { int width, height; } vid_modes[] =
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

/* меню видео у id выбирает библиотеку и режим — здесь выбирать нечего */
void VID_MenuInit (void)
{
}

void VID_MenuDraw (void)
{
}

const char *VID_MenuKey (int k)
{
	(void)k;
	return NULL;
}

void VID_Init (void)
{
	refimport_t ri;

	vid_ref = Cvar_Get ("vid_ref", "esp", CVAR_NOSET);
	vid_fullscreen = Cvar_Get ("vid_fullscreen", "0", CVAR_ARCHIVE);
	vid_gamma = Cvar_Get ("vid_gamma", "1", CVAR_ARCHIVE);
	viddef.width = 424;
	viddef.height = 240;

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
	ri.LoadingProgress = SCR_LoadingWorldProgress;	// полоса загрузки (cl_scrn.c)

	re = GetRefAPI (ri);
	if (re.api_version != API_VERSION)
		Com_Error (ERR_FATAL, "ref_esp: api_version %d instead of %d", re.api_version, API_VERSION);
	if (re.Init (NULL, NULL) == -1)
		Com_Error (ERR_FATAL, "ref_esp: R_Init failed");
	cls.disable_screen = 0;
}

void VID_Shutdown (void)
{
	if (re.Shutdown)
		re.Shutdown ();
	memset (&re, 0, sizeof(re));
}

void VID_CheckChanges (void)
{
}
