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
 * Части рендера ESP (ref_esp, p06; 25.11.2026 — где время кадра на плате) — части RF:
 *   Q2M_R_SETUP — R_SetupFrame, R_MarkLeaves, R_PushDlights; Q2M_R_WORLD — R_RenderWorld (обход BSP,
 *   грани мира — команды адаптера); Q2M_R_BMODEL — двери, лифты; Q2M_R_ENT — модели (md2, спрайты);
 *   Q2M_R_PART — частицы; Q2M_R_ALPHA — прозрачные грани.
 * ESP (esp/main/bz_link.c): Q2M_LINK — отправка потока команд в ядро (bz_link_send), Q2M_LREAD —
 *   чтение SDRAM ядра (кадры md2, звуки) — они внутри частей выше.
 * Внутри частей (gpu/front/bz_front.c): Q2M_F_FACE — грань мира целиком (bz_face), Q2M_F_LM — её
 *   lightmap (lm_get), Q2M_F_TEX — текстура (tex_get), Q2M_A_TRI — треугольник модели (BZ_AliasTriangle).
 * Q2M_LWAIT — игра ждёт задачу LINK (bz_link_wait_idle): с 25.11.2026 отправка идёт на ядре ESP 0, и
 *   Q2M_LINK — время самой задачи (параллельно кадру), а в кадр входит только LWAIT.
 * Части SCR вне RF (26.11.2026, на плате SCR − RF ~8 мс): Q2M_C_ENTS — CL_AddEntities (сущности, временные
 *   эффекты, частицы, свет — для сцены), Q2M_C_2D — 2D поверх кадра (HUD, раскладка, консоль, меню),
 *   Q2M_C_END — re.EndFrame (конец кадра: команды — в ядро).
 * Части моделей (26.11.2026, на плате ent ~15 мс): Q2M_A_FRM — R_AliasSetupFrames (кадры md2, чтение SDRAM ядра),
 *   Q2M_A_LIT — R_AliasSetupLighting (R_LightPoint), Q2M_A_XF — R_AliasTransformFinalVerts, Q2M_A_TRS —
 *   треугольники в пачку (BZ_AliasModelTris); Q2M_K_MDL/VERT/TRI — счёт моделей, вершин, треугольников (не такты).
 */
#ifndef Q2M_H
#define Q2M_H

enum { Q2M_SV, Q2M_GAME, Q2M_CL, Q2M_SCR, Q2M_RF, Q2M_SND,
       Q2M_R_SETUP, Q2M_R_WORLD, Q2M_R_BMODEL, Q2M_R_ENT, Q2M_R_PART, Q2M_R_ALPHA, Q2M_LINK, Q2M_LREAD,
       Q2M_F_FACE, Q2M_F_LM, Q2M_F_TEX, Q2M_A_TRI, Q2M_LWAIT,
       Q2M_C_ENTS, Q2M_C_2D, Q2M_C_END,
       Q2M_A_FRM, Q2M_A_LIT, Q2M_A_XF, Q2M_A_TRS, Q2M_K_MDL, Q2M_K_VERT, Q2M_K_TRI,
       Q2M_N };

long long Q2M_Now (void);
extern long long q2m_t0[Q2M_N];
extern long long q2m_acc[Q2M_N];

#define Q2M_BEGIN(i) (q2m_t0[i] = Q2M_Now ())
#define Q2M_END(i)   (q2m_acc[i] += Q2M_Now () - q2m_t0[i])

#endif
