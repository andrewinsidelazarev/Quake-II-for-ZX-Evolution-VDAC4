"""ref_esp: рендер для ESP — ref_bz (p04_ref_bz.py) без программного рисования id.

ЗАЧЕМ
  ref_bz рисует каждый кадр дважды: программным рендером id (эталон для сравнения) и вторым проходом,
  в котором те же функции id отдают многоугольники переднему краю «бомж-3D» (gpu/front/bz_front.c).
  На ESP эталон не нужен: картинку рисует ПЛИС, ESP только готовит команды. ref_esp — это второй
  проход в чистом виде:
    - кадр — один проход с bz_pass = 1 (без R_RenderFrame эталона, без сохранения его кадра);
    - 2D (буквы, картинки, заливки, затемнение) — только командой адаптера, в память кадра id не пишется;
    - подводная волна D_WarpScreen не зовётся: она переставляет точки готового кадра id, а его нет.
  Собирается с ключом BZ_ONLY (bz_front.c тогда не сравнивает кадр модели с кадром id).

«ДИЕТА» ПАМЯТИ (23.11, для ESP — PSRAM 8 МБ на всё)
  - r_leaftovis/r_vistoleaf (2 × 256 КБ) убраны всегда: id их заполняет, но нигде не читает (остаток
    Quake 1); компилятор подтвердит — обращение к ним не соберётся;
  - с ключом BZ_ESP (сборка Q2.ELF) рендер не заводит z-буфер (W×H×2) и кэш поверхностей (~880 КБ на
    424×240 — в нём id собирает текстуру со светом для своего растеризатора) и не чистит их: z и свет
    у ядра Tang. Что ничего в них не пишется, проверяет q2esp.exe на ПК (swimp_bz.c, BZ_ONLY): буферы
    заполнены образцом и сверяются после каждого кадра;
  - с BZ_ESP R_CinematicSetPalette не чистит кадр id (кадра нет) — только отдаёт кадр адаптеру.
  - 24.11: у модели мира нет полей растеризатора id — msurface_t.cachespots (16 байт на грань) и
    medge_t.cachededgeoffset (ребро 4 байта вместо 8); код, который их трогал (R_EmitCachedEdge,
    программная часть R_RenderFace, D_CacheSurface), в ref_esp не работает и выключен (#if 0), а
    R_RenderFace и D_CacheSurface при вызове падают с ошибкой — молча испортить память не могут.
  - 24.11: смена карты — модели прошлой отдаются при нехватке памяти (R_EspPurgeStale; метка — R_EspLevelEnd
    из CM_LoadMap; до новой регистрации без 3D); md2 — начало файла на месте, кадры группами до 64 КБ
    (md2_load, model_t.md2_frames), без команд GL; плоскости и видимость — общие с моделью столкновений
    (CM_SharedPlanes/Visibility). Проверка на ПК: r_purge_test 1 и mcrc кадра модели против ref_bz.

make(src, out): копия src (port/ref_soft) в out (build/port/gen/ref_esp) с врезками p04 и правками ниже.
Исходники port/ref_soft не меняются. Концы строк каждого файла сохраняются (правка байтами, как в p04).
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import p04_ref_bz  # noqa: E402

# Правки поверх врезок p04: (старый текст, новый текст) — каждый старый текст встречается ровно раз.
ESP = {
    'r_main.c': [
        # кадр — один проход с врезками (у ref_bz: эталон id, затем второй проход)
        ('void R_RenderFrame (refdef_t *fd)\n{\n'
         '\tR_RenderFrame1 (fd);\n'
         '\tif (!BZ_Enabled ())\n\t\treturn;\n'
         '\tBZ_SaveSoft ();\n'
         '\tbz_pass = 1;\n'
         '\tR_RenderFrame1 (fd);\n'
         '\tbz_pass = 0;\n'
         '\tBZ_End3D ();\n'
         '\tBZ_RestoreSoft ();\n}\n',
         'void R_RenderFrame (refdef_t *fd)\n{\n'
         '\t// ref_esp: один проход — функции id обходят карту, врезки шлют многоугольники адаптеру\n'
         '\tbz_pass = 1;\n'
         '\tR_RenderFrame1 (fd);\n'
         '\tbz_pass = 0;\n'
         '\tBZ_End3D ();\n}\n'),
        # массивы рёбер и поверхностей id: в режиме команд рёбра не порождаются (R_RenderFace уходит в
        # BZ_RenderFace раньше) — на ESP они крошечные (было 128 КБ стека); что рёбер правда нет,
        # проверяется после обхода мира (ниже), на ПК — во всём демо
        ('\tedge_t\tledges[NUMSTACKEDGES +\n\t\t\t\t((CACHE_SIZE - 1) / sizeof(edge_t)) + 1];\n'
         '\tsurf_t\tlsurfs[NUMSTACKSURFACES +\n\t\t\t\t((CACHE_SIZE - 1) / sizeof(surf_t)) + 1];\n',
         '#ifdef BZ_ESP\t// ref_esp: рёбер и поверхностей id нет — только заглушки (R_BeginEdgeFrame пишет surfaces[1])\n'
         '\tedge_t\tledges[4 + ((CACHE_SIZE - 1) / sizeof(edge_t)) + 1];\n'
         '\tsurf_t\tlsurfs[4 + ((CACHE_SIZE - 1) / sizeof(surf_t)) + 1];\n#else\n'
         '\tedge_t\tledges[NUMSTACKEDGES +\n\t\t\t\t((CACHE_SIZE - 1) / sizeof(edge_t)) + 1];\n'
         '\tsurf_t\tlsurfs[NUMSTACKSURFACES +\n\t\t\t\t((CACHE_SIZE - 1) / sizeof(surf_t)) + 1];\n#endif\n'),
        ('\tif (bz_pass)\n\t\tBZ_EndWorld ();\t\t// ref_bz: рёбер нет, мир уже ушёл командами; теперь небо\n',
         '\tif (edge_p != r_edges || surface_p != &surfaces[2])\t// ref_esp: id не должен был породить ни ребра\n'
         '\t\tri.Sys_Error (ERR_FATAL, "ref_esp: edges %d, surfaces %d emitted",\n'
         '\t\t              (int)(edge_p - r_edges), (int)(surface_p - &surfaces[2]));\n'
         '\tif (bz_pass)\n\t\tBZ_EndWorld ();\t\t// ref_bz: рёбер нет, мир уже ушёл командами; теперь небо\n'),
        # подводная волна переставляет точки кадра id — кадра нет
        ('\tif (r_dowarp)\n\t\tD_WarpScreen ();\n',
         '\t// ref_esp: D_WarpScreen не нужен — кадр рисует адаптер, кадра id нет\n'),
        # z-буфер id: на ESP не заводится (z — в SDRAM ядра Tang)
        ('\td_pzbuffer = malloc(vid.width*vid.height*2);\n',
         '#ifdef BZ_ESP\n\td_pzbuffer = NULL;\t\t// ref_esp: z-буфер у ядра Tang\n#else\n'
         '\td_pzbuffer = malloc(vid.width*vid.height*2);\n'
         '\tmemset (d_pzbuffer, 0xA5, vid.width*vid.height*2);\t// ref_esp на ПК: образец — его проверяет swimp_bz.c\n'
         '#endif\n'),
        # кинематика: «очистить экран» — у ESP кадра id нет
        ('\t// clear screen to black to avoid any palette flash\n'
         '\tw = abs(vid.rowbytes)>>2;\t// stupid negative pitch win32 stuff...\n'
         '\tfor (i=0 ; i<vid.height ; i++, d+=w)\n\t{\n'
         '\t\td = (int *)(vid.buffer + i*vid.rowbytes);\n\t\tfor (j=0 ; j<w ; j++)\n\t\t\td[j] = 0;\n\t}\n',
         '\t// clear screen to black to avoid any palette flash\n#ifndef BZ_ESP\t// ref_esp: кадра id нет\n'
         '\tw = abs(vid.rowbytes)>>2;\t// stupid negative pitch win32 stuff...\n'
         '\tfor (i=0 ; i<vid.height ; i++, d+=w)\n\t{\n'
         '\t\td = (int *)(vid.buffer + i*vid.rowbytes);\n\t\tfor (j=0 ; j<w ; j++)\n\t\t\td[j] = 0;\n\t}\n#endif\n'),
        # смена карты: движок (CM_LoadMap) скажет рендеру, что прошлая карта больше не нужна (p06, r_model.c)
        ('\tMod_Init ();\n',
         '\tMod_Init ();\n'
         '\tCM_NewMapHook = R_EspLevelEnd;\t// ref_esp: модели прошлой карты — старые с начала загрузки новой\n'),
        ('\t// ref_esp: один проход — функции id обходят карту, врезки шлют многоугольники адаптеру\n',
         '\tif (R_EspFrameBlocked ())\t// ref_esp: модели прошлой карты уже отданы — до регистрации без 3D\n'
         '\t\treturn;\n'
         '\t// ref_esp: один проход — функции id обходят карту, врезки шлют многоугольники адаптеру\n'),
        ('qboolean R_Init( void *hInstance, void *wndProc )\n{\n',
         'void\tR_EspLevelEnd (void);\t\t// ref_esp, r_model.c\n'
         'int\tR_EspFrameBlocked (void);\n'
         '\n'
         'qboolean R_Init( void *hInstance, void *wndProc )\n{\n'),
    ],
    'r_polyse.c': [
        # отрезки треугольника id (38 КБ стека) — в режиме команд не нужны: врезка выходит раньше
        ('void R_DrawTriangle( void )\n{\n\tspanpackage_t spans[DPS_MAXSPANS];\n',
         'void R_DrawTriangle( void )\n{\n#ifdef BZ_ESP\n\tspanpackage_t spans[1];\t// ref_esp: врезка ниже выходит раньше\n'
         '#else\n\tspanpackage_t spans[DPS_MAXSPANS];\n#endif\n'),
    ],
    'r_image.c': [
        # отчёт о памяти ESP (sys_esp.c q2_mem_report): пиксели всех картинок рендера
        ('image_t\t\tr_images[MAX_RIMAGES];\nint\t\t\tnumr_images;\n',
         'image_t\t\tr_images[MAX_RIMAGES];\nint\t\t\tnumr_images;\n'
         '\n'
         '// ref_esp: сколько байт держат пиксели картинок (все мип-уровни) и сколько картинок — для отчёта о\n'
         '// памяти ESP (sys_esp.c). Картинка без registration_sequence — свободное место таблицы\n'
         'void R_EspImageBytes (unsigned *bytes, int *count)\n'
         '{\n'
         '\tint\t\ti, m, n = 0;\n'
         '\tunsigned\ttotal = 0;\n'
         '\n'
         '\tfor (i = 0 ; i < numr_images ; i++)\n'
         '\t{\n'
         '\t\tif (!r_images[i].registration_sequence)\n'
         '\t\t\tcontinue;\n'
         '\t\tn++;\n'
         '\t\tfor (m = 0 ; m < 4 ; m++)\n'
         '\t\t\tif (r_images[i].pixels[m])\n'
         '\t\t\t\ttotal += (unsigned)((r_images[i].width >> m) * (r_images[i].height >> m));\n'
         '\t}\n'
         '\t*bytes = total;\n'
         '\t*count = n;\n'
         '}\n'),
        # «диета», шаг 2: пик по всем 47 картам — 443 картинки (замер q2esp mapstats, 23.11)
        ('#define\tMAX_RIMAGES\t1024\n',
         '#define\tMAX_RIMAGES\t640\t// ref_esp: пик по 47 картам — 443 (у id 1024; −38 КБ)\n'),
    ],
    'r_model.c': [
        # хранилище в SDRAM ядра (SPEC 0.4): кадры md2 (md2_store) и освобождение моделей
        ('#include "r_local.h"\n',
         '#include "r_local.h"\n#include "bz_front.h"\t// ref_esp: хранилище в SDRAM ядра (кадры md2)\n'),
        ('void Mod_Free (model_t *mod)\n{\n\tHunk_Free (mod->extradata);\n',
         'void Mod_Free (model_t *mod)\n{\n'
         '\tMod_EspStoreFree (mod);\t\t// ref_esp: куски хранилища SDRAM (кадры md2) — назад\n'
         '\tHunk_Free (mod->extradata);\n'),
        ("\t\t{\t// don't need this model\n\t\t\tHunk_Free (mod->extradata);\n",
         "\t\t{\t// don't need this model\n"
         '\t\t\tMod_EspStoreFree (mod);\t// ref_esp: куски хранилища SDRAM (кадры md2) — назад\n'
         '\t\t\tHunk_Free (mod->extradata);\n'),
        # отчёт о памяти ESP (sys_esp.c q2_mem_report): hunk всех загруженных моделей
        ('void Mod_Init (void)\n{\n',
         '// ref_esp: сколько байт hunk держат загруженные модели и сколько их — для отчёта о памяти ESP\n'
         'void R_EspModelBytes (unsigned *bytes, int *count)\n'
         '{\n'
         '\tint\t\ti, n = 0;\n'
         '\tunsigned\ttotal = 0;\n'
         '\n'
         '\tfor (i = 0 ; i < mod_numknown ; i++)\n'
         '\t\tif (mod_known[i].name[0])\n'
         '\t\t{\n'
         '\t\t\ttotal += (unsigned)mod_known[i].extradatasize;\n'
         '\t\t\tn++;\n'
         '\t\t}\n'
         '\t*bytes = total;\n'
         '\t*count = n;\n'
         '}\n'
         '\n'
         'void Mod_Init (void)\n{\n'),
        # Unreal NG 24.11: демо 2 встало на переходе к новой карте — модели прошлой (44 md2, ~3,4 МБ) у id
        # живут до R_EndRegistration (общие — оружие, ошмётки — тогда не перечитываются), а новая карта уже
        # грузится рядом. ESP при нехватке памяти зовёт R_EspPurgeStale (compat.c, sys_esp.c): та сбрасывает
        # самую большую модель прошлой регистрации, и выделение повторяется. Пока памяти хватает — как у id.
        ('// ref_esp: сколько байт hunk держат загруженные модели',
         '// ref_esp: сброс моделей прошлой карты при нехватке памяти (R_EspPurgeStale, ниже)\n'
         'int\t\tR_EspPurgeStale (void);\n'
         'static int\tpurge_count;\t\t// сколько моделей сброшено с начала игры\n'
         'static unsigned\tpurge_bytes;\t\t// и сколько байт hunk они держали\n'
         'static int\tpurge_test;\t\t// r_purge_test 1 (проверка на ПК): при каждой загрузке новой модели —\n'
         '\t\t\t\t\t// сбросить все старые, как при самой острой нехватке памяти\n'
         '\n'
         '// ref_esp: сколько байт hunk держат загруженные модели'),
        ('\t//\n\t// find a free model slot spot\n\t//\n',
         '\t// ref_esp, проверка на ПК: память «кончилась» — старые модели прочь (как R_EspPurgeStale на ESP)\n'
         '\tif (purge_test)\n'
         '\t\twhile (R_EspPurgeStale ())\n'
         '\t\t\t;\n'
         '\n'
         '\t//\n\t// find a free model slot spot\n\t//\n'),
        ('\tregistration_sequence++;\n',
         '\tregistration_sequence++;\n'
         '\tpurge_test = (int)ri.Cvar_Get ("r_purge_test", "0", 0)->value;\t// ref_esp: проверка сброса на ПК\n'),
        ('void R_EndRegistration (void)\n{\n',
         '/*\n'
         '================\n'
         'R_EspPurgeStale — ref_esp: освободить одну модель прошлой регистрации (самую большую).\n'
         '\n'
         'Зовут выделители памяти ESP, когда куче не хватило: между R_BeginRegistration и R_EndRegistration\n'
         'модели прошлой карты, ещё не зарегистрированные заново, никто не рисует (кадра нет — идёт загрузка),\n'
         'а указатели на них клиент перепишет, когда зарегистрирует модель снова (тогда она перечитается из pak).\n'
         'Не трогаем: свободные места, модели этой регистрации, модель без регистрации (только что занятое\n'
         'Mod_ForName место — оно ещё грузится) и loadmodel. Вне загрузки старых моделей нет — вернёт 0.\n'
         '================\n'
         '*/\n'
         'int R_EspPurgeStale (void)\n'
         '{\n'
         '\tint\t\ti;\n'
         '\tmodel_t\t*mod, *best = NULL;\n'
         '\n'
         '\tfor (i = 0, mod = mod_known ; i < mod_numknown ; i++, mod++)\n'
         '\t\tif (mod->name[0] && mod->registration_sequence\n'
         '\t\t\t&& mod->registration_sequence != registration_sequence && mod != loadmodel\n'
         '\t\t\t&& (!best || mod->extradatasize > best->extradatasize))\n'
         '\t\t\tbest = mod;\n'
         '\tif (!best)\n'
         '\t\treturn 0;\n'
         '\tpurge_count++;\n'
         '\tpurge_bytes += (unsigned)best->extradatasize;\n'
         '\tMod_EspStoreFree (best);\t\t// куски хранилища SDRAM (кадры md2) — назад\n'
         '\tHunk_Free (best->extradata);\t// как в R_EndRegistration\n'
         '\tmemset (best, 0, sizeof(*best));\n'
         '\treturn 1;\n'
         '}\n'
         '\n'
         '// ref_esp: сколько моделей сброшено и сколько байт они держали — для отчёта о памяти ESP\n'
         'void R_EspPurgeStats (int *count, unsigned *bytes)\n'
         '{\n'
         '\t*count = purge_count;\n'
         '\t*bytes = purge_bytes;\n'
         '}\n'
         '\n'
         'void R_EndRegistration (void)\n{\n'),
        ('\tR_FreeUnusedImages ();\n}\n',
         '\tif (purge_test)\t// ref_esp, проверка на ПК: в вывод программы (не в консоль игры — она на экране)\n'
         '\t\tprintf ("ref_esp: r_purge_test: models purged %d (%u KB) so far, md2 frames in core SDRAM %u KB\\n",\n'
         '\t\t\tpurge_count, purge_bytes >> 10, (unsigned)(BZ_StoreBytes () >> 10));\n'
         '\tR_FreeUnusedImages ();\n'
         '#ifdef BZ_ESP\n'
         '\t{\t// ESP: строка о памяти после каждой карты (плата 24.11: base2 упал без памяти — видеть запас)\n'
         '\t\tvoid q2_map_report (void);\t// sys_esp.c\n'
         '\t\tq2_map_report ();\n'
         '\t}\n'
         '#endif\n'
         '}\n'),
        # смена карты: CM_LoadMap (движок) зовёт R_EspLevelEnd раньше, чем грузит столкновения новой карты
        # (сервер — до клиента). С этого места модели прошлой карты — старые: при нехватке памяти их можно
        # отдать. Кадр 3D после такого сброса и до новой регистрации рендер пропускает (R_EspFrameBlocked):
        # обычно его и так нет — экран закрыт заставкой загрузки (SCR_BeginLoadingPlaque в SV_Map).
        ('static int\tpurge_test;\t\t// r_purge_test 1 (проверка на ПК): при каждой загрузке новой модели —\n',
         'static int\tlevel_ended;\t\t// 1 — началась другая карта, регистрации ещё не было\n'
         'static int\tlevel_purge_mark;\t// purge_count в тот момент: сброс после него — кадр 3D нельзя\n'
         'static int\tpurge_test;\t\t// r_purge_test 1 (проверка на ПК): при каждой загрузке новой модели —\n'),
        ('void R_EndRegistration (void)\n{\n',
         '/*\n'
         '================\n'
         'R_EspLevelEnd — ref_esp: началась другая карта (CM_NewMapHook из CM_LoadMap).\n'
         'Все модели становятся «прошлой регистрации» (новая начнётся в R_BeginRegistration): выделители памяти\n'
         'ESP смогут отдать их ещё до загрузки новой карты. В проверке на ПК (r_purge_test) — отдать сразу все.\n'
         '================\n'
         '*/\n'
         'void R_EspLevelEnd (void)\n'
         '{\n'
         '\tregistration_sequence++;\n'
         '\tlevel_ended = 1;\n'
         '\tlevel_purge_mark = purge_count;\n'
         '\tif (purge_test)\n'
         '\t\twhile (R_EspPurgeStale ())\n'
         '\t\t\t;\n'
         '}\n'
         '\n'
         '// ref_esp: 1 — другая карта уже грузится, а её регистрации не было: кадр 3D рисовать нельзя —\n'
         '// модели прошлой карты могли быть отданы, а её плоскости и видимость (общие с CM) уже освобождены\n'
         'int R_EspFrameBlocked (void)\n'
         '{\n'
         '\treturn level_ended;\n'
         '}\n'
         '\n'
         'void R_EndRegistration (void)\n{\n'),
        ('\tpurge_test = (int)ri.Cvar_Get ("r_purge_test", "0", 0)->value;\t// ref_esp: проверка сброса на ПК\n',
         '\tpurge_test = (int)ri.Cvar_Get ("r_purge_test", "0", 0)->value;\t// ref_esp: проверка сброса на ПК\n'
         '\tlevel_ended = 0;\t\t// ref_esp: регистрация новой карты началась\n'),
        # «диета», шаг 2: моделей — пик 69 по 47 картам; модели-кисти карты (*1, *2…) — свой размер:
        # у city3 их 184, а id объявлял их тем же MAX_MOD_KNOWN и границу не проверял вовсе
        ('#define\tMAX_MOD_KNOWN\t256\n',
         '#define\tMAX_MOD_KNOWN\t128\t// ref_esp: пик по 47 картам — 69 (у id 256; −47 КБ)\n'
         '#define\tMAX_MOD_INLINE\t256\t// ref_esp: модели-кисти карты — отдельно (city3 — 184)\n'),
        ('model_t\tmod_inline[MAX_MOD_KNOWN];\n', 'model_t\tmod_inline[MAX_MOD_INLINE];\n'),
        ('\tloadmodel->submodels = out;\n\tloadmodel->numsubmodels = count;\n',
         '\tif (count > MAX_MOD_INLINE)\t// ref_esp: id не проверял — карта с большим числом испортила бы память\n'
         '\t\tri.Sys_Error (ERR_DROP, "%s has %i inline models", loadmodel->name, count);\n'
         '\tloadmodel->submodels = out;\n\tloadmodel->numsubmodels = count;\n'),
        # таблицы номеров листьев — мёртвые (только пишутся): −512 КБ
        ('int\t\tr_leaftovis[MAX_MAP_LEAFS];\nint\t\tr_vistoleaf[MAX_MAP_LEAFS];\n',
         '// ref_esp: r_leaftovis/r_vistoleaf (2 × 256 КБ) убраны — id их только заполнял\n'),
        ('\t\tr_leaftovis[leafnum] = r_numvisleafs;\n\t\tr_vistoleaf[r_numvisleafs] = leafnum;\n', ''),
        # «диета», шаг 3 (Unreal NG 24.11: на загрузке base2 файл карты 2 МБ целиком лежал в зоне рядом со
        # строящейся моделью, и PSRAM кончилась): карта — по lump-ам. Mod_ForName для .bsp читает только
        # заголовок, Mod_LoadBrushModel зовёт загрузчики id через Mod_Lump: lump — из pak в свой буфер, mod_base
        # сдвинут так, что mod_base + l->fileofs — это буфер, буфер сразу прочь. Пик — самый большой lump.
        ('int\t\tmodfilelen;\n',
         'int\t\tmodfilelen;\n'
         '\n'
         '// ref_esp («диета», шаг 3): карта читается по lump-ам. Рендер собран с движком в одну программу\n'
         '// (REF_HARD_LINKED), поэтому файлы — прямо функциями движка (files.c), а не через ri\n'
         'int\t\tFS_FOpenFile (char *filename, FILE **file);\n'
         'void\tFS_FCloseFile (FILE *f);\n'
         'void\t*FS_ReadPart (FILE *f, int base, int offset, int len);\n'
         'void\tFS_FreeFile (void *buffer);\n'
         'static FILE\t*bsp_file;\t// файл карты, пока Mod_LoadBrushModel её разбирает (NULL — не карта)\n'
         'static int\tbsp_base;\t// его место в pak\n'
         'static byte\t*bsp_scratch;\t// буфер lump-ов карты, один на все (Mod_LumpScratch; NULL — нет)\n'
         'static int\tbsp_scratch_size;\n'
         'static int\tbsp_read;\t// сколько байт карты прочитано — для полосы загрузки (ri.LoadingProgress)\n'
         'static int\tbsp_total;\t// сколько всего прочитают lump-ы рендера (Mod_LumpScratch; 0 — длина файла)\n'
         '\n'
         '// ref_esp (26.11.2026): полоса загрузки под LOADING — ход чтения файла карты\n'
         'static void Mod_BspProgress (int len)\n'
         '{\n'
         '\tbsp_read += len;\n'
         '\tif (ri.LoadingProgress)\n'
         '\t\tri.LoadingProgress (bsp_read, bsp_total > 0 ? bsp_total : modfilelen);\n'
         '}\n'),
        ('\tmodfilelen = ri.FS_LoadFile (mod->name, (void **)&buf);\n',
         '\t// ref_esp: у карты (.bsp) — только заголовок; lump-ы читает Mod_Lump по одному\n'
         '\tif (strlen (mod->name) > 4 && !Q_stricmp (mod->name + strlen (mod->name) - 4, ".bsp"))\n'
         '\t{\n'
         '\t\tbuf = NULL;\n'
         '\t\tmodfilelen = FS_FOpenFile (mod->name, &bsp_file);\n'
         '\t\tbsp_read = bsp_total = 0;\n'
         '\t\tif (bsp_file)\n'
         '\t\t{\n'
         '\t\t\tbsp_base = ftell (bsp_file);\n'
         '\t\t\tbuf = FS_ReadPart (bsp_file, bsp_base, 0, sizeof(dheader_t));\n'
         '\t\t}\n'
         '\t}\n'
         '\telse\n'
         '\t\tmodfilelen = ri.FS_LoadFile (mod->name, (void **)&buf);\n'),
        ('\tri.FS_FreeFile (buf);\n\n\treturn mod;\n',
         '\tri.FS_FreeFile (buf);\n'
         '\tif (bsp_file)\t\t// ref_esp: файл карты разобран\n'
         '\t{\n'
         '\t\tFS_FCloseFile (bsp_file);\n'
         '\t\tbsp_file = NULL;\n'
         '\t}\n'
         '\tMod_TempFree (bsp_scratch);\t// ref_esp: буфер lump-ов карты — назад (в область уровня или кучу)\n'
         '\tbsp_scratch = NULL;\n\n\treturn mod;\n'),
        ('void Mod_LoadBrushModel (model_t *mod, void *buffer)\n{\n',
         '/*\n'
         'Буфер lump-ов карты (ref_esp, 24.11.2026). Плата, а за ней Unreal NG с PSRAM как на плате: у каждого\n'
         'lump-а был свой временный буфер в куче, а между ними hunk брал куски под разобранную карту, и картинки —\n'
         'свои временные буферы. Буферы уходили, на их местах оставались дыры, и они дробились: на base2 свободно\n'
         '1200 КБ, но самый большой кусок — 368 КБ, и таблица граней (474 КБ) не легла. Хуже всех — свет\n'
         '(lighting): base2 — 672 КБ, space — 1454 КБ временного буфера, хотя в hunk свет втрое меньше.\n'
         'Теперь буфер один на всю карту: берётся до первого lump-а, размером с самый большой читаемый lump\n'
         '(кроме света — он читается через тот же буфер частями, Mod_LumpLighting), и уходит, когда карта\n'
         'разобрана (Mod_ForName). Временных дыр между кусками hunk больше нет.\n'
         '*/\n'
         '#define\tBSP_SCRATCH_MIN\t(48 * 1024)\t// не меньше: свет читается частями этого размера (кратен 3)\n'
         '\n'
         '// ref_esp: буфер под самый большой lump карты из тех, что разбирает рендер (плоскости и видимость —\n'
         '// общие с CM и не читаются; если общих нет — их lump, больший буфера, читается как раньше)\n'
         'static void Mod_LumpScratch (dheader_t *header)\n'
         '{\n'
         '\tstatic const int\tread[] = { LUMP_VERTEXES, LUMP_EDGES, LUMP_SURFEDGES, LUMP_TEXINFO, LUMP_FACES,\n'
         '\t\tLUMP_LEAFFACES, LUMP_LEAFS, LUMP_NODES, LUMP_MODELS };\n'
         '\tint\ti, size = BSP_SCRATCH_MIN;\n'
         '\n'
         '\tMod_TempFree (bsp_scratch);\t// прошлая карта могла оборваться ошибкой посреди загрузки\n'
         '\tbsp_scratch = NULL;\n'
         '\tif (!bsp_file)\n'
         '\t\treturn;\n'
         '\tbsp_total = header->lumps[LUMP_LIGHTING].filelen;\t// полоса загрузки: свет и lump-ы ниже\n'
         '\tfor (i = 0 ; i < (int)(sizeof(read) / sizeof(read[0])) ; i++)\n'
         '\t{\n'
         '\t\tbsp_total += header->lumps[read[i]].filelen;\n'
         '\t\tif (header->lumps[read[i]].filelen > size)\n'
         '\t\t\tsize = header->lumps[read[i]].filelen;\n'
         '\t}\n'
         '\tbsp_scratch = Mod_TempAlloc (size);\n'
         '\tbsp_scratch_size = size;\n'
         '}\n'
         '\n'
         '// ref_esp: len байт файла карты с места offset (от начала карты) — в буфер lump-ов\n'
         'static void Mod_LumpRead (int offset, int len)\n'
         '{\n'
         '\tfseek (bsp_file, bsp_base + offset, SEEK_SET);\n'
         '\tFS_Read (bsp_scratch, len, bsp_file);\n'
         '\tMod_BspProgress (len);\n'
         '}\n'
         '\n'
         '// ref_esp: загрузчик id над одним lump-ом карты (у файла целиком — как у id)\n'
         'static void Mod_Lump (void (*load) (lump_t *l), lump_t *l)\n'
         '{\n'
         '\tbyte\t*whole = mod_base, *part;\n'
         '\n'
         '\tif (!bsp_file)\n'
         '\t{\n'
         '\t\tload (l);\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tif (bsp_scratch && l->filelen <= bsp_scratch_size)\t// обычный путь: lump — в общий буфер\n'
         '\t{\n'
         '\t\tMod_LumpRead (l->fileofs, l->filelen);\n'
         '\t\tmod_base = bsp_scratch - l->fileofs;\n'
         '\t\tload (l);\n'
         '\t\tmod_base = whole;\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tpart = FS_ReadPart (bsp_file, bsp_base, l->fileofs, l->filelen);\n'
         '\tMod_BspProgress (l->filelen);\n'
         '\tmod_base = part - l->fileofs;\n'
         '\tload (l);\n'
         '\tmod_base = whole;\n'
         '\tFS_FreeFile (part);\n'
         '}\n'
         '\n'
         '// ref_esp: свет карты — как Mod_LoadLighting id (у точки — самый яркий из R, G, B, тем же правилом),\n'
         '// но файл читается частями через буфер lump-ов: свет целиком — 0,6–1,5 МБ временного буфера\n'
         'static void Mod_LumpLighting (lump_t *l)\n'
         '{\n'
         '\tint\t\ti, n, done, size, part;\n'
         '\tbyte\t*in, *out;\n'
         '\n'
         '\tif (!bsp_file || !bsp_scratch)\n'
         '\t{\n'
         '\t\tMod_Lump (Mod_LoadLighting, l);\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tif (!l->filelen)\n'
         '\t{\n'
         '\t\tloadmodel->lightdata = NULL;\n'
         '\t\treturn;\n'
         '\t}\n'
         '\tsize = l->filelen/3;\t\t\t\t// точек света (у id хвост меньше 3 байт тоже не читается)\n'
         '\tloadmodel->lightdata = Hunk_Alloc (size);\n'
         '\tout = loadmodel->lightdata;\n'
         '\tpart = bsp_scratch_size / 3 * 3;\t// часть — целыми точками (по 3 байта)\n'
         '\tfor (done = 0 ; done < size * 3 ; done += n)\n'
         '\t{\n'
         '\t\tn = size * 3 - done < part ? size * 3 - done : part;\n'
         '\t\tMod_LumpRead (l->fileofs + done, n);\n'
         '\t\tfor (i = 0, in = bsp_scratch ; i < n / 3 ; i++, in += 3, out++)\n'
         '\t\t{\n'
         '\t\t\tif (in[0] > in[1] && in[0] > in[2])\n'
         '\t\t\t\t*out = in[0];\n'
         '\t\t\telse if (in[1] > in[0] && in[1] > in[2])\n'
         '\t\t\t\t*out = in[1];\n'
         '\t\t\telse\n'
         '\t\t\t\t*out = in[2];\n'
         '\t\t}\n'
         '\t}\n'
         '}\n'
         '\n'
         'void Mod_LoadBrushModel (model_t *mod, void *buffer)\n{\n'),
        ('\tMod_LoadVertexes (&header->lumps[LUMP_VERTEXES]);\n',
         '\tMod_LumpScratch (header);\t\t// ref_esp: один буфер на все lump-ы карты — до первого\n'
         '\tMod_Lump (Mod_LoadVertexes, &header->lumps[LUMP_VERTEXES]);\n'),
        ('\tMod_LoadEdges (&header->lumps[LUMP_EDGES]);\n',
         '\tMod_Lump (Mod_LoadEdges, &header->lumps[LUMP_EDGES]);\n'),
        ('\tMod_LoadSurfedges (&header->lumps[LUMP_SURFEDGES]);\n',
         '\tMod_Lump (Mod_LoadSurfedges, &header->lumps[LUMP_SURFEDGES]);\n'),
        ('\tMod_LoadLighting (&header->lumps[LUMP_LIGHTING]);\n',
         '\tMod_LumpLighting (&header->lumps[LUMP_LIGHTING]);\t// ref_esp: частями через буфер lump-ов\n'),
        ('\tMod_LoadPlanes (&header->lumps[LUMP_PLANES]);\n',
         '\tMod_Lump (Mod_LoadPlanes, &header->lumps[LUMP_PLANES]);\n'),
        ('\tMod_LoadTexinfo (&header->lumps[LUMP_TEXINFO]);\n',
         '\tMod_Lump (Mod_LoadTexinfo, &header->lumps[LUMP_TEXINFO]);\n'),
        ('\tMod_LoadFaces (&header->lumps[LUMP_FACES]);\n',
         '\tMod_Lump (Mod_LoadFaces, &header->lumps[LUMP_FACES]);\n'),
        ('\tMod_LoadMarksurfaces (&header->lumps[LUMP_LEAFFACES]);\n',
         '\tMod_Lump (Mod_LoadMarksurfaces, &header->lumps[LUMP_LEAFFACES]);\n'),
        ('\tMod_LoadVisibility (&header->lumps[LUMP_VISIBILITY]);\n',
         '\tMod_Lump (Mod_LoadVisibility, &header->lumps[LUMP_VISIBILITY]);\n'),
        ('\tMod_LoadLeafs (&header->lumps[LUMP_LEAFS]);\n',
         '\tMod_Lump (Mod_LoadLeafs, &header->lumps[LUMP_LEAFS]);\n'),
        ('\tMod_LoadNodes (&header->lumps[LUMP_NODES]);\n',
         '\tMod_Lump (Mod_LoadNodes, &header->lumps[LUMP_NODES]);\n'),
        ('\tMod_LoadSubmodels (&header->lumps[LUMP_MODELS]);\n',
         '\tMod_Lump (Mod_LoadSubmodels, &header->lumps[LUMP_MODELS]);\n'),
        # плоскости и видимость — общие с моделью столкновений (CM_SharedPlanes/Visibility, cmodel.c):
        # те же лумпы файла, у рендера копий нет (−0,3…0,5 МБ на карту); нет общих — свои, как у id
        ('\tMod_Lump (Mod_LoadPlanes, &header->lumps[LUMP_PLANES]);\n',
         '\tloadmodel->planes = (mplane_t *)CM_SharedPlanes (loadmodel->name,\n'
         '\t\theader->lumps[LUMP_PLANES].filelen / (int)sizeof(dplane_t));\t// ref_esp: общие с CM\n'
         '\tif (loadmodel->planes)\n'
         '\t\tloadmodel->numplanes = header->lumps[LUMP_PLANES].filelen / (int)sizeof(dplane_t);\n'
         '\telse\n'
         '\t\tMod_Lump (Mod_LoadPlanes, &header->lumps[LUMP_PLANES]);\n'),
        ('\tMod_Lump (Mod_LoadVisibility, &header->lumps[LUMP_VISIBILITY]);\n',
         '\tloadmodel->vis = (dvis_t *)CM_SharedVisibility (loadmodel->name,\n'
         '\t\theader->lumps[LUMP_VISIBILITY].filelen);\t\t\t// ref_esp: общая с CM\n'
         '\tif (!loadmodel->vis)\n'
         '\t\tMod_Lump (Mod_LoadVisibility, &header->lumps[LUMP_VISIBILITY]);\n'),
        # модели md2 (Unreal NG 24.11: на base2 модель 305 КБ не легла — файл лежал в зоне и ждал второй такой же
        # кусок в hunk): файл читается прямо в hunk, Mod_LoadAliasModel раскладывает его на месте — id копирует
        # каждую часть md2 на то же смещение, меняя только порядок байт (у ESP он и так свой)
        # 24.11, Unreal NG (демо 2, waste2): модель 466 КБ не легла — свободно 720 КБ, но самый большой кусок
        # 224 КБ. Поэтому md2 больше не читается одним сплошным куском (md2_load ниже): начало файла — на месте
        # (как раньше), кадры анимации (88 % файла) — группами до 64 КБ, команды GL (6 %) — не читаются.
        ('void\t*FS_ReadPart (FILE *f, int base, int offset, int len);\n',
         'void\t*FS_ReadPart (FILE *f, int base, int offset, int len);\n'
         'void\tFS_Read (void *buffer, int len, FILE *f);\n'
         'static void\t*md2_inplace;\t// начало файла md2, прочитанное прямо в hunk (md2_load → Mod_LoadAliasModel)\n'
         '\n'
         '// ref_esp: временный буфер загрузки (lump-ы карты, группа кадров md2 по пути в ядро). На ESP — сверху\n'
         '// «области уровня» (sys_esp.c, 24.11.2026): большие сплошные куски от кучи PSRAM тогда не нужны, а\n'
         '// за несколько смен карт куча дробилась так, что их не находилось. Нет места там (или ПК) — куча.\n'
         '#ifdef BZ_ESP\n'
         'void\t*q2_level_scratch (int size);\n'
         'int\tq2_level_scratch_free (void *p);\n'
         '#endif\n'
         'static void *Mod_TempAlloc (int size)\n'
         '{\n'
         '#ifdef BZ_ESP\n'
         '\tvoid\t*p = q2_level_scratch (size);\n'
         '\n'
         '\tif (p)\n'
         '\t\treturn p;\n'
         '#endif\n'
         '\treturn malloc (size);\n'
         '}\n'
         '\n'
         'static void Mod_TempFree (void *p)\n'
         '{\n'
         '#ifdef BZ_ESP\n'
         '\tif (q2_level_scratch_free (p))\t// из области уровня — не в кучу\n'
         '\t\treturn;\n'
         '#endif\n'
         '\tfree (p);\n'
         '}\n'
         '\n'
         '/*\n'
         '================\n'
         'md2_load — ref_esp: модель md2 из pak без копии в зоне и без одного сплошного куска памяти.\n'
         '\n'
         '- Начало файла (заголовок, имена шкурок, s/t, треугольники — всё до ofs_frames) читается в первый\n'
         '  кусок hunk ровно по размеру: id хранит адрес заголовка как extradata, а Mod_LoadAliasModel\n'
         '  раскладывает модель на месте (порядок байт у ESP и так свой, копировать нечего).\n'
         '- Кадры анимации (88 % всех md2 pak) — группами до 64 КБ, каждая своим Hunk_Alloc: hunk сам сцепляет\n'
         '  куски, и модели на 0,5–1 МБ не нужен сплошной кусок кучи. Адреса кадров — в таблице mod->md2_frames\n'
         '  (её читают Mod_LoadAliasModel и R_AliasSetupFrames). Группа, а не кадр: на ESP без флешки каждое\n'
         '  чтение pak — запрос через Z80, а кадров у модели до 491.\n'
         '- Команды GL (6 %, у всех 119 md2 pak — в конце файла) программному рендеру не нужны: не читаются,\n'
         '  num_glcmds = 0.\n'
         'Если части лежат не так (id так не пишет) — файл целиком на место одним куском, как раньше.\n'
         'Возвращает начало модели в hunk (или NULL — файла нет), modfilelen — длина файла.\n'
         '\n'
         '24.11 («Да, SDRAM Tang»): кадры, если ядро умеет читать SDRAM (SPEC 0.4), — не в PSRAM, а в\n'
         'хранилище SDRAM ядра (md2_store); рендер берёт их через кольцо недавних кадров (BZ_Md2Frame).\n'
         '================\n'
         '*/\n'
         'static unsigned\tmd2_serial;\t// номер модели md2 для кольца кадров: не повторяется\n'
         '\n'
         '/*\n'
         '================\n'
         'md2_store — ref_esp: кадры md2 (файл стоит на первом кадре) — в хранилище SDRAM ядра группами,\n'
         'как в md2_load; адреса кадров — mod->md2_sdram, куски — mod->md2_span. 0 — хранилища нет или в нём\n'
         'не хватило места: тогда всё взятое возвращается, файл — снова на первом кадре (кадры пойдут в PSRAM).\n'
         'Адрес кадра кратен 4 (группа — кратна 32): ESP читает его окном, начиная с ближней пачки.\n'
         '================\n'
         '*/\n'
         'static int md2_store (model_t *mod, const dmdl_t *h, int per, FILE *f)\n'
         '{\n'
         '\tint\t\ti, j, n, g, ngroups = (h->num_frames + per - 1) / per;\n'
         '\tunsigned\t*span, *addr, a;\n'
         '\tbyte\t*tmp;\n'
         '\tlong\tat = ftell (f);\n'
         '\n'
         '\tif (!BZ_StoreAvailable ())\n'
         '\t\treturn 0;\n'
         '\tspan = Hunk_Alloc (ngroups * 2 * (int)sizeof(unsigned));\n'
         '\taddr = Hunk_Alloc (h->num_frames * (int)sizeof(unsigned));\n'
         '\ttmp = Mod_TempAlloc (per * h->framesize);\t// группа по пути в ядро — на миг\n'
         '\tfor (i = 0, g = 0 ; i < h->num_frames ; i += n, g++)\n'
         '\t{\n'
         '\t\tn = h->num_frames - i < per ? h->num_frames - i : per;\n'
         '\t\ta = BZ_StoreAlloc (n * h->framesize);\n'
         '\t\tif (!a)\n'
         '\t\t\tbreak;\n'
         '\t\tspan[2 * g] = a;\n'
         '\t\tspan[2 * g + 1] = n * h->framesize;\n'
         '\t\tFS_Read (tmp, n * h->framesize, f);\n'
         '\t\tBZ_StoreWrite (a, tmp, n * h->framesize);\n'
         '\t\tfor (j = 0 ; j < n ; j++)\n'
         '\t\t\taddr[i + j] = a + j * h->framesize;\n'
         '\t}\n'
         '\tMod_TempFree (tmp);\n'
         '\tif (i < h->num_frames)\t\t// места не хватило: взятое — назад, файл — на первый кадр\n'
         '\t{\n'
         '\t\twhile (g--)\n'
         '\t\t\tBZ_StoreFree (span[2 * g], span[2 * g + 1]);\n'
         '\t\tfseek (f, at, SEEK_SET);\n'
         '\t\treturn 0;\n'
         '\t}\n'
         '\tBZ_StoreMarkModel ();\t\t// ESP: перед первым чтением — дождаться, что ядро всё записало (метка кадров md2)\n'
         '\tmod->md2_sdram = addr;\n'
         '\tmod->md2_span = span;\n'
         '\tmod->md2_nspan = ngroups;\n'
         '\treturn 1;\n'
         '}\n'
         '\n'
         '// ref_esp: вернуть хранилищу куски модели (зовут все места, где модель освобождается)\n'
         'static void Mod_EspStoreFree (model_t *mod)\n'
         '{\n'
         '\tint\ti;\n'
         '\n'
         '\tfor (i = 0 ; i < mod->md2_nspan ; i++)\n'
         '\t\tBZ_StoreFree (mod->md2_span[2 * i], mod->md2_span[2 * i + 1]);\n'
         '}\n'
         '\n'
         'static void *md2_load (model_t *mod)\n'
         '{\n'
         '\tFILE\t*f;\n'
         '\tdmdl_t\th, *head;\n'
         '\tbyte\t*group;\n'
         '\tint\t\ti, j, n, per;\n'
         '\n'
         '\tmodfilelen = FS_FOpenFile (mod->name, &f);\n'
         '\tif (!f)\n'
         '\t\treturn NULL;\n'
         '\tif (modfilelen < (int)sizeof(dmdl_t))\n'
         '\t{\n'
         '\t\tFS_FCloseFile (f);\n'
         '\t\treturn NULL;\n'
         '\t}\n'
         '\tFS_Read (&h, sizeof(h), f);\n'
         '\t// шкурки, s/t и треугольники — между заголовком и кадрами; кадры — целиком в файле\n'
         '\tif (h.ofs_skins >= (int)sizeof(dmdl_t) && h.ofs_st >= (int)sizeof(dmdl_t) && h.ofs_tris >= (int)sizeof(dmdl_t)\n'
         '\t\t&& h.ofs_skins + h.num_skins * MAX_SKINNAME <= h.ofs_frames\n'
         '\t\t&& h.ofs_st + h.num_st * (int)sizeof(dstvert_t) <= h.ofs_frames\n'
         '\t\t&& h.ofs_tris + h.num_tris * (int)sizeof(dtriangle_t) <= h.ofs_frames\n'
         '\t\t&& h.num_frames > 0 && h.framesize >= (int)sizeof(daliasframe_t) && !(h.framesize & 3)\n'
         '\t\t&& h.ofs_frames + h.num_frames * h.framesize <= modfilelen)\n'
         '\t{\n'
         '\t\tper = 65536 / h.framesize;\t\t// кадров в группе (до 64 КБ)\n'
         '\t\tif (per < 1)\n'
         '\t\t\tper = 1;\n'
         '#ifdef BZ_ESP\n'
         '\t\t// ESP (sys_esp.c): hunk — цепочка кусков; точный запрос — первый кусок ровно под начало файла\n'
         '\t\tmod->extradata = Hunk_Begin ((h.ofs_frames + 31) & ~31);\n'
         '#else\n'
         '\t\t// ПК (id, q_shwin.c): Hunk_Begin сразу резервирует всю область — посчитать её, как Hunk_Alloc\n'
         '\t\t// (каждый кусок кратен 32)\n'
         '\t\t// (+ таблицы хранилища md2_store: адрес кадра и пары «адрес, длина» групп)\n'
         '\t\tj = ((h.ofs_frames + 31) & ~31) + ((h.num_frames * (int)sizeof(void *) + 31) & ~31)\n'
         '\t\t\t+ ((h.num_frames * 4 + 31) & ~31) + (((h.num_frames / per + 1) * 8 + 31) & ~31);\n'
         '\t\tfor (i = 0 ; i < h.num_frames ; i += n)\n'
         '\t\t{\n'
         '\t\t\tn = h.num_frames - i < per ? h.num_frames - i : per;\n'
         '\t\t\tj += (n * h.framesize + 31) & ~31;\n'
         '\t\t}\n'
         '\t\tmod->extradata = Hunk_Begin (j);\n'
         '#endif\n'
         '\t\thead = Hunk_Alloc (h.ofs_frames);\n'
         '\t\tmemcpy (head, &h, sizeof(h));\n'
         '\t\tFS_Read ((byte *)head + sizeof(h), h.ofs_frames - (int)sizeof(h), f);\n'
         '\t\thead->num_glcmds = 0;\t\t\t// команды GL не прочитаны\n'
         '\t\tmod->md2_id = ++md2_serial;\n'
         '\t\tif (!md2_store (mod, &h, per, f))\t// SPEC 0.4: кадры — в SDRAM ядра; места нет — в PSRAM\n'
         '\t\t{\n'
         '\t\t\tif (BZ_StoreAvailable ())\t// хранилище есть, а места нет: в журнал — PSRAM ESP тесна\n'
         '\t\t\t\tri.Con_Printf (PRINT_ALL, "ref_esp: %s: md2 frames in PSRAM (%d KB), core SDRAM full\\n",\n'
         '\t\t\t\t\tmod->name, (h.num_frames * h.framesize) >> 10);\n'
         '\t\t\tmod->md2_frames = Hunk_Alloc (h.num_frames * (int)sizeof(void *));\n'
         '\t\t\tfor (i = 0 ; i < h.num_frames ; i += n)\n'
         '\t\t\t{\n'
         '\t\t\t\tn = h.num_frames - i < per ? h.num_frames - i : per;\n'
         '\t\t\t\tgroup = Hunk_Alloc (n * h.framesize);\n'
         '\t\t\t\tFS_Read (group, n * h.framesize, f);\t// кадры в файле подряд — читать по порядку\n'
         '\t\t\t\tfor (j = 0 ; j < n ; j++)\n'
         '\t\t\t\t\tmod->md2_frames[i + j] = group + j * h.framesize;\n'
         '\t\t\t}\n'
         '\t\t}\n'
         '\t}\n'
         '\telse\n'
         '\t{\n'
         '\t\tmod->extradata = Hunk_Begin ((modfilelen + 31) & ~31);\t// Hunk_Alloc округляет до 32\n'
         '\t\thead = Hunk_Alloc (modfilelen);\n'
         '\t\tmemcpy (head, &h, sizeof(h));\n'
         '\t\tFS_Read ((byte *)head + sizeof(h), modfilelen - (int)sizeof(h), f);\n'
         '\t}\n'
         '\tFS_FCloseFile (f);\n'
         '\tmd2_inplace = head;\n'
         '\treturn head;\n'
         '}\n'),
        ('\telse\n\t\tmodfilelen = ri.FS_LoadFile (mod->name, (void **)&buf);\n',
         '\telse if (strlen (mod->name) > 4 && !Q_stricmp (mod->name + strlen (mod->name) - 4, ".md2"))\n'
         '\t\tbuf = md2_load (mod);\t\t// ref_esp: md2 — в hunk без копии в зоне, кадры группами\n'
         '\telse\n\t\tmodfilelen = ri.FS_LoadFile (mod->name, (void **)&buf);\n'),
        ('\tcase IDALIASHEADER:\n\t\tloadmodel->extradata = Hunk_Begin (0x200000);\n',
         '\tcase IDALIASHEADER:\n'
         '\t\tif (!md2_inplace)\t// ref_esp: файл md2 уже в hunk (Mod_ForName)\n'
         '\t\t\tloadmodel->extradata = Hunk_Begin (0x200000);\n'),
        ('\tri.FS_FreeFile (buf);\n\tif (bsp_file)\t\t// ref_esp: файл карты разобран\n',
         '\tif (md2_inplace)\t\t// ref_esp: файл md2 — это и есть модель в hunk, не освобождать\n'
         '\t\tmd2_inplace = NULL;\n'
         '\telse\n'
         '\t\tri.FS_FreeFile (buf);\n'
         '\tif (bsp_file)\t\t// ref_esp: файл карты разобран\n'),
        ('\tpheader = Hunk_Alloc (LittleLong(pinmodel->ofs_end));\n',
         '\t// ref_esp: файл уже в hunk (md2_inplace) — модель раскладывается прямо в нём\n'
         '\tif (buffer == md2_inplace && LittleLong (pinmodel->ofs_end) <= modfilelen)\n'
         '\t\tpheader = (dmdl_t *)buffer;\n'
         '\telse\n'
         '\t\tpheader = Hunk_Alloc (LittleLong(pinmodel->ofs_end));\n'),
        # кадры в SDRAM ядра (md2_store) — на ESP их нет, раскладывать нечего (порядок байт у ESP свой)
        ('// load the frames\n//\n\tfor (i=0 ; i<pheader->num_frames ; i++)\n',
         '// load the frames\n//\n'
         '\tif (!mod->md2_sdram)\t// ref_esp: кадры в SDRAM ядра — на ESP их нет\n'
         '\tfor (i=0 ; i<pheader->num_frames ; i++)\n'),
        ('\t\tmemcpy (poutframe->name, pinframe->name, sizeof(poutframe->name));\n',
         '\t\tif (mod->md2_frames)\t// ref_esp: кадры md2 — группами (md2_load), а не после треугольников\n'
         '\t\t\tpinframe = poutframe = mod->md2_frames[i];\n'
         '\t\tif (poutframe != pinframe)\t// ref_esp: на месте копировать нечего\n'
         '\t\t\tmemcpy (poutframe->name, pinframe->name, sizeof(poutframe->name));\n'),
        ('\t\t// verts are all 8 bit, so no swapping needed\n\t\tmemcpy (poutframe->verts, pinframe->verts, \n',
         '\t\t// verts are all 8 bit, so no swapping needed\n\t\tif (poutframe != pinframe)\n'
         '\t\tmemcpy (poutframe->verts, pinframe->verts, \n'),
        ('\tmemcpy ((char *)pheader + pheader->ofs_skins, (char *)pinmodel + pheader->ofs_skins,\n',
         '\tif (pheader != pinmodel)\t// ref_esp: на месте копировать нечего\n'
         '\tmemcpy ((char *)pheader + pheader->ofs_skins, (char *)pinmodel + pheader->ofs_skins,\n'),
    ],
    'r_alias.c': [
        # кадры md2 — группами по 64 КБ (md2_load, r_model.c): их адреса в таблице, а не подряд за треугольниками
        ('\tr_lastframe = (daliasframe_t *)((byte *)pmdl + pmdl->ofs_frames \n\t\t+ lastframe * pmdl->framesize);\n',
         '\tr_lastframe = (daliasframe_t *)((byte *)pmdl + pmdl->ofs_frames \n\t\t+ lastframe * pmdl->framesize);\n'
         '\tif (currentmodel->md2_frames)\t// ref_esp: кадры md2 — отдельными группами (md2_load)\n'
         '\t{\n'
         '\t\tr_thisframe = currentmodel->md2_frames[thisframe];\n'
         '\t\tr_lastframe = currentmodel->md2_frames[lastframe];\n'
         '\t}\n'
         '\tif (currentmodel->md2_sdram)\t// ref_esp: кадры md2 в SDRAM ядра — через кольцо недавних кадров\n'
         '\t{\n'
         '\t\tr_thisframe = (daliasframe_t *)BZ_Md2Frame (currentmodel->md2_id, thisframe,\n'
         '\t\t\tcurrentmodel->md2_sdram[thisframe], pmdl->framesize);\n'
         '\t\tr_lastframe = (daliasframe_t *)BZ_Md2Frame (currentmodel->md2_id, lastframe,\n'
         '\t\t\tcurrentmodel->md2_sdram[lastframe], pmdl->framesize);\n'
         '\t}\n'
         '\t// замер на ПК (bz_md2trace): какие кадры моделей берёт рендер — для кэша кадров md2 на ESP\n'
         '\tBZ_Md2Access (currentmodel->name, thisframe, pmdl->framesize);\n'
         '\tif (lastframe != thisframe)\n'
         '\t\tBZ_Md2Access (currentmodel->name, lastframe, pmdl->framesize);\n'),
        # 26.11.2026, горячий модуль Q2HOT.ELF: перевод вершин модели в экран (R_AliasTransformFinalVerts,
        # R_AliasProjectAndClipTestFinalVert — ~11 % команд кадра) — в gpu/front/bz_hot.c тем же текстом; здесь
        # их нет, их имена даёт bz_front.c (своя копия или копия модуля во внутреннем ОЗУ). s_ziscale модуль
        # берёт по адресу — не static.
        ('static float\ts_ziscale;\n',
         'float\ts_ziscale;\t\t// ref_esp: адрес берёт горячий модуль (bz_front.c, BZ_HotTables)\n'),
        ('#else\nvoid R_AliasTransformFinalVerts( int numpoints, finalvert_t *fv, dtrivertx_t *oldv, dtrivertx_t *newv )\n'
         '{\n\tint i;\n',
         '#elif 0\t// ref_esp: перенесена в gpu/front/bz_hot.c (горячий модуль)\n'
         'void R_AliasTransformFinalVerts( int numpoints, finalvert_t *fv, dtrivertx_t *oldv, dtrivertx_t *newv )\n'
         '{\n\tint i;\n'),
        ('void R_AliasProjectAndClipTestFinalVert( finalvert_t *fv )\n{\n',
         '#if 0\t// ref_esp: перенесена в gpu/front/bz_hot.c (горячий модуль)\n'
         'void R_AliasProjectAndClipTestFinalVert( finalvert_t *fv )\n{\n'),
        ('\t\tfv->flags |= ALIAS_BOTTOM_CLIP;\t\n}\n',
         '\t\tfv->flags |= ALIAS_BOTTOM_CLIP;\t\n}\n#endif\n'),
    ],
    'r_model.h': [
        # модель md2: адреса кадров анимации — они лежат группами в разных кусках hunk (md2_load, r_model.c)
        ('\tint\t\t\textradatasize;\n',
         '\tint\t\t\textradatasize;\n'
         '\tvoid\t\t**md2_frames;\t// ref_esp: адреса кадров md2 (md2_load) или NULL — кадры подряд, как у id\n'
         '\tunsigned\tmd2_id;\t\t// ref_esp: номер модели md2 для кольца кадров (BZ_Md2Frame)\n'
         '\tunsigned\t*md2_sdram;\t// ref_esp: адреса кадров md2 в SDRAM ядра (SPEC 0.4) или NULL\n'
         '\tunsigned\t*md2_span;\t// ref_esp: куски хранилища модели — пары «адрес, длина»\n'
         '\tint\t\t\tmd2_nspan;\n'),
        # модель мира: поля растеризатора id, которого в ref_esp нет (R_ScanEdges не зовётся, рёбер нет —
        # это проверяет R_EdgeDrawing). На base2 — ~150 КБ граней и ~95 КБ рёбер
        ('\tunsigned short\tv[2];\n\tunsigned int\tcachededgeoffset;\n} medge_t;\n',
         '\tunsigned short\tv[2];\n'
         '\t// ref_esp: cachededgeoffset (кэш рёбер растеризатора id) убран — ребро 4 байта вместо 8\n'
         '} medge_t;\n'),
        ('// surface generation data\n\tstruct surfcache_s\t*cachespots[MIPLEVELS];\n',
         '// surface generation data\n'
         '\t// ref_esp: cachespots (кэш поверхностей id, 4 указателя) убран — свет и текстуру сводит ядро Tang\n'),
    ],
    'r_rast.c': [
        # небо: у рёбер коробки неба поля кэша больше нет
        ('\t\tr_skyedges[i].cachededgeoffset = 0;\n',
         '\t\t// ref_esp: поля cachededgeoffset нет (кэш рёбер растеризатора id)\n'),
        # кэшированное ребро зовёт только программная часть R_RenderFace — её в ref_esp нет
        ('void R_EmitCachedEdge (void)\n{\n\tedge_t\t\t*pedge_t;\n\n'
         '\tpedge_t = (edge_t *)((unsigned long)r_edges + r_pedge->cachededgeoffset);\n\n'
         '\tif (!pedge_t->surfs[0])\n\t\tpedge_t->surfs[0] = surface_p - surfaces;\n'
         '\telse\n\t\tpedge_t->surfs[1] = surface_p - surfaces;\n\n'
         '\tif (pedge_t->nearzi > r_nearzi)\t// for mipmap finding\n\t\tr_nearzi = pedge_t->nearzi;\n\n'
         '\tr_emitted = 1;\n}\n',
         '#if 0\t// ref_esp: растеризатора id нет — кэшированных рёбер тоже (поле cachededgeoffset убрано)\n'
         'void R_EmitCachedEdge (void)\n{\n\tedge_t\t\t*pedge_t;\n\n'
         '\tpedge_t = (edge_t *)((unsigned long)r_edges + r_pedge->cachededgeoffset);\n\n'
         '\tif (!pedge_t->surfs[0])\n\t\tpedge_t->surfs[0] = surface_p - surfaces;\n'
         '\telse\n\t\tpedge_t->surfs[1] = surface_p - surfaces;\n\n'
         '\tif (pedge_t->nearzi > r_nearzi)\t// for mipmap finding\n\t\tr_nearzi = pedge_t->nearzi;\n\n'
         '\tr_emitted = 1;\n}\n#endif\n'),
        # грань мира: в ref_esp кадр всегда идёт с bz_pass — программная часть id не нужна
        ('\t\tBZ_RenderFace (fa, clipflags);\t// ref_bz: грань — в команды адаптера\n\t\treturn;\n\t}\n',
         '\t\tBZ_RenderFace (fa, clipflags);\t// ref_bz: грань — в команды адаптера\n\t\treturn;\n\t}\n'
         '\tri.Sys_Error (ERR_FATAL, "ref_esp: R_RenderFace without bz_pass");\n'
         '#if 0\t// ref_esp: рёбра растеризатора id (и их кэш cachededgeoffset) не порождаются\n'),
        ('\tsurface_p++;\n}\n\n\n/*\n================\nR_RenderBmodelFace\n',
         '\tsurface_p++;\n#endif\t// ref_esp\n}\n\n\n/*\n================\nR_RenderBmodelFace\n'),
    ],
    'r_surf.c': [
        # кэш поверхностей id: в ref_esp его не зовут (R_ScanEdges нет, прозрачное — BZ_AlphaSurfaces;
        # на ПК swimp_bz.c сверяет образец кэша после каждого кадра); поля cachespots у грани больше нет
        ('surfcache_t *D_CacheSurface (msurface_t *surface, int miplevel)\n{\n\tsurfcache_t     *cache;\n',
         'surfcache_t *D_CacheSurface (msurface_t *surface, int miplevel)\n{\n\tsurfcache_t     *cache;\n\n'
         '\tri.Sys_Error (ERR_FATAL, "ref_esp: D_CacheSurface");\t// растеризатора id нет\n'
         '\treturn NULL;\n'
         '#if 0\t// ref_esp: поле cachespots убрано из msurface_t\n'),
        ('\t// rasterize the surface into the cache\n\tR_DrawSurface ();\n\n\treturn cache;\n}\n',
         '\t// rasterize the surface into the cache\n\tR_DrawSurface ();\n\n\treturn cache;\n#endif\t// ref_esp\n}\n'),
        # кэш поверхностей: на ESP — один маленький пустой блок (D_FlushCaches ходит по его списку)
        ('\t// round up to page size\n\tsize = (size + 8191) & ~8191;\n',
         '\t// round up to page size\n\tsize = (size + 8191) & ~8191;\n'
         '#ifdef BZ_ESP\n\tsize = 1024;\t\t// ref_esp: свет и текстуру сводит ядро Tang — кэш id не нужен\n#endif\n'),
        ('\tsc_base = (surfcache_t *)malloc(size);\n',
         '\tsc_base = (surfcache_t *)malloc(size);\n'
         '#ifndef BZ_ESP\t// ref_esp на ПК: кэш — образец (заголовок id запишет ниже), его проверяет swimp_bz.c\n'
         '\tmemset (sc_base, 0xA5, size);\n#endif\n'),
    ],
    'r_misc.c': [
        ('\t\tmemset( d_pzbuffer, 0xff, vid.width * vid.height * sizeof( d_pzbuffer[0] ) );\n',
         '#ifndef BZ_ESP\t// ref_esp: z-буфер у ядра Tang\n'
         '\t\tmemset( d_pzbuffer, 0xff, vid.width * vid.height * sizeof( d_pzbuffer[0] ) );\n#endif\n'),
    ],
    'r_draw.c': [
        # 2D: после команды адаптера — выйти, в память кадра id не рисовать
        ('\tBZ_DrawChar (x, y, num, draw_chars);\t// ref_bz: 2D командой адаптера\n',
         '\tBZ_DrawChar (x, y, num, draw_chars);\t// ref_bz: 2D командой адаптера\n'
         '\treturn;\t\t\t\t\t\t\t// ref_esp: кадра id нет\n'),
        ('\tBZ_StretchPic (x, y, w, h, pic);\t// ref_bz\n',
         '\tBZ_StretchPic (x, y, w, h, pic);\t// ref_bz\n\treturn;\t\t\t\t\t\t\t// ref_esp\n'),
        ('\tBZ_DrawPic (x, y, pic);\t// ref_bz\n',
         '\tBZ_DrawPic (x, y, pic);\t// ref_bz\n\treturn;\t\t\t\t\t\t\t// ref_esp\n'),
        ('\tBZ_TileClear (x, y, w, h, pic);\t// ref_bz\n',
         '\tBZ_TileClear (x, y, w, h, pic);\t// ref_bz\n\treturn;\t\t\t\t\t\t\t// ref_esp\n'),
        ('\tBZ_Fill (x, y, w, h, c);\t// ref_bz\n',
         '\tBZ_Fill (x, y, w, h, c);\t// ref_bz\n\treturn;\t\t\t\t\t\t\t// ref_esp\n'),
        ('\tBZ_FadeScreen ();\t// ref_bz\n',
         '\tBZ_FadeScreen ();\t// ref_bz\n\treturn;\t\t\t\t\t\t\t// ref_esp\n'),
    ],
    # 28.11.2026 («разгрузить тяжёлые кадры»): частица — без double (у ESP32-S3 он программный). 1/z во float — то же
    # частное (53 ≥ 2·24 + 2: двойное округление безвредно); (int)(x + 0.5) с суммой в double — esp_round_half, то же
    # целое: |x| < 2^23 — сумма во float точна; больше — x целое (x ≥ 0 — x, x < 0 — x + 1).
    'r_part.c': [
        ('\tzi = 1.0 / transformed[2];\n'
         '\tu = (int)(xcenter + zi * transformed[0] + 0.5);\n'
         '\tv = (int)(ycenter - zi * transformed[1] + 0.5);\n',
         '\tzi = 1.0f / transformed[2];\t\t\t\t\t\t// ref_esp: без double\n'
         '\t{\n'
         '\t\tconst float fu = xcenter + zi * transformed[0], fv = ycenter - zi * transformed[1];\n'
         '\t\tu = (fu < 8388608.0f && fu > -8388608.0f) ? (int)(fu + 0.5f) : (int)fu + (fu < 0);\n'
         '\t\tv = (fv < 8388608.0f && fv > -8388608.0f) ? (int)(fv + 0.5f) : (int)fv + (fv < 0);\n'
         '\t}\n'),
    ],
}


# Замер частей рендера (25.11.2026, «оптимизация быстродействия»): метки Q2M_R_* (measure/q2m.h) — время по
# частям R_RenderFrame на плате (журнал раз в 5 с, esp/main/q2_app.c) и на ПК (q2esp.exe).
ESP['r_main.c'] += [
    ('\tR_SetupFrame ();\n',
     '\tQ2M_BEGIN (Q2M_R_SETUP);\n\tR_SetupFrame ();\n'),
    ('\tR_PushDlights (r_worldmodel);\n',
     '\tR_PushDlights (r_worldmodel);\n\tQ2M_END (Q2M_R_SETUP);\n'),
    ('\tR_RenderWorld ();\n',
     '\tQ2M_BEGIN (Q2M_R_WORLD);\n\tR_RenderWorld ();\n\tQ2M_END (Q2M_R_WORLD);\n'),
    ('\tR_DrawBEntitiesOnList ();\n',
     '\tQ2M_BEGIN (Q2M_R_BMODEL);\n\tR_DrawBEntitiesOnList ();\n\tQ2M_END (Q2M_R_BMODEL);\n'),
    ('\tR_DrawEntitiesOnList ();\n',
     '\tQ2M_BEGIN (Q2M_R_ENT);\n\tR_DrawEntitiesOnList ();\n\tQ2M_END (Q2M_R_ENT);\n'),
    ('\tR_DrawParticles ();\n',
     '\tQ2M_BEGIN (Q2M_R_PART);\n\tR_DrawParticles ();\n\tQ2M_END (Q2M_R_PART);\n'),
    ('\tR_DrawAlphaSurfaces();\n',
     '\tQ2M_BEGIN (Q2M_R_ALPHA);\n\tR_DrawAlphaSurfaces();\n\tQ2M_END (Q2M_R_ALPHA);\n'),
]


# Перечитывание текстуры .wal (25.11.2026): только заголовок (100 байт) и нужный мип-уровень, а не весь файл.
# На плате файл идёт с карты Evo (~76 КБ/с): стена 256×256 — 87 КБ (все уровни) и столько же PSRAM одним
# куском посреди игры, а дальнему уровню нужно 1–4 КБ. Движок и рендер — одна программа (REF_HARD_LINKED),
# поэтому FS_FOpenFile / FS_Read зовутся напрямую.
# Части моделей (26.11.2026, «нужно хотя бы 30 fps»: на плате ent ~15 мс кадра) — метки на модель, не на
# треугольник: кадры md2 (с чтением SDRAM ядра), свет (R_LightPoint), перевод вершин в экран, треугольники в
# пачку; и счёт моделей, вершин, треугольников (Q2M_K_* — числа, а не такты).
ESP['r_alias.c'] += [
    ('\tR_AliasSetupFrames( s_pmdl );\n',
     '\tQ2M_BEGIN (Q2M_A_FRM);\n\tR_AliasSetupFrames( s_pmdl );\n\tQ2M_END (Q2M_A_FRM);\n'),
    ('\tR_AliasSetupLighting ();\n',
     '\tQ2M_BEGIN (Q2M_A_LIT);\n\tR_AliasSetupLighting ();\n\tQ2M_END (Q2M_A_LIT);\n'),
    ('\tR_AliasTransformFinalVerts( aliasbatchedtransformdata.num_points,\n',
     '\tq2m_acc[Q2M_K_MDL]++;\n'
     '\tq2m_acc[Q2M_K_VERT] += s_pmdl->num_xyz;\n'
     '\tq2m_acc[Q2M_K_TRI] += s_pmdl->num_tris;\n'
     '\tQ2M_BEGIN (Q2M_A_XF);\n'
     '\tR_AliasTransformFinalVerts( aliasbatchedtransformdata.num_points,\n'),
    ('\t\tBZ_AliasModelBegin (pfinalverts, s_pmdl->num_xyz);\n',
     '\t\tQ2M_END (Q2M_A_XF);\n'
     '\t\tQ2M_BEGIN (Q2M_A_TRS);\n'
     '\t\tBZ_AliasModelBegin (pfinalverts, s_pmdl->num_xyz);\n'),
    ('\t\t\tBZ_AliasModelEnd ();\n\t\t\treturn;\n',
     '\t\t\tBZ_AliasModelEnd ();\n\t\t\tQ2M_END (Q2M_A_TRS);\n\t\t\treturn;\n'),
]


ESP.setdefault('r_image.c', []).append((
    '\t\tmiptex_t\t*mt;\n'
    '\t\tint\t\t\tsize = ri.FS_LoadFile (image->name, (void **)&mt);\n'
    '\n'
    '\t\tif (!mt)\n'
    '\t\t\treturn NULL;\n'
    '\t\tif (mip >= 0 && mip < MIPLEVELS && LittleLong (mt->width) == image->width &&\n'
    '\t\t    LittleLong (mt->height) == image->height &&\n'
    '\t\t    LittleLong (mt->offsets[mip]) + w * h <= size)\n'
    '\t\t{\n'
    '\t\t\tout = malloc (w * h);\n'
    '\t\t\tmemcpy (out, (byte *)mt + LittleLong (mt->offsets[mip]), w * h);\n'
    '\t\t}\n'
    '\t\tri.FS_FreeFile (mt);\n',
    '\t\t// ref_esp: только заголовок и нужный мип-уровень — не весь файл (25.11.2026)\n'
    '\t\tint\t\tFS_FOpenFile (char *filename, FILE **file);\n'
    '\t\tvoid\tFS_Read (void *buffer, int len, FILE *f);\n'
    '\t\tvoid\tFS_FCloseFile (FILE *f);\n'
    '\t\tmiptex_t\tmt;\n'
    '\t\tFILE\t\t*f;\n'
    '\t\tint\t\t\tsize = FS_FOpenFile (image->name, &f), base;\n'
    '\n'
    '\t\tif (!f)\n'
    '\t\t\treturn NULL;\n'
    '\t\tbase = ftell (f);\n'
    '\t\tif (size >= (int)sizeof(mt))\n'
    '\t\t{\n'
    '\t\t\tFS_Read (&mt, sizeof(mt), f);\n'
    '\t\t\tif (mip >= 0 && mip < MIPLEVELS && LittleLong (mt.width) == image->width &&\n'
    '\t\t\t    LittleLong (mt.height) == image->height &&\n'
    '\t\t\t    LittleLong (mt.offsets[mip]) + w * h <= size)\n'
    '\t\t\t{\n'
    '\t\t\t\tout = malloc (w * h);\n'
    '\t\t\t\tfseek (f, base + LittleLong (mt.offsets[mip]), SEEK_SET);\n'
    '\t\t\t\tFS_Read (out, w * h, f);\n'
    '\t\t\t}\n'
    '\t\t}\n'
    '\t\tFS_FCloseFile (f);\n'))


# 28.11.2026, «Модели на втором ядре» (gpu/front/bz_front.c, «МОДЕЛИ НА ЯДРЕ 0»): непрозрачные модели кадра считает
# задача LINK на ядре ESP 0, пока игра на ядре 1 обходит мир. Мир по ходу меняет переменные id, которые читают и
# модели: currententity, currentmodel, modelorg, r_entorigin (у дверей и лифтов — и vpn, vright, vup,
# view_clipplanes: R_RotateBmodel). Файлы моделей начинаются с gpu/front/bz_ent.h — в них эти имена (и ri: консоль
# общая) становятся своими копиями e_*, во всём файле разом, вместе с объявлениями r_local.h. Копии определены в
# r_ent.c (ниже), копии вида кладёт BZ_EntBegin. Другой код этих файлов, который зовёт игра (R_PushDlights,
# R_MarkLights у r_light.c), переименованных имён не трогает.
ENT_FIRST = '/*\nCopyright (C) 1997-2001 Id Software, Inc.\n'
ENT_INCLUDE = ('#include "bz_ent.h"\t// ref_esp (28.11.2026): модели на ядре ESP 0 — свои копии общих переменных id\n'
               + ENT_FIRST)
ENT = {
    'r_alias.c': [
        (ENT_FIRST, ENT_INCLUDE),
        # вершины модели в экране — не на стеке (80 КБ: у задачи LINK столько стека нет), а в буфере bz_front.c
        ('\tfinalvert_t\tfinalverts[MAXALIASVERTS +\n\t\t\t\t\t\t((CACHE_SIZE - 1) / sizeof(finalvert_t)) + 3];\n'
         '\tfinalvert_t\t*pfinalverts;\n',
         '\tfinalvert_t\t*pfinalverts;\n'),
        ('\t// put work vertexes on stack, cache aligned\n\tpfinalverts = (finalvert_t *)\n'
         '\t\t\t(((long)&finalverts[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));\n',
         '\t// ref_esp (28.11.2026): вершины — в буфере bz_front.c (BZ_EntBegin запас место на модели кадра)\n'
         '\tpfinalverts = (finalvert_t *)BZ_EntVerts (s_pmdl->num_xyz);\n'
         '\tif (!pfinalverts)\n\t\treturn;\n'),
    ],
    'r_sprite.c': [(ENT_FIRST, ENT_INCLUDE)],
    'r_poly.c': [(ENT_FIRST, ENT_INCLUDE)],
    'r_light.c': [(ENT_FIRST, ENT_INCLUDE)],
    'r_main.c': [
        # модели — со стороны 1 (их куски встают в поток здесь) или, если не отдавались, здесь же, как раньше;
        # прозрачные — всегда здесь. Проход id — в r_ent.c (E_DrawEntities), прежний текст — как был, не зовётся.
        ('void R_DrawEntitiesOnList (void)\n{\n',
         'extern entity_t\t*e_currententity;\n\n'
         'void R_DrawEntitiesOnList (void)\n{\n'
         '\tif (!r_drawentities->value)\n\t\treturn;\n'
         '\t// ref_esp (28.11.2026): непрозрачные модели посчитала сторона 1 (BZ_EntJoin — их куски встают в поток здесь),\n'
         '\t// иначе — здесь, как раньше; прозрачные — здесь. Без моделей id оставил бы currententity от мира — его\n'
         '\t// потом читает R_LightPoint (R_SetLightLevel), у которого это e_currententity.\n'
         '\tif (!BZ_EntJoin ())\n\t\tE_DrawEntities (0);\n'
         '\te_currententity = currententity;\n'
         '\tE_DrawEntities (1);\n'
         '}\n\n'
         'void R_DrawEntitiesOnList_id (void)\t// ref_esp: прежний проход (у ref_esp его ведёт r_ent.c)\n{\n'),
        ('\tif (bz_pass)\n\t\tBZ_Begin3D ();\t\t// ref_bz: очистка z, ножницы, фаза волны\n',
         '\tif (bz_pass)\n\t\tBZ_Begin3D ();\t\t// ref_bz: очистка z, ножницы, фаза волны\n'
         '\tBZ_EntBegin ();\t\t// ref_esp (28.11.2026): копии вида для моделей; модели — задаче LINK на ядре ESP 0\n'),
    ],
}

# r_ent.c: копии переменных id у моделей (bz_ent.h переименовывает имена ниже) и проход моделей id —
# R_DrawEntitiesOnList, по одному из двух его циклов за вызов.
R_ENT_C = '''/*
 * r_ent.c — модели кадра (ref_esp, 28.11.2026; сделан measure/patches/p06_ref_esp.py).
 * Начинается с bz_ent.h: currententity, vpn и т. д. здесь — копии моделей e_* (gpu/front/bz_front.c, «МОДЕЛИ НА
 * ЯДРЕ 0»). E_DrawEntities — цикл R_DrawEntitiesOnList id: translucent = 0 — первый (непрозрачные; его может
 * вести задача LINK на ядре 0), 1 — второй (прозрачные; всегда игра, после мира). Текст циклов — как у id.
 */
#include "bz_ent.h"
#include "r_local.h"
#include "bz_front.h"

entity_t	*currententity;		// e_currententity и т. д. (bz_ent.h)
model_t		*currentmodel;
vec3_t		modelorg, r_entorigin;
vec3_t		vpn, vright, vup;
clipplane_t	view_clipplanes[4];
refimport_t	ri;

void R_DrawNullModel( void );

void TransformVector (vec3_t in, vec3_t out)
{
	out[0] = DotProduct(in,vright);
	out[1] = DotProduct(in,vup);
	out[2] = DotProduct(in,vpn);
}

// AngleVectors id (q_shared.c) держит синусы в static — её зовёт и мир (RotatedBBox у дверей и лифтов). Здесь — та же,
// с переменными на стеке (E_AngleVectors, bz_ent.h).
void AngleVectors (vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	float		angle;
	float		sr, sp, sy, cr, cp, cy;

	angle = angles[YAW] * (M_PI*2 / 360);
	sy = sin(angle);
	cy = cos(angle);
	angle = angles[PITCH] * (M_PI*2 / 360);
	sp = sin(angle);
	cp = cos(angle);
	angle = angles[ROLL] * (M_PI*2 / 360);
	sr = sin(angle);
	cr = cos(angle);

	if (forward)
	{
		forward[0] = cp*cy;
		forward[1] = cp*sy;
		forward[2] = -sp;
	}
	if (right)
	{
		right[0] = (-1*sr*sp*cy+-1*cr*-sy);
		right[1] = (-1*sr*sp*sy+-1*cr*cy);
		right[2] = -1*sr*cp;
	}
	if (up)
	{
		up[0] = (cr*sp*cy+-sr*-sy);
		up[1] = (cr*sp*sy+-sr*cy);
		up[2] = cr*cp;
	}
}

void E_DrawEntities (int translucent)
{
	int			i;

	for (i=0 ; i<r_newrefdef.num_entities ; i++)
	{
		currententity = &r_newrefdef.entities[i];

		if ( !( currententity->flags & RF_TRANSLUCENT ) != !translucent )
			continue;
		if ( !translucent )
			BZ_EntTick ();		// сторона 1: задача LINK — своё

		if ( currententity->flags & RF_BEAM )
		{
			modelorg[0] = -r_origin[0];
			modelorg[1] = -r_origin[1];
			modelorg[2] = -r_origin[2];
			VectorCopy( vec3_origin, r_entorigin );
			R_DrawBeam( currententity );
		}
		else
		{
			currentmodel = currententity->model;
			if (!currentmodel)
			{
				R_DrawNullModel();
				continue;
			}
			VectorCopy (currententity->origin, r_entorigin);
			VectorSubtract (r_origin, r_entorigin, modelorg);

			switch (currentmodel->type)
			{
			case mod_sprite:
				R_DrawSprite ();
				break;

			case mod_alias:
				R_AliasDrawModel ();
				break;

			default:
				break;
			}
		}
	}
}
'''


def make(src, out):
    out = p04_ref_bz.make(src, out)
    for name, pairs in ESP.items():
        p04_ref_bz._patch(pathlib.Path(out) / name, pairs)
    for name, pairs in ENT.items():
        p04_ref_bz._patch(pathlib.Path(out) / name, pairs)
    (pathlib.Path(out) / 'r_ent.c').write_bytes(R_ENT_C.encode('utf-8'))
    return out


if __name__ == '__main__':
    print(make(sys.argv[1], sys.argv[2]))
