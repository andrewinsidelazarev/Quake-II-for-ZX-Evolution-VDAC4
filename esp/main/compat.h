/*
 * =====================================================================================================
 * compat.h — Quake 2 в модуле ESP VDAC4: чем заменить функции библиотеки C, которых модулю не дают.
 * =====================================================================================================
 *
 * ПОЧЕМУ ТАК
 *   Q2.ELF — не программа со своей libc, а модуль: его грузит прошивка ESP VDAC4 и связывает с собой.
 *   Внешнее имя модуля загрузчик ищет только в двух таблицах прошивки (components/elf_loader —
 *   ESP_ELFSYM_EXPORT, src/zm_symbols.cpp — ZM_SYM). Там есть строки, память, sprintf/vsnprintf,
 *   математика, setjmp/longjmp, qsort, — но нет файлов (fopen, fread…), vsprintf, sscanf, atof,
 *   strtok, strdup, time. Такой вызов модуль не свяжет: на плате — «relocate failed».
 *
 *   Поэтому этот заголовок подставляется в начало каждого файла Quake 2 (ключ компилятора -include):
 *   сначала подключаются настоящие заголовки C (они защищены от повторного подключения, и когда
 *   файл id подключит их снова, ничего не изменится), потом имена недостающих функций заменяются
 *   макросами на свои — q2_* из compat.c. Сами файлы id не правятся.
 *
 *   Память: malloc/free модуля шли бы в общую кучу прошивки мимо учёта ядра. Ядро помнит и при
 *   выгрузке освобождает только то, что выдано через zm_core_t::alloc, — поэтому malloc и
 *   компания тоже идут через него, в PSRAM (8 МБ; внутренней памяти ESP почти нет).
 *
 *   Файлы: q2_fopen и др. — маленькая своя система файлов (compat.c): чтение идёт к «хранилищу pak»
 *   (где оно — решает пользователь: карта Tang, карта Evo или флешка USB), запись — в память.
 *
 * ВАЖНО: <stdbool.h> сюда не подключать. В q_shared.h id объявляет `typedef enum {false, true}
 * qboolean;`, а stdbool.h делает false и true макросами — объявление id тогда не скомпилируется.
 */
#ifndef Q2ESP_COMPAT_H
#define Q2ESP_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>

/* ---------------------------------------------------------------- память (zm_core_t::alloc, PSRAM) */
void *q2_malloc (size_t n);
void *q2_calloc (size_t n, size_t size);
void *q2_realloc (void *p, size_t n);
void  q2_free (void *p);
char *q2_strdup (const char *s);

/* ---------------------------------------------------------------- файлы (compat.c) */
FILE  *q2_fopen (const char *name, const char *mode);
int    q2_fclose (FILE *f);
size_t q2_fread (void *data, size_t size, size_t count, FILE *f);
size_t q2_fwrite (const void *data, size_t size, size_t count, FILE *f);
int    q2_fseek (FILE *f, long offset, int origin);
long   q2_ftell (FILE *f);
int    q2_fprintf (FILE *f, const char *fmt, ...);
int    q2_fflush (FILE *f);
int    q2_remove (const char *name);
int    q2_rename (const char *from, const char *to);

/* ---------------------------------------------------------------- строки и прочее */
int    q2_vsprintf (char *out, const char *fmt, va_list ap);
int    q2_sscanf (const char *s, const char *fmt, ...);
double q2_atof (const char *s);
char  *q2_strtok (char *s, const char *delim);
char  *q2_strlwr (char *s);
int    q2_toupper (int c);
int    q2_tolower (int c);
char  *q2_getenv (const char *name);
int    q2_putenv (char *s);
int    q2_printf (const char *fmt, ...);
time_t q2_time (time_t *t);
struct tm *q2_localtime (const time_t *t);
void   q2_exit (int code) __attribute__((noreturn));

/* ctype: макросы isspace и др. у newlib ESP-IDF 6 смотрят в таблицу _ctype_b, а её прошивка модулю
 * не даёт — свои, только ASCII (Quake 2 другого и не знает) */
static inline int q2_isspace (int c) { return c == ' ' || (c >= 9 && c <= 13); }
static inline int q2_isdigit (int c) { return c >= '0' && c <= '9'; }
static inline int q2_isupper (int c) { return c >= 'A' && c <= 'Z'; }
static inline int q2_islower (int c) { return c >= 'a' && c <= 'z'; }
static inline int q2_isalpha (int c) { return q2_isupper (c) || q2_islower (c); }
static inline int q2_isalnum (int c) { return q2_isalpha (c) || q2_isdigit (c); }
static inline int q2_isxdigit (int c) { return q2_isdigit (c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static inline int q2_isprint (int c) { return c >= 32 && c < 127; }
static inline int q2_iscntrl (int c) { return (c >= 0 && c < 32) || c == 127; }
static inline int q2_ispunct (int c) { return q2_isprint (c) && c != ' ' && !q2_isalnum (c); }
#undef isspace
#undef isdigit
#undef isupper
#undef islower
#undef isalpha
#undef isalnum
#undef isxdigit
#undef isprint
#undef iscntrl
#undef ispunct
#define isspace(_a)  q2_isspace(_a)
#define isdigit(_a)  q2_isdigit(_a)
#define isupper(_a)  q2_isupper(_a)
#define islower(_a)  q2_islower(_a)
#define isalpha(_a)  q2_isalpha(_a)
#define isalnum(_a)  q2_isalnum(_a)
#define isxdigit(_a) q2_isxdigit(_a)
#define isprint(_a)  q2_isprint(_a)
#define iscntrl(_a)  q2_iscntrl(_a)
#define ispunct(_a)  q2_ispunct(_a)

#ifndef Q2ESP_COMPAT_IMPL
/* функциональные макросы: заменяют только вызов «имя(…)», поля структур с тем же именем
 * (cl.time, level.time) не трогают */
#undef toupper
#undef tolower
/* Математика — во float (26.11.2026). Quake 2 зовёт sin, cos, sqrt, floor … с аргументами float, а C отдаёт их
 * функциям double: у ESP32-S3 FPU только float, double считается программно (профиль Unreal NG: __kernel_sin,
 * __muldf3 … из ПЗУ). Версии float прошивка экспортирует (zm_symbols.cpp); fabsf GCC делает одной командой FPU.
 * <math.h> уже подключён выше — его объявления double не задеты. */
#define sin(_a)      sinf(_a)
#define cos(_a)      cosf(_a)
#define tan(_a)      tanf(_a)
#define atan(_a)     atanf(_a)
#define atan2(_a,_b) atan2f(_a,_b)
#define sqrt(_a)     sqrtf(_a)
#define floor(_a)    floorf(_a)
#define ceil(_a)     ceilf(_a)
#define fabs(_a)     fabsf(_a)

#define malloc(_a) q2_malloc(_a)
#define calloc(_a,_b) q2_calloc(_a,_b)
#define realloc(_a,_b) q2_realloc(_a,_b)
#define free(_a)   q2_free(_a)
#define strdup(_a) q2_strdup(_a)
#define fopen(_a,_b) q2_fopen(_a,_b)
#define fclose(_a) q2_fclose(_a)
#define fread(_a,_b,_c,_d) q2_fread(_a,_b,_c,_d)
#define fwrite(_a,_b,_c,_d) q2_fwrite(_a,_b,_c,_d)
#define fseek(_a,_b,_c) q2_fseek(_a,_b,_c)
#define ftell(_a)  q2_ftell(_a)
#define fflush(_a) q2_fflush(_a)
#define remove(_a) q2_remove(_a)
#define rename(_a,_b) q2_rename(_a,_b)
#define vsprintf(_a,_b,_c) q2_vsprintf(_a,_b,_c)
#define atof(_a)   q2_atof(_a)
#define strtok(_a,_b) q2_strtok(_a,_b)
#define strlwr(_a) q2_strlwr(_a)
#define toupper(_a) q2_toupper(_a)
#define tolower(_a) q2_tolower(_a)
#define getenv(_a) q2_getenv(_a)
#define putenv(_a) q2_putenv(_a)
#define time(_a)   q2_time(_a)
#define localtime(_a) q2_localtime(_a)
#define exit(_a)   q2_exit(_a)
#define fprintf(...) q2_fprintf(__VA_ARGS__)
#define sscanf(...) q2_sscanf(__VA_ARGS__)
#define printf(...) q2_printf(__VA_ARGS__)
#endif

#endif
