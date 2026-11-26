/*
 * =====================================================================================================
 * q2hot.c — горячий модуль Q2HOT.ELF: самые частые функции переднего края во внутреннем ОЗУ ESP (26.11.2026).
 * =====================================================================================================
 *
 * ЗАЧЕМ
 *   Пользователь: «Да, переноси». На плате (board_log15, счётчики Xtensa ядра игры) ~22 % тактов ядро ждёт
 *   выборки команд: код Q2.ELF (400 КБ) лежит в PSRAM, кэш команд ESP32-S3 — 16 КБ. Прошивка (ABI 1.19) даёт
 *   приложению 16 КБ исполняемого внутреннего ОЗУ (code_ram) и кладёт туда код модуля, открытого
 *   module_open_ex с ZM_MODULE_TEXT_INTERNAL. Код оттуда выбирается без кэша и без ожидания PSRAM.
 *
 * ЧТО ВНУТРИ
 *   gpu/front/bz_hot.c — грань мира (отсечение, проекция, градиенты), треугольники моделей
 *   пачками, поиск текстуры, частицы (по профилю Unreal NG ~65 % команд кадра). Исходник тот же, что
 *   подключает bz_front.c; здесь перед ним q2hot_remap.h переназначает имена данных и функций Q2.ELF на
 *   таблицу импорта (bz_int.h, q2hot_imp_t): модуль ссылается только на прошивку (memcpy, деление float …).
 *
 * КАК ПОДКЛЮЧАЕТСЯ
 *   Q2.ELF (q2_app.c) при старте просит code_ram и открывает «Q2HOT.ELF»; прошивка зовёт main (3, {имя,
 *   imp, exp}) — здесь это q2hot_entry: сверка версии и размеров типов, копия таблицы импорта, свои функции —
 *   в таблицу exp. Потом Q2.ELF (BZ_HotUse) зовёт функции модуля вместо своих копий. Модуля нет или он не той
 *   версии — игра идёт на своих копиях, как раньше.
 *   На ПК этот файл собирается в q2esp.exe (measure/build.py), и q2esp с Q2M_HOT=1 зовёт q2hot_entry сам —
 *   кадры сверяются с обычной сборкой бит в бит.
 *
 * ВНУТРЕННЕЕ ОЗУ (zm_app.h, code_ram): шина команд читает его только словами по 4 байта — в .text только
 *   код и литералы (L32R); таблиц переходов нет (-fno-jump-tables, как у всех модулей); константы — в
 *   .rodata. Проверяет esp/hot/build_hot.py.
 */
#include "r_local.h"
#include "bz_front.h"
#include "bomzh.h"
#include "bz_int.h"

#include <string.h>

/* функции и данные id, которых нет в r_local.h (их зовёт bz_hot.c; адреса — из таблицы) */
image_t *R_TextureAnimation (mtexinfo_t *tex);
int      D_MipLevelForScale (float scale);
extern vec3_t transformed_modelorg;

static q2hot_imp_t q2h;            /* копия таблицы импорта: данные и функции Q2.ELF */

#include "q2hot_remap.h"
#include "bz_hot.c"

/* Подключение: 0 — таблица exp заполнена; иначе — модуль не той версии (игра идёт без него). */
int q2hot_entry (const q2hot_imp_t *imp, q2hot_exp_t *exp)
{
	static const uint32_t sizes[Q2HOT_NSIZES] = Q2HOT_SIZES;

	if (!imp || !exp)
		return -1;
	if (imp->size != sizeof(q2hot_imp_t) || imp->abi != Q2HOT_ABI || exp->size != 0)
		return -2;
	if (memcmp (imp->sizes, sizes, sizeof(sizes)))
		return -3;                  /* Q2.ELF и Q2HOT.ELF собраны из разных исходников */
	q2h = *imp;
	exp->size = sizeof(*exp);
	exp->abi = Q2HOT_ABI;
	exp->face = bz_face;
	exp->alias_tris = bz_alias_model_tris;
	exp->particle = bz_particle;
	exp->final_verts = hot_final_verts;
	exp->project_vert = hot_project_final_vert;
	return 0;
}

#ifdef Q2HOT_MODULE
/* Точка входа модуля: прошивка зовёт main (3, {имя, arg1, arg2}); сборка переименует её в app_main. */
int main (int argc, char **argv)
{
	if (argc < 3)
		return -1;
	return q2hot_entry ((const q2hot_imp_t *)argv[1], (q2hot_exp_t *)argv[2]);
}
#endif
