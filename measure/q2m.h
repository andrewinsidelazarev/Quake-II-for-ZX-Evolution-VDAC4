/*
 * q2m.h — замерочная сборка Quake 2 для VDAC4 (движок).
 *
 * Счётчики процессорного времени по частям кадра. Время меряется настоящими
 * часами Windows (QueryPerformanceCounter), а игра идёт по виртуальным часам
 * (Sys_Milliseconds в sys_q2m.c): каждый вызов Qcommon_Frame — ровно 1/30 с
 * игрового времени. Так замер не зависит от скорости ПК: игра делает ту же
 * работу, что сделала бы при 30 кадрах/с, а мы видим, сколько это стоит.
 *
 * Части (вставлены в копию исходников id сценарием measure/build.py):
 *   Q2M_SV   — SV_Frame целиком (сервер: приём команд, игра, рассылка);
 *   Q2M_GAME — ge->RunFrame (игровая логика: монстры, физика, оружие) — часть SV;
 *   Q2M_CL   — CL_Frame целиком (клиент: разбор, предсказание, экран, звук);
 *   Q2M_SCR  — SCR_UpdateScreen (3D-вид, 2D-поверх, конец кадра) — часть CL;
 *   Q2M_RF   — re.RenderFrame (3D-вид мира) — часть SCR;
 *   Q2M_SND  — S_Update (смешивание звука) — часть CL.
 */
#ifndef Q2M_H
#define Q2M_H

enum { Q2M_SV, Q2M_GAME, Q2M_CL, Q2M_SCR, Q2M_RF, Q2M_SND, Q2M_N };

long long Q2M_Now (void);
extern long long q2m_t0[Q2M_N];
extern long long q2m_acc[Q2M_N];

#define Q2M_BEGIN(i) (q2m_t0[i] = Q2M_Now ())
#define Q2M_END(i)   (q2m_acc[i] += Q2M_Now () - q2m_t0[i])

#endif
