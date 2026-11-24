/*
 * =====================================================================================================
 * bz_front.h — врезки переднего края «бомж-3D» в программный рендер Quake 2 (ref_bz).
 * =====================================================================================================
 *
 * ЧТО ТАКОЕ ВРЕЗКА
 *   ref_bz — копия программного рендера id (ref_soft). В нужные места его кода вставлены вызовы наших
 *   функций BZ_* (measure/patches/p04_ref_bz.py делает это при сборке). Так код id обходит карту,
 *   отсекает невидимое и считает свет — а рисует не он, а наш передний край (bz_front.c).
 *
 * КАК ИДЁТ КАДР
 *   1) Первый проход — обычный программный рендер id: эталонный кадр.
 *   2) Второй проход (bz_pass = 1): те же функции, но врезки отдают многоугольники сюда.
 *   3) BZ_EndFrame: поток команд выполняет C-модель, её кадр сравнивается с эталоном точка в точку.
 *   2D (строка состояния, консоль, меню) рисуется один раз: врезка просто дублирует его командой.
 *
 * То, что делает bz_front.c с плавающей точкой, на VDAC4 будет делать ESP; поэтому этот файл — прообраз
 * кода ESP, а счётчики потока (слова команд, байты загрузок) — оценка нагрузки LINK.
 */
#ifndef BZ_FRONT_H
#define BZ_FRONT_H

#include <stdint.h>

struct msurface_s;
struct image_s;

extern int   bz_pass;            /* 1 — идёт второй проход: врезки шлют команды вместо рисования */
extern float bz_poly_alpha;      /* последний вызов R_ClipAndDrawPoly: прозрачность...           */
extern int   bz_poly_textured;   /* ...и есть ли текстура (луч — сплошной цвет)                  */
extern int   bz_raw;             /* 1 — Draw_StretchPicImplementation зовётся из Draw_StretchRaw */
extern struct image_s *bz_alias_skin;   /* шкурка модели, что рисуется (R_AliasSetupSkin)          */
extern struct image_s *bz_poly_image;   /* картинка спрайта для BZ_DrawPoly (R_DrawSprite)         */

/* BZ_TEX_RESIDENT («диета», шаг 3): картинка загружена (R_FindImage) — её текстуры сразу в SDRAM ядра,
 * точки на ESP отдаются; понадобятся ещё раз — R_EspReloadPixels перечитает их из файла. */
void BZ_ImageLoaded (struct image_s *image);
unsigned char *R_EspReloadPixels (struct image_s *image, int mip);   /* r_image.c (p04) */

/* Замер на ПК (24.11, перед переездом кадров md2 в SDRAM ядра): какой кадр какой модели берёт рендер —
 * в <q2m_out>.md2trace при bz_md2trace 1 (зовёт R_AliasSetupFrames, p06). На ESP — пусто. */
void BZ_Md2Access (const char *model, int frame, int framesize);

/* Хранилище в SDRAM ядра (SPEC 0.4, bz_front.c): кадры md2 лежат там, на ESP — кольцо недавних кадров.
 * uint32_t, а не unsigned int: у Xtensa (ESP-IDF 5+) uint32_t — это unsigned long. */
int         BZ_StoreAvailable (void);
uint32_t    BZ_StoreAlloc (uint32_t bytes);                     /* адрес (кратен 32) или 0 — нет места */
void        BZ_StoreFree (uint32_t addr, uint32_t bytes);
void        BZ_StoreWrite (uint32_t addr, const void *data, uint32_t bytes);
void        BZ_StoreMark (void);                                /* после загрузки модели */
uint32_t    BZ_StoreBytes (void);
uint32_t    BZ_CmdBufBytes (void);                              /* байт в буферах команд кадра и неба */
const void *BZ_Md2Frame (uint32_t id, int frame, uint32_t addr, int size);

int  BZ_Enabled (void);
void BZ_Init (void);
void BZ_SetMode (int width, int height);
void BZ_Shutdown (void);
void BZ_SaveSoft (void);         /* запомнить кадр эталона перед вторым проходом */
void BZ_RestoreSoft (void);      /* вернуть его: второй проход не должен портить эталон */
void BZ_EndFrame (const unsigned char *palette_rgba);

/* 3D (второй проход) */
void BZ_Begin3D (void);
void BZ_End3D (void);             /* после второго прохода: ножницы снова на весь экран */
void BZ_EndWorld (void);
void BZ_RenderFace (struct msurface_s *fa, int clipflags);
void BZ_AlphaSurfaces (void);
void BZ_AliasModelBegin (void *pfinalverts, int num);   /* модель: вершины уже в экране */
void BZ_AliasModelEnd (void);                           /* модель кончилась: отправить пачку */
void BZ_AliasTriangle (int kind);   /* 0 — непрозрачно, 1/2 — 33/66 %, 3/4 — оболочка 33/66 % */
void BZ_Particle (int u, int v, float zi, int pix, int color, int level);
void BZ_DrawPoly (int iswater, int blendcolor);
void BZ_CalcGradients (struct msurface_s *pface, int mip);   /* в r_edge.c: miplevel там static */

/* 2D (оба прохода не нужны: 2D рисуется один раз, врезка просто дублирует его командой) */
void BZ_DrawChar (int x, int y, int num, struct image_s *chars);
void BZ_StretchPic (int x, int y, int w, int h, struct image_s *pic);
void BZ_DrawPic (int x, int y, struct image_s *pic);
void BZ_TileClear (int x, int y, int w, int h, struct image_s *pic);
void BZ_Fill (int x, int y, int w, int h, int c);
void BZ_FadeScreen (void);

#endif
