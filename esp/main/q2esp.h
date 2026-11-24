/*
 * =====================================================================================================
 * q2esp.h — общее для файлов платформы Q2.ELF (Quake 2 на ESP32-S3 платы VDAC4).
 * =====================================================================================================
 *
 * КАК УСТРОЕН Q2.ELF
 *   Прошивка ESP VDAC4 («ядро ESP») грузит модуль Q2.ELF и зовёт его main: модуль отдаёт свою таблицу
 *   zm_app_t (q2_app.c) и получает таблицу функций ядра zm_core_t — через неё всё, что модулю нужно
 *   от платы: память, время, задачи, LINK к Tang, звук. Игра идёт в своей задаче (q2_app.c): там
 *   Qcommon_Init и цикл Qcommon_Frame, как в main() у id.
 *
 *   Файлы платформы (вместо win32/ у id):
 *     q2_app.c     — вход модуля, задача игры, выход по ошибке (longjmp);
 *     sys_esp.c    — Sys_*: время, ошибки, память Hunk, поиск файлов;
 *     vid_esp.c    — «видео»: рендер (ref_esp) собран в этот же модуль, окна нет;
 *     swimp_esp.c  — то, что у ref_soft делает окно: режим экрана, конец кадра;
 *     bz_link.c    — поток команд «бомж-3D» в очередь ядра Tang по LINK;
 *     snddma_esp.c — звук (пока нет: S_Init увидит отказ и пойдёт без звука);
 *     net_esp.c    — сеть: только петля клиент ↔ сервер внутри модуля;
 *     store_esp.c  — хранилище pak: файлы данных приложения с карты Evo в кэше на флешке USB
 *                    (data_open/data_read/data_close, папка /SYSTEM/CACHE/Q2), без флешки — прямо с
 *                    карты Evo по запросу у программы Z80 (прошивка 0.1.166);
 *     compat.c     — замена недостающей libc (compat.h).
 */
#ifndef Q2ESP_H
#define Q2ESP_H

#include <stdint.h>
#include "zm_app.h"

extern const zm_core_t *q2_core;     /* таблица ядра ESP — выдана в main */

/* выход из игры: из любой глубины назад в задачу игры (q2_app.c), code — для журнала */
void q2_esp_exit (int code) __attribute__((noreturn));
/* просьба выгрузить приложение (stop): Sys_SendKeyEvents проверяет и выходит */
extern volatile int q2_stop_request;

/* ---------------------------------------------------------------- хранилище файлов игры */
/* Открыть файл хранилища по пути Quake 2 («./baseq2/pak0.pak»): дескриптор или NULL, *size — длина.
 * Читать — q2_store_read (сколько прочитано), закрыть — q2_store_close. */
void *q2_store_open (const char *path, uint32_t *size);
uint32_t q2_store_read (void *h, uint32_t offset, void *data, uint32_t count);
void q2_store_close (void *h);

/* Отчёт о памяти PSRAM в журнал (sys_esp.c): свободно всего и самым большим куском, зона Z_Malloc и hunk.
 * Берёт и отдаёт всю свободную память — звать только там, где другие задачи приложения её не ждут. */
void q2_mem_report (const char *why);
void q2_map_report (void);         /* sys_esp.c: строка о памяти после загрузки карты (R_EndRegistration) */
void q2_level_init (void);         /* sys_esp.c: область уровня — после Qcommon_Init, до первой карты */
void *q2_level_scratch (int size); /* sys_esp.c: временный буфер сверху области уровня (или NULL) */
int  q2_level_scratch_free (void *p);   /* 1 — буфер был из области и отдан */

/* Рендер (ref_esp, p06): освободить одну модель прошлой карты — самую большую из тех, что после
 * R_BeginRegistration ещё не зарегистрированы заново. 1 — освободил, 0 — нечего (вне загрузки карты
 * таких нет). Зовут выделители памяти (compat.c, hunk в sys_esp.c), когда куче не хватило. */
int R_EspPurgeStale (void);
void R_EspPurgeStats (int *count, unsigned *bytes);
uint32_t BZ_StoreBytes (void);   /* bz_front.c: сколько байт хранилища в SDRAM ядра занято (кадры md2) */
uint32_t BZ_CmdBufBytes (void);  /* bz_front.c: сколько PSRAM держат буферы команд кадра и неба */

/* ---------------------------------------------------------------- файлы в памяти (compat.c) */
/* Перебор файлов в памяти по образцу «save/current/» + «*» + «.sav» (для Sys_FindFirst): *iter — с 0;
 * возвращает имя или NULL. */
const char *q2_mem_find (const char *pattern, int *iter);

/* ---------------------------------------------------------------- LINK (bz_link.c) */
/* Ядро «бомж-3D» на Tang: загрузить и проверить. ZM_OK — ядро на LINK, его VERSION — *version. */
int  bz_link_open (uint32_t *version);
/* Слова команд — в очередь ядра (ждёт места в очереди). ZM_OK или ZM_ERROR (LINK не работает). */
int  bz_link_send (const uint32_t *words, uint32_t count);
void bz_link_close (void);
/* SPEC 0.4: чтение SDRAM ядра (ядро 0.02+) — кадры md2 живут там (bz_front.c, хранилище) */
int  bz_link_can_read (void);
int  bz_link_wait_mark (uint32_t mark);
int  bz_link_read (uint32_t addr, void *dst, uint32_t bytes);

#endif
