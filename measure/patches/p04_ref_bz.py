"""ref_bz: копия программного рендера (port/ref_soft) с врезками переднего края «бомж-3D».

make(src, out) копирует src (port/ref_soft) в out (build/port/gen/ref_bz) и вставляет врезки.
Исходники port/ref_soft не меняются: ref_bz — производный файл, собирается build.py --port.

Что вставляется (подробно — gpu/front/bz_front.h):
  r_main.c   — кадр дважды: эталон id, потом второй проход bz_pass = 1; в R_EdgeDrawing вместо
               R_ScanEdges — BZ_EndWorld (небо после мира);
  r_rast.c   — R_RenderFace: грань → BZ_RenderFace;
  r_bsp.c    — модели-кисти без отсечения по BSP мира (у адаптера z-буфер);
  r_polyse.c — треугольник модели → BZ_AliasTriangle;
  r_alias.c  — начало и конец модели (пачка треугольников VBUF/STBUF/TRIS);
  r_part.c   — частица → BZ_Particle;
  r_poly.c   — прозрачные грани → BZ_AlphaSurfaces, спрайты и лучи → BZ_DrawPoly;
  r_edge.c   — BZ_CalcGradients (miplevel там static);
  r_draw.c   — 2D дублируется командами.
Концы строк каждого файла сохраняются (правка байтами).
"""
import pathlib
import shutil

INC = '#include "r_local.h"\n'
INC_BZ = '#include "r_local.h"\n#include "bz_front.h"\n'

PATCHES = {
    'r_main.c': [
        (INC, INC_BZ),
        ('\tR_PushDlights (r_worldmodel);\n\n\tR_EdgeDrawing ();\n',
         '\tR_PushDlights (r_worldmodel);\n\n'
         '\tif (bz_pass)\n\t\tBZ_Begin3D ();\t\t// ref_bz: очистка z, ножницы, фаза волны\n\n'
         '\tR_EdgeDrawing ();\n'),
        ('\tR_ScanEdges ();\n}',
         '\tif (bz_pass)\n\t\tBZ_EndWorld ();\t\t// ref_bz: рёбер нет, мир уже ушёл командами; теперь небо\n'
         '\telse\n\t\tR_ScanEdges ();\n}'),
        ('void R_RenderFrame (refdef_t *fd)\n{', 'static void R_RenderFrame1 (refdef_t *fd)\n{'),
        ('/*\n** R_InitGraphics\n*/',
         '/*\n'
         '** R_RenderFrame (ref_bz): кадр рисуется дважды. Первый проход — программный рендер id без\n'
         '** изменений, это эталон. Второй (bz_pass = 1) — те же функции id, но врезки отдают\n'
         '** многоугольники переднему краю адаптера (bz_front.c). Что второй проход случайно нарисовал\n'
         '** в кадре id (подводная волна), затирается сохранённым эталоном.\n'
         '*/\n'
         'void R_RenderFrame (refdef_t *fd)\n{\n'
         '\tR_RenderFrame1 (fd);\n'
         '\tif (!BZ_Enabled ())\n\t\treturn;\n'
         '\tBZ_SaveSoft ();\n'
         '\tbz_pass = 1;\n'
         '\tR_RenderFrame1 (fd);\n'
         '\tbz_pass = 0;\n'
         '\tBZ_End3D ();\n'
         '\tBZ_RestoreSoft ();\n}\n\n'
         '/*\n** R_InitGraphics\n*/'),
    ],
    'r_rast.c': [
        (INC, INC_BZ),
        ('R_EmitSkyBox ();\t\n\t\treturn;\n\t}\n\n// skip out if no more surfs\n',
         'R_EmitSkyBox ();\t\n\t\treturn;\n\t}\n\n'
         '\tif (bz_pass)\n\t{\n\t\tBZ_RenderFace (fa, clipflags);\t// ref_bz: грань — в команды адаптера\n'
         '\t\treturn;\n\t}\n\n// skip out if no more surfs\n'),
    ],
    'r_bsp.c': [
        (INC, INC_BZ),
        ('\tmedge_t\t\t*pedge, *pedges;\n\n// FIXME: use bounding-box-based frustum clipping info?\n\n'
         '\tpsurf = &pmodel->surfaces[pmodel->firstmodelsurface];',
         '\tmedge_t\t\t*pedge, *pedges;\n\n'
         '\tif (bz_pass)\n\t{\n'
         '\t\t// ref_bz: у адаптера z-буфер — модель-кисть не режется по BSP мира\n'
         '\t\tR_DrawSubmodelPolygons (pmodel, r_clipflags, topnode);\n\t\treturn;\n\t}\n\n'
         '// FIXME: use bounding-box-based frustum clipping info?\n\n'
         '\tpsurf = &pmodel->surfaces[pmodel->firstmodelsurface];'),
    ],
    'r_polyse.c': [
        (INC, INC_BZ),
        ('void R_DrawTriangle( void )\n{\n\tspanpackage_t spans[DPS_MAXSPANS];\n\n'
         '\tint dv1_ab, dv0_ac;\n\tint dv0_ab, dv1_ac;\n',
         'void R_PolysetDrawSpansConstant8_33 (spanpackage_t *pspanpackage);\n'
         'void R_PolysetDrawSpansConstant8_66 (spanpackage_t *pspanpackage);\n\n'
         'void R_DrawTriangle( void )\n{\n\tspanpackage_t spans[DPS_MAXSPANS];\n\n'
         '\tint dv1_ab, dv0_ac;\n\tint dv0_ab, dv1_ac;\n\n'
         '\tif (bz_pass)\n\t{\n'
         '\t\t// ref_bz: вид закраски — по выбранной id функции отрезков\n'
         '\t\tBZ_AliasTriangle (d_pdrawspans == R_PolysetDrawSpans8_33 ? 1 :\n'
         '\t\t                  d_pdrawspans == R_PolysetDrawSpans8_66 ? 2 :\n'
         '\t\t                  d_pdrawspans == R_PolysetDrawSpansConstant8_33 ? 3 :\n'
         '\t\t                  d_pdrawspans == R_PolysetDrawSpansConstant8_66 ? 4 : 0);\n'
         '\t\treturn;\n\t}\n'),
    ],
    'r_alias.c': [
        (INC, INC_BZ),
        ('aliasbatchedtransformdata.this_verts );\n',
         'aliasbatchedtransformdata.this_verts );\n\n'
         '\tif (bz_pass)\t\t// ref_bz: вершины модели уже в экране — начать пачку треугольников\n'
         '\t\tBZ_AliasModelBegin (pfinalverts, s_pmdl->num_xyz);\n'),
        ('\tr_affinetridesc.pskin = pskindesc->pixels[0];\n',
         '\tr_affinetridesc.pskin = pskindesc->pixels[0];\n'
         '\tbz_alias_skin = pskindesc;\t\t// ref_bz: шкурка для таблицы текстур ядра (по картинке, не по точкам)\n'),
        ('R_AliasClipTriangle (pfv[0], pfv[1], pfv[2]);\n\t\t\t}\n\t\t}\n\t}\n}',
         'R_AliasClipTriangle (pfv[0], pfv[1], pfv[2]);\n\t\t\t}\n\t\t}\n\t}\n'
         '\tif (bz_pass)\t\t// ref_bz: модель кончилась — отправить пачку (pfinalverts ещё на стеке)\n'
         '\t\tBZ_AliasModelEnd ();\n}'),
    ],
    'r_part.c': [
        (INC, INC_BZ),
        ('\telse if (pix > d_pix_max)\n\t\tpix = d_pix_max;\n',
         '\telse if (pix > d_pix_max)\n\t\tpix = d_pix_max;\n\n'
         '\tif (bz_pass)\n\t{\n\t\tBZ_Particle (u, v, zi, pix, color, level);\t// ref_bz\n\t\treturn;\n\t}\n'),
    ],
    'r_poly.c': [
        (INC, INC_BZ),
        ('void R_ClipAndDrawPoly( float alpha, qboolean isturbulent, qboolean textured )\n{\n'
         '\temitpoint_t\toutverts[MAXWORKINGVERTS+3], *pout;\n\tfloat\t\t*pv;\n\tint\t\t\ti, nump;\n'
         '\tfloat\t\tscale;\n\tvec3_t\t\ttransformed, local;\n\n',
         'void R_ClipAndDrawPoly( float alpha, qboolean isturbulent, qboolean textured )\n{\n'
         '\temitpoint_t\toutverts[MAXWORKINGVERTS+3], *pout;\n\tfloat\t\t*pv;\n\tint\t\t\ti, nump;\n'
         '\tfloat\t\tscale;\n\tvec3_t\t\ttransformed, local;\n\n'
         '\tbz_poly_alpha = alpha;\t\t// ref_bz: вид закраски для BZ_DrawPoly\n'
         '\tbz_poly_textured = textured;\n\n'),
        ('static void R_DrawPoly( qboolean iswater )\n{\n\tint\t\t\ti, nump;\n\tfloat\t\tymin, ymax;\n'
         '\temitpoint_t\t*pverts;\n\tespan_t\tspans[MAXHEIGHT+1];\n',
         'static void R_DrawPoly( qboolean iswater )\n{\n\tint\t\t\ti, nump;\n\tfloat\t\tymin, ymax;\n'
         '\temitpoint_t\t*pverts;\n\tespan_t\tspans[MAXHEIGHT+1];\n\n'
         '\tif (bz_pass)\n\t{\n\t\tBZ_DrawPoly (iswater, r_polyblendcolor);\t// ref_bz: спрайт или луч\n'
         '\t\treturn;\n\t}\n'),
        ('void R_DrawAlphaSurfaces( void )\n{\n\tmsurface_t *s = r_alpha_surfaces;\n\n',
         'void R_DrawAlphaSurfaces( void )\n{\n\tmsurface_t *s = r_alpha_surfaces;\n\n'
         '\tif (bz_pass)\n\t{\n\t\tBZ_AlphaSurfaces ();\t// ref_bz: прозрачные грани своим путём\n'
         '\t\treturn;\n\t}\n\n'),
    ],
    'r_draw.c': [
        (INC, INC_BZ),
        ('\tnum &= 255;\n\n\tif (num == 32 || num == 32+128)',
         '\tBZ_DrawChar (x, y, num, draw_chars);\t// ref_bz: 2D командой адаптера\n\n'
         '\tnum &= 255;\n\n\tif (num == 32 || num == 32+128)'),
        ('\tint\t\t\t\tskip;\n\n\tif ((x < 0) ||\n\t\t(x + w > vid.width) ||',
         '\tint\t\t\t\tskip;\n\n\tBZ_StretchPic (x, y, w, h, pic);\t// ref_bz\n\n'
         '\tif ((x < 0) ||\n\t\t(x + w > vid.width) ||'),
        ('\tDraw_StretchPicImplementation (x, y, w, h, &pic);\n}',
         '\tbz_raw = 1;\t\t// ref_bz: кадр кинематики грузится заново каждый раз\n'
         '\tDraw_StretchPicImplementation (x, y, w, h, &pic);\n\tbz_raw = 0;\n}'),
        ('\t\treturn;\t//\tri.Sys_Error (ERR_FATAL,"Draw_Pic: bad coordinates");\n',
         '\t\treturn;\t//\tri.Sys_Error (ERR_FATAL,"Draw_Pic: bad coordinates");\n\n'
         '\tBZ_DrawPic (x, y, pic);\t// ref_bz\n'),
        ('\tx2 = x + w;\n', '\tBZ_TileClear (x, y, w, h, pic);\t// ref_bz\n\n\tx2 = x + w;\n'),
        ('\tif (w < 0 || h < 0)\n\t\treturn;\n\tdest = vid.buffer + y*vid.rowbytes + x;',
         '\tif (w < 0 || h < 0)\n\t\treturn;\n\tBZ_Fill (x, y, w, h, c);\t// ref_bz\n'
         '\tdest = vid.buffer + y*vid.rowbytes + x;'),
        ('void Draw_FadeScreen (void)\n{\n\tint\t\t\tx,y;\n\tbyte\t\t*pbuf;\n\tint\tt;\n\n',
         'void Draw_FadeScreen (void)\n{\n\tint\t\t\tx,y;\n\tbyte\t\t*pbuf;\n\tint\tt;\n\n'
         '\tBZ_FadeScreen ();\t// ref_bz\n\n'),
    ],
    # Текстуры только в SDRAM ядра («диета», шаг 3, 24.11; включает BZ_TEX_RESIDENT — q2esp и Q2.ELF): картинка
    # опознаётся по своему номеру (bz_id), а не по адресу точек — точки после загрузки в ядро отдаются, и
    # по их адресу могла бы лечь другая картинка. Шкурка модели и картинка спрайта запоминаются тут же, где id
    # берёт их точки. Без BZ_TEX_RESIDENT (ref_bz) точки остаются, как у id.
    'r_local.h': [
        ('\tint             registration_sequence;  // 0 = free\n\tbyte\t\t*pixels[4];\t\t\t\t// mip levels\n} image_t;\n',
         '\tint             registration_sequence;  // 0 = free\n\tbyte\t\t*pixels[4];\t\t\t\t// mip levels\n'
         '\tint\t\tbz_id;\t\t\t\t\t// ref_bz: номер картинки для таблицы текстур ядра (0 — ещё не выдан)\n'
         '\tint\t\tbz_mips;\t\t\t\t// ref_bz: сколько мип-уровней было, когда точки отдали (BZ_TEX_RESIDENT)\n'
         '} image_t;\n'),
    ],
    'r_image.c': [
        (INC, INC_BZ),
        # точек может не быть (отданы ядру): id «подкачивал» их чтением памяти
        ('\t\t\tCom_PageInMemory ((byte *)image->pixels[0], image->width*image->height);\n',
         '\t\t\tif (image->pixels[0])\t// ref_bz: точки могли отдать ядру (BZ_TEX_RESIDENT)\n'
         '\t\t\t\tCom_PageInMemory ((byte *)image->pixels[0], image->width*image->height);\n'),
        ('\tif (pic)\n\t\tfree(pic);\n\tif (palette)\n\t\tfree(palette);\n\n\treturn image;\n}\n',
         '\tif (pic)\n\t\tfree(pic);\n\tif (palette)\n\t\tfree(palette);\n\n'
         '#ifdef BZ_TEX_RESIDENT\n'
         '\tif (image)\n'
         '\t\tBZ_ImageLoaded (image);\t// ref_bz: картинка — в SDRAM ядра, точки на ESP — прочь\n'
         '#endif\n'
         '\treturn image;\n}\n'
         '\n'
         '/*\n'
         '===============\n'
         'R_EspReloadPixels\n'
         '\n'
         'ref_bz (BZ_TEX_RESIDENT): точки мип-уровня mip картинки заново из её файла — в новый буфер (malloc;\n'
         'освобождать free). Нужны, когда ядру текстура понадобилась ещё раз (таблица текстур сброшена, другой\n'
         'вид — подложка строками/плитками), а свои точки ESP отдала после загрузки. NULL — файла нет или он\n'
         'другой (размер не тот).\n'
         '===============\n'
         '*/\n'
         'byte *R_EspReloadPixels (image_t *image, int mip)\n'
         '{\n'
         '\tint\t\tlen = (int)strlen (image->name);\n'
         '\tint\t\tw = image->width >> mip, h = image->height >> mip;\n'
         '\tbyte\t*out = NULL;\n'
         '\n'
         '\tif (len > 4 && !strcmp (image->name + len - 4, ".wal"))\n'
         '\t{\n'
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
         '\t\tri.FS_FreeFile (mt);\n'
         '\t}\n'
         '\telse if (len > 4 && !strcmp (image->name + len - 4, ".pcx") && mip == 0)\n'
         '\t{\n'
         '\t\tint\t\tpw, ph;\n'
         '\n'
         '\t\tLoadPCX (image->name, &out, NULL, &pw, &ph);\n'
         '\t\tif (out && (pw != image->width || ph != image->height))\n'
         '\t\t{\n'
         '\t\t\tfree (out);\n'
         '\t\t\tout = NULL;\n'
         '\t\t}\n'
         '\t}\n'
         '\treturn out;\n'
         '}\n'),
    ],
    'r_sprite.c': [
        (INC, INC_BZ),
        ('\tr_polydesc.pixels       = currentmodel->skins[currententity->frame]->pixels[0];\n',
         '\tr_polydesc.pixels       = currentmodel->skins[currententity->frame]->pixels[0];\n'
         '\tbz_poly_image = currentmodel->skins[currententity->frame];\t// ref_bz: картинка для BZ_DrawPoly\n'),
    ],
}

APPEND = {
    'r_edge.c':
        '\n/*\n** ref_bz: D_CalcGradients для заданного mip (miplevel в этом файле static) — зовёт\n'
        '** передний край адаптера bz_front.c.\n*/\n'
        'void BZ_CalcGradients (msurface_t *pface, int mip)\n{\n'
        '\tmiplevel = mip;\n\tD_CalcGradients (pface);\n}\n',
}


def _patch(path, pairs, append=''):
    """Правка байтами: текст id остаётся байт в байт, врезки (с русскими комментариями) — в UTF-8
    (компилятор собирает с /utf-8)."""
    raw = path.read_bytes()
    crlf = b'\r\n' in raw
    t = raw.replace(b'\r\n', b'\n')
    for old, new in pairs:
        ob, nb = old.encode('utf-8'), new.encode('utf-8')
        n = t.count(ob)
        assert n == 1, f'{path.name}: врезка найдена {n} раз: {old[:60]!r}'
        t = t.replace(ob, nb)
    t += append.encode('utf-8')
    path.write_bytes(t.replace(b'\n', b'\r\n') if crlf else t)


def make(src, out):
    src, out = pathlib.Path(src), pathlib.Path(out)
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(src, out)
    for name, pairs in PATCHES.items():
        _patch(out / name, pairs, APPEND.get(name, ''))
    for name, text in APPEND.items():
        if name not in PATCHES:
            _patch(out / name, [], text)
    return out


if __name__ == '__main__':
    import sys
    print(make(sys.argv[1], sys.argv[2]))
