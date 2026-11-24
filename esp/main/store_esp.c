/*
 * =====================================================================================================
 * store_esp.c — хранилище файлов игры (pak0.pak и др.) для Q2.ELF.
 * =====================================================================================================
 *
 * Quake 2 читает свои данные из pak-файлов: pak0.pak полной игры — 180 МБ, демо — 50 МБ. В PSRAM ESP
 * (8 МБ) такое не влезает, поэтому pak читается кусками по мере нужды (уровень берёт из него ~5–10 МБ).
 *
 * ОТКУДА (решение пользователя 23.11)
 *   pak лежит на SD-карте Evo в папке игры, рядом с программой Z80 (SPG). Возит его общий механизм
 *   приложений VDAC4, как ELF и ядро ПЛИС (прошивка ESP 0.1.165, ABI 1.20): программа Z80 один раз шлёт
 *   файл в ESP (MOD_QUERY с флагом 4), ESP кладёт его на флешку USB в /SYSTEM/CACHE/Q2/PAK0.PAK и потом,
 *   если файл на карте не менялся, больше не просит. Читает его Q2.ELF функциями ядра ESP data_open,
 *   data_read, data_close — папка Q2 берётся из имени ELF (Q2.ELF), поэтому здесь нужно только имя файла.
 *
 * БЕЗ ФЛЕШКИ (прошивка 0.1.166; пользователь 23.11: «Игра должна работать и без флешки otg esp32»)
 *   Флешка — только кэш: без неё pak остаётся на карте Evo, а ESP просит его куски у программы Z80
 *   (ESP App SPG), пока игра идёт: та читает секторы карты и отдаёт их по DMA кусками до 8 КБ. Для
 *   Q2.ELF ничего не меняется — те же data_open, data_read, data_close; только чтение медленнее
 *   (~100 КБ/с против ~1 МБ/с с флешки), то есть дольше загрузка уровня.
 *
 * ИМЕНА
 *   Quake 2 открывает «basedir/baseq2/pak0.pak» (q2_fopen в compat.c, basedir — «q2»). В кэше лежат
 *   файлы под именами с карты Evo (8.3, заглавными): берётся последняя часть пути — «pak0.pak»; регистр
 *   ядру ESP не важен. Файлов, которых в кэше нет (config.cfg, лишние pak1…pak9), data_open не находит —
 *   Quake 2 так и задуман: ищет pak0…pak9 и берёт найденные.
 */
#include "q2esp.h"

#include <stddef.h>
#include <string.h>

/* Есть ли у прошивки функции ABI 1.20 (data_open, data_read, data_close). */
static int store_ready (void)
{
	return q2_core->size >= offsetof (zm_core_t, data_close) + sizeof(q2_core->data_close);
}

void *q2_store_open (const char *path, uint32_t *size)
{
	const char *name = path;
	const char *p;
	void       *file = NULL;
	int         r;

	*size = 0;
	if (!store_ready ())
		return NULL;
	for (p = path; *p; p++)
		if (*p == '/' || *p == '\\')
			name = p + 1;
	r = q2_core->data_open (name, &file, size);
	if (r != ZM_OK)
	{
		*size = 0;
		return NULL;
	}
	return file;
}

uint32_t q2_store_read (void *h, uint32_t offset, void *data, uint32_t count)
{
	uint32_t got = 0;

	if (!h || q2_core->data_read (h, offset, data, count, &got) != ZM_OK)
		return got;             /* флешку вынули: сколько успели — Quake 2 сам скажет «короткое чтение» */
	return got;
}

void q2_store_close (void *h)
{
	if (h)
		q2_core->data_close (h);
}
