/*
 * =====================================================================================================
 * net_esp.c — сеть Quake 2 на ESP: только «петля» внутри модуля (у id — win32/net_wins.c).
 * =====================================================================================================
 *
 * Даже в одиночной игре Quake 2 — это клиент и сервер, которые обмениваются пакетами. Когда оба в одной
 * программе, пакеты не уходят в сеть: сервер кладёт их в кольцо loopbacks[клиент], клиент — в
 * loopbacks[сервер] (по 4 пакета, как у id). Здесь только это; адреса IP/IPX и сокеты (игра по сети)
 * не нужны: NET_StringToAdr знает лишь «localhost».
 * Код петли — из net_wins.c id без изменений.
 */
#include "q2esp.h"
#include "../../port/qcommon/qcommon.h"

#define MAX_LOOPBACK 4

typedef struct
{
	byte data[MAX_MSGLEN];
	int  datalen;
} loopmsg_t;

typedef struct
{
	loopmsg_t msgs[MAX_LOOPBACK];
	int       get, send;
} loopback_t;

cvar_t            *net_shownet;
static loopback_t  loopbacks[2];

qboolean NET_CompareAdr (netadr_t a, netadr_t b)
{
	return a.type == b.type && a.type == NA_LOOPBACK;
}

qboolean NET_CompareBaseAdr (netadr_t a, netadr_t b)
{
	return a.type == b.type && a.type == NA_LOOPBACK;
}

char *NET_AdrToString (netadr_t a)
{
	static char s[64];
	Com_sprintf (s, sizeof(s), a.type == NA_LOOPBACK ? "loopback" : "none");
	return s;
}

qboolean NET_StringToAdr (char *s, netadr_t *a)
{
	memset (a, 0, sizeof(*a));
	if (!strcmp (s, "localhost"))
	{
		a->type = NA_LOOPBACK;
		return true;
	}
	return false;               /* игры по сети нет */
}

qboolean NET_IsLocalAddress (netadr_t adr)
{
	return adr.type == NA_LOOPBACK;
}

static qboolean NET_GetLoopPacket (netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
	int         i;
	loopback_t *loop = &loopbacks[sock];

	if (loop->send - loop->get > MAX_LOOPBACK)
		loop->get = loop->send - MAX_LOOPBACK;
	if (loop->get >= loop->send)
		return false;
	i = loop->get & (MAX_LOOPBACK - 1);
	loop->get++;
	memcpy (net_message->data, loop->msgs[i].data, loop->msgs[i].datalen);
	net_message->cursize = loop->msgs[i].datalen;
	memset (net_from, 0, sizeof(*net_from));
	net_from->type = NA_LOOPBACK;
	return true;
}

static void NET_SendLoopPacket (netsrc_t sock, int length, void *data, netadr_t to)
{
	int         i;
	loopback_t *loop = &loopbacks[sock ^ 1];

	(void)to;
	i = loop->send & (MAX_LOOPBACK - 1);
	loop->send++;
	memcpy (loop->msgs[i].data, data, length);
	loop->msgs[i].datalen = length;
}

qboolean NET_GetPacket (netsrc_t sock, netadr_t *net_from, sizebuf_t *net_message)
{
	return NET_GetLoopPacket (sock, net_from, net_message);
}

void NET_SendPacket (netsrc_t sock, int length, void *data, netadr_t to)
{
	if (to.type == NA_LOOPBACK)
		NET_SendLoopPacket (sock, length, data, to);
}

void NET_Config (qboolean multiplayer)
{
	(void)multiplayer;
}

void NET_Sleep (int msec)
{
	(void)msec;
}

void NET_Init (void)
{
	net_shownet = Cvar_Get ("net_shownet", "0", 0);
}

void NET_Shutdown (void)
{
}
