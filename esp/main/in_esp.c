/*
 * =====================================================================================================
 * in_esp.c — ввод Quake 2 на ESP (у id — win32/in_win.c, «пустой» вариант — null/in_null.c).
 * =====================================================================================================
 *
 * Клавиатура и мышь придут с Evo (программа Z80 читает очередь PS/2 и мышь Kempston, как у DOOM.ELF) —
 * это следующий шаг. Пока ввода нет, но cvar in_joystick заводится, как у id: меню «Options»
 * читает его значение, и без него игра упала бы на пустом указателе.
 */
#include "q2esp.h"
#include "../../port/client/client.h"

cvar_t *in_joystick;

void IN_Init (void)
{
	in_joystick = Cvar_Get ("in_joystick", "0", CVAR_ARCHIVE);
}

void IN_Shutdown (void)
{
}

void IN_Commands (void)
{
}

void IN_Frame (void)
{
}

void IN_Move (usercmd_t *cmd)
{
	(void)cmd;
}

void IN_Activate (qboolean active)
{
	(void)active;
}

void IN_ActivateMouse (void)
{
}

void IN_DeactivateMouse (void)
{
}
