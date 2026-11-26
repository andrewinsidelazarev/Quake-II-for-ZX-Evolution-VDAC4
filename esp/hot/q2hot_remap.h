/*
 * q2hot_remap.h — имена данных и функций основного модуля → поля таблицы импорта (26.11.2026).
 *
 * Подключается только в esp/hot/q2hot.c — после всех заголовков (их объявления не должны попасть под эти
 * макросы) и перед gpu/front/bz_hot.c. Так тот же исходник горячих функций, что у bz_front.c, в модуле
 * Q2HOT.ELF обращается к данным и функциям Q2.ELF через таблицу q2h (копия q2hot_imp_t, bz_int.h): ссылок
 * на символы Q2.ELF у модуля нет — загрузчик прошивки связал бы его только со своими.
 *
 * Новое имя в bz_hot.c, которого здесь нет, — ошибка связывания модуля (esp/hot/build_hot.py ищет
 * неопределённые символы и сверяет их с таблицами прошивки).
 */
#ifndef Q2HOT_REMAP_H
#define Q2HOT_REMAP_H

/* данные переднего края (bz_front.c) */
#define cb                    (*q2h.cb)
#define cb_sky                (*q2h.cb_sky)
#define fs                    (*q2h.fs)
#define mapa                  (*q2h.mapa)
#define ab                    (*q2h.ab)
#define texhash               (q2h.texhash)
#define tex_page_frame        (q2h.tex_page_frame)
#define tex_frame             (*q2h.tex_frame)
#define bz_alias_skin         (*q2h.bz_alias_skin)

/* функции переднего края (bz_front.c) */
#define put_full              (q2h.put_full)
#define tex_load              (q2h.tex_load)
#define image_id              (q2h.image_id)
#define lm_build              (q2h.lm_build)
#define lmhash                (q2h.lmhash)
#define lm_total              (*q2h.lm_total)
#define BZ_CalcGradients      (q2h.BZ_CalcGradients)

/* данные рендера id */
#define currentmodel          (*q2h.currentmodel)
#define r_worldmodel          (*q2h.r_worldmodel)
#define r_newrefdef           (*q2h.r_newrefdef)
#define r_framecount          (*q2h.r_framecount)
#define r_pcurrentvertbase    (*q2h.r_pcurrentvertbase)
#define view_clipplanes       (q2h.view_clipplanes)
#define modelorg              (q2h.modelorg)
#define vright                (q2h.vright)
#define vup                   (q2h.vup)
#define vpn                   (q2h.vpn)
#define transformed_modelorg  (q2h.transformed_modelorg)
#define xcenter               (*q2h.xcenter)
#define ycenter               (*q2h.ycenter)
#define xscale                (*q2h.xscale)
#define yscale                (*q2h.yscale)
#define xscaleinv             (*q2h.xscaleinv)
#define yscaleinv             (*q2h.yscaleinv)
#define scale_for_mip         (*q2h.scale_for_mip)
#define r_refdef              (*q2h.r_refdef)
#define d_sdivzorigin         (*q2h.d_sdivzorigin)
#define d_sdivzstepu          (*q2h.d_sdivzstepu)
#define d_sdivzstepv          (*q2h.d_sdivzstepv)
#define d_tdivzorigin         (*q2h.d_tdivzorigin)
#define d_tdivzstepu          (*q2h.d_tdivzstepu)
#define d_tdivzstepv          (*q2h.d_tdivzstepv)
#define sadjust               (*q2h.sadjust)
#define tadjust               (*q2h.tadjust)
#define r_aliasblendcolor     (*q2h.r_aliasblendcolor)
#define r_affinetridesc       (*q2h.r_affinetridesc)
#define currententity         (*q2h.currententity)
#define r_lerp_frontv         (q2h.r_lerp_frontv)
#define r_lerp_backv          (q2h.r_lerp_backv)
#define r_lerp_move           (q2h.r_lerp_move)
#define r_plightvec           (q2h.r_plightvec)
#define r_avertexnormals      (q2h.r_avertexnormals)
#define aliastransform        (q2h.aliastransform)
#define r_ambientlight        (*q2h.r_ambientlight)
#define r_shadelight          (*q2h.r_shadelight)
#define s_ziscale             (*q2h.s_ziscale)
#define aliasxscale           (*q2h.aliasxscale)
#define aliasyscale           (*q2h.aliasyscale)
#define aliasxcenter          (*q2h.aliasxcenter)
#define aliasycenter          (*q2h.aliasycenter)

/* функции рендера id */
#define R_TextureAnimation    (q2h.R_TextureAnimation)
#define D_MipLevelForScale    (q2h.D_MipLevelForScale)
#define R_AliasClipTriangle   (q2h.R_AliasClipTriangle)

#endif
