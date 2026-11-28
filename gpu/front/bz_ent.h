/*
 * bz_ent.h — модели на ядре ESP 0: свои копии общих переменных рендера id (28.11.2026).
 *
 * Непрозрачные модели кадра считает задача LINK на ядре ESP 0, пока игра на ядре 1 обходит мир (bz_front.c,
 * «МОДЕЛИ НА ЯДРЕ 0»). Мир по ходу пишет переменные id, которые читают и модели: currententity, currentmodel,
 * modelorg, r_entorigin, а у дверей и лифтов (R_RotateBmodel) — ещё vpn, vright, vup и view_clipplanes. Консоль
 * (ri.Con_Printf) у сторон общая.
 *
 * Этот заголовок — ПЕРВАЯ строка файлов моделей (r_alias.c, r_sprite.c, r_poly.c, r_light.c и r_ent.c — их
 * делает measure/patches/p06_ref_esp.py): до r_local.h, так что в файле переименовано всё разом — и объявления
 * r_local.h, и поля полей с теми же именами (polydesc_t.vpn …: раскладка та же, имена в одном файле — одни).
 * Копии определены в r_ent.c; копии вида каждый кадр кладёт BZ_EntBegin, e_ri — BZ_Init/BZ_EntBegin
 * (Con_Printf — BZ_EntPrintf). TransformVector читает vright, vup, vpn, а AngleVectors id держит синусы в static — у моделей свои (r_ent.c).
 */
#define currententity    e_currententity
#define currentmodel     e_currentmodel
#define modelorg         e_modelorg
#define r_entorigin      e_r_entorigin
#define vpn              e_vpn
#define vright           e_vright
#define vup              e_vup
#define view_clipplanes  e_view_clipplanes
#define TransformVector  E_TransformVector
#define ri               e_ri
#define AngleVectors     E_AngleVectors
