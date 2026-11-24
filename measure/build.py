"""Сборка замерочного Quake 2 для VDAC4 (MSVC, 32 бита, без ассемблера id: C_ONLY).

python build.py          — исходники id без правок (эталон):
  1. Копирует исходники id (E:\\zx\\Quake2\\id-source, не меняются) в build\\src.
  2. Вставляет в копию счётчики времени (q2m.h) и метки видов рисования (q2m_ref.h),
     дописывает функции сведений о памяти в common.c и cmodel.c.
  3. Делает build\\gen\\qgl_cnt.c: все функции OpenGL — пустые заглушки (из qgl_win.c).
  4. Собирает build\\bin: q2m.exe, gamex86.dll, ref_cnt.dll (считающий GL), ref_soft.dll.
python build.py --port   — наш движок E:\\zx\\Quake2\\port (в нём счётчики уже стоят, ничего не
  вставляется): build\\port\\gen, build\\port\\bin. Кроме DLL здесь же ref_bz.dll (эталон id + команды
  «бомж-3D») и q2esp.exe — движок, игра и рендер ref_esp (только команды адаптера) одной программой,
  как будет на ESP; запуск — как q2m.exe, без vid_ref.
Временные файлы компилятора — в E:\\zx\\cache\\tmp\\quake2.
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(r'E:\zx\Quake2')
ID = ROOT / 'id-source'
MEASURE = ROOT / 'measure'
GPU = ROOT / 'gpu'
BUILD = ROOT / 'build'
SRC = BUILD / 'src'
GEN = BUILD / 'gen'
BIN = BUILD / 'bin'
OBJ = BUILD / 'obj'
TMP = pathlib.Path(r'E:\zx\cache\tmp\quake2')
VCVARS = pathlib.Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat')


def patch(rel, pairs, append=''):
    """Заменить куски текста (каждый ровно один раз), сохранив концы строк файла."""
    path = SRC / rel
    raw = path.read_bytes()
    crlf = b'\r\n' in raw
    t = raw.decode('latin-1').replace('\r\n', '\n')
    for old, new in pairs:
        n = t.count(old)
        if n != 1:
            sys.exit(f'{rel}: «{old.strip()[:60]}» найдено {n} раз')
        t = t.replace(old, new)
    nl = '\r\n' if crlf else '\n'
    # исходник id — однобайтный (latin-1), дописанное — UTF-8 (русские комментарии)
    path.write_bytes(t.replace('\n', nl).encode('latin-1') + append.replace('\n', nl).encode('utf-8'))


ZONE_STATS = r'''
/* q2m: память зоны по меткам (Z_TagMalloc) — для сведений о карте */
#include <stdio.h>
void Q2M_ZoneStats (FILE *f)
{
	int		tags[64], bytes[64], counts[64], n = 0, i;
	zhead_t	*z;

	for (z = z_chain.next ; z != &z_chain ; z = z->next)
	{
		for (i = 0; i < n && tags[i] != z->tag; i++)
			;
		if (i == n && n < 64)
		{
			tags[n] = z->tag; bytes[n] = 0; counts[n] = 0; n++;
		}
		if (i < n)
		{
			bytes[i] += z->size;
			counts[i]++;
		}
	}
	fprintf (f, "\"zone_bytes\":%d,\"zone_count\":%d,\"zone_tags\":{", z_bytes, z_count);
	for (i = 0; i < n; i++)
		fprintf (f, "%s\"%d\":[%d,%d]", i ? "," : "", tags[i], bytes[i], counts[i]);
	fprintf (f, "}");
}
'''

CMODEL_STATS = r'''
/* q2m: модель столкновений карты — сколько чего и сколько байт в памяти */
#include <stdio.h>
void Q2M_CmodelStats (FILE *f)
{
	fprintf (f, "\"cmodel\":{\"planes\":[%d,%d],\"nodes\":[%d,%d],\"leafs\":[%d,%d],"
		"\"leafbrushes\":[%d,%d],\"brushes\":[%d,%d],\"brushsides\":[%d,%d],"
		"\"texinfo\":[%d,%d],\"cmodels\":[%d,%d],\"visibility\":[%d,1],\"entitychars\":[%d,1],"
		"\"areas\":[%d,%d],\"areaportals\":[%d,%d],\"clusters\":%d}",
		numplanes, (int)sizeof(cplane_t), numnodes, (int)sizeof(cnode_t),
		numleafs, (int)sizeof(cleaf_t), numleafbrushes, (int)sizeof(unsigned short),
		numbrushes, (int)sizeof(cbrush_t), numbrushsides, (int)sizeof(cbrushside_t),
		numtexinfo, (int)sizeof(mapsurface_t), numcmodels, (int)sizeof(cmodel_t),
		numvisibility, numentitychars, numareas, (int)sizeof(carea_t),
		numareaportals, (int)sizeof(dareaportal_t), numclusters);
}
'''


EDICTS_COUNT = r'''
/* q2m: сколько объектов игры занято сейчас (ge->num_edicts) — замеры перед 512 вместо 1024 */
int Q2M_NumEdicts (void)
{
	return ge ? ge->num_edicts : 0;
}
'''


def prepare():
    if SRC.exists():
        shutil.rmtree(SRC)
    shutil.copytree(ID, SRC, ignore=shutil.ignore_patterns('.git'))
    GEN.mkdir(parents=True, exist_ok=True)

    # --- движок: время частей кадра
    patch('qcommon/common.c', [
        ('\tSV_Frame (msec);\n', '\tQ2M_BEGIN(Q2M_SV); SV_Frame (msec); Q2M_END(Q2M_SV);\n'),
        ('\tCL_Frame (msec);\n', '\tQ2M_BEGIN(Q2M_CL); CL_Frame (msec); Q2M_END(Q2M_CL);\n'),
    ], ZONE_STATS)
    patch('qcommon/cmodel.c', [], CMODEL_STATS)
    patch('server/sv_main.c', [
        ('\t\tge->RunFrame ();\n', '\t\tQ2M_BEGIN(Q2M_GAME); ge->RunFrame (); Q2M_END(Q2M_GAME);\n'),
    ])
    patch('server/sv_game.c', [], EDICTS_COUNT)
    patch('client/cl_main.c', [
        ('\tSCR_UpdateScreen ();\n\tif (host_speeds->value)\n\t\ttime_after_ref',
         '\tQ2M_BEGIN(Q2M_SCR); SCR_UpdateScreen (); Q2M_END(Q2M_SCR);\n\tif (host_speeds->value)\n\t\ttime_after_ref'),
        ('\tS_Update (cl.refdef.vieworg, cl.v_forward, cl.v_right, cl.v_up);\n',
         '\tQ2M_BEGIN(Q2M_SND); S_Update (cl.refdef.vieworg, cl.v_forward, cl.v_right, cl.v_up); Q2M_END(Q2M_SND);\n'),
    ])
    patch('client/cl_view.c', [
        ('\tre.RenderFrame (&cl.refdef);\n', '\tQ2M_BEGIN(Q2M_RF); re.RenderFrame (&cl.refdef); Q2M_END(Q2M_RF);\n'),
    ])
    # время игры даёт sys_q2m.c (виртуальные часы); настоящее — под другим именем
    eng = (SRC / 'win32/q_shwin.c').read_bytes().decode('latin-1')
    old = 'int\tcurtime;\r\nint Sys_Milliseconds (void)'
    if eng.count(old) != 1:
        sys.exit('q_shwin.c: не найден Sys_Milliseconds')
    (SRC / 'win32/q_shwin_eng.c').write_bytes(
        eng.replace(old, 'static int\tcurtime;\r\nint Sys_Milliseconds_real (void)').encode('latin-1'))

    # --- считающий рендер: метки видов рисования
    patch('ref_gl/gl_rmain.c', [
        ('\tR_DrawWorld ();\n', '\tq2m_cat = Q2M_C_WORLD;\n\tR_DrawWorld ();\n'),
        ('\tR_DrawEntitiesOnList ();\n', '\tq2m_cat = Q2M_C_ALIAS;\n\tR_DrawEntitiesOnList ();\n'),
        ('\tR_RenderDlights ();\n', '\tq2m_cat = Q2M_C_BLEND;\n\tR_RenderDlights ();\n'),
        ('\tR_DrawParticles ();\n', '\tq2m_cat = Q2M_C_PART;\n\tR_DrawParticles ();\n'),
        ('\tR_DrawAlphaSurfaces ();\n', '\tq2m_cat = Q2M_C_ALPHA;\n\tR_DrawAlphaSurfaces ();\n'),
        ('\tR_Flash();\n', '\tq2m_cat = Q2M_C_BLEND;\n\tR_Flash();\n'),
        ('void\tR_SetGL2D (void)\n{\n', 'void\tR_SetGL2D (void)\n{\n\tq2m_cat = Q2M_C_2D;\n'),
        ('void R_DrawSpriteModel (entity_t *e)\n{\n', 'void R_DrawSpriteModel (entity_t *e)\n{\n\tq2m_cat = Q2M_C_SPRITE;\n'),
        ('void R_DrawNullModel (void)\n{\n', 'void R_DrawNullModel (void)\n{\n\tq2m_cat = Q2M_C_SPRITE;\n'),
        ('void R_Register( void )\n{\n', 'void R_Register( void )\n{\n\tQ2M_RefRegister ();\n'),
    ])
    patch('ref_gl/gl_mesh.c', [
        ('void R_DrawAliasModel (entity_t *e)\n{\n', 'void R_DrawAliasModel (entity_t *e)\n{\n\tq2m_cat = Q2M_C_ALIAS;\n'),
    ])
    patch('ref_gl/gl_rsurf.c', [
        ('void R_DrawBrushModel (entity_t *e)\n{\n', 'void R_DrawBrushModel (entity_t *e)\n{\n\tq2m_cat = Q2M_C_BMODEL;\n'),
        ('\t\tEmitWaterPolys (fa);\n',
         '\t\t{ int q2m_c = q2m_cat; q2m_cat = Q2M_C_WARP; EmitWaterPolys (fa); q2m_cat = q2m_c; }\n'),
        ('\t\t\tEmitWaterPolys (s);\n',
         '\t\t\t{ int q2m_c = q2m_cat; q2m_cat = Q2M_C_WARP; EmitWaterPolys (s); q2m_cat = q2m_c; }\n'),
        ('\tR_DrawSkyBox ();\n', '\tq2m_cat = Q2M_C_SKY;\n\tR_DrawSkyBox ();\n\tq2m_cat = Q2M_C_WORLD;\n'),
    ])
    gen_qgl()


def gen_qgl():
    """qgl_cnt.c: указатели qgl* как в qgl_win.c, но на пустые заглушки, без opengl32.dll."""
    t = (SRC / 'win32/qgl_win.c').read_bytes().decode('latin-1').replace('\r\n', '\n')
    head = t[:t.index('static void ( APIENTRY * dllAccum )')]
    logs = re.findall(r'^static (.+?)\s*APIENTRY\s+log(\w+)\s*\(([^)]*)\)\s*$', t, re.M)
    out = ['/* Сделано measure/build.py из win32/qgl_win.c id: пустые функции GL для ref_cnt. */\n',
           head.replace('#include "glw_win.h"\n', ''), '\n']
    names = []
    for ret, name, params in logs:
        ret = ret.strip()
        body = '{ }' if ret == 'void' else '{ return 0; }'
        out.append(f'static {ret} APIENTRY cnt{name}({params}) {body}\n')
        names.append(name)
    out.append('\nqboolean QGL_Init (const char *dllname)\n{\n')
    for name in names:
        out.append(f'\tqgl{name} = cnt{name};\n')
    out.append('\tqglLockArraysEXT = 0;\n\tqglUnlockArraysEXT = 0;\n'
               '\tqglPointParameterfEXT = 0;\n\tqglPointParameterfvEXT = 0;\n'
               '\tqglColorTableEXT = 0;\n\tqglSelectTextureSGIS = 0;\n\tqglMTexCoord2fSGIS = 0;\n'
               '\tqwglSwapIntervalEXT = 0;\n'
               '\tQ2M_InstallGL ();\n\treturn true;\n}\n\n'
               'void QGL_Shutdown (void)\n{\n}\n')
    (GEN / 'qgl_cnt.c').write_text(''.join(out), encoding='utf-8', newline='\r\n')
    print(f'qgl_cnt.c: {len(names)} заглушек')


def files(d, names):
    return [str(SRC / d / n) for n in names.split()]


def build():
    for d in (BIN, OBJ, TMP):
        d.mkdir(parents=True, exist_ok=True)
    defs = ('/DC_ONLY /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE '
            '/D_WINSOCK_DEPRECATED_NO_WARNINGS')
    cflags = f'/nologo /O2 /MD /W1 /MP /GS- /utf-8 /wd4828 {defs}'

    game = sorted(str(p) for p in (SRC / 'game').glob('*.c'))
    ref_gl = files('ref_gl', 'gl_draw.c gl_image.c gl_light.c gl_mesh.c gl_model.c gl_rmain.c '
                             'gl_rmisc.c gl_rsurf.c gl_warp.c') + files('game', 'q_shared.c') + \
        files('win32', 'q_shwin.c') + [str(GEN / 'qgl_cnt.c'), str(MEASURE / 'ref_cnt/q2m_gl.c')]
    ref_soft = sorted(str(p) for p in (SRC / 'ref_soft').glob('*.c')) + files('game', 'q_shared.c') + \
        files('win32', 'q_shwin.c') + [str(MEASURE / 'ref_soft/swimp_q2m.c')]
    # ref_bz (только --port): копия ref_soft с врезками «бомж-3D» (patches/p04_ref_bz.py) +
    # передний край и C-модель адаптера из gpu
    ref_bz = []
    if (SRC / 'ref_soft').exists() and SRC == ROOT / 'port':
        sys.path.insert(0, str(MEASURE / 'patches'))
        import p04_ref_bz
        bzdir = p04_ref_bz.make(SRC / 'ref_soft', GEN / 'ref_bz')
        ref_bz = sorted(str(p) for p in bzdir.glob('*.c')) + files('game', 'q_shared.c') + \
            files('win32', 'q_shwin.c') + [str(GPU / 'front/bz_front.c'), str(GPU / 'front/swimp_bz.c'),
                                           str(GPU / 'front/bz_crash.c'),
                                           str(GPU / 'model/bomzh.c')]
    exe = files('client', 'cl_cin.c cl_ents.c cl_fx.c cl_input.c cl_inv.c cl_main.c cl_newfx.c '
                          'cl_parse.c cl_pred.c cl_scrn.c cl_tent.c cl_view.c console.c keys.c menu.c '
                          'qmenu.c snd_dma.c snd_mem.c snd_mix.c') + \
        files('qcommon', 'cmd.c cmodel.c common.c crc.c cvar.c files.c md4.c net_chan.c pmove.c') + \
        files('server', 'sv_ccmds.c sv_ents.c sv_game.c sv_init.c sv_main.c sv_send.c sv_user.c sv_world.c') + \
        files('game', 'q_shared.c m_flash.c') + files('win32', 'net_wins.c q_shwin_eng.c') + \
        files('null', 'cd_null.c in_null.c') + \
        [str(MEASURE / n) for n in ('sys_q2m.c', 'vid_q2m.c', 'snd_q2m.c')]

    def target(name, srcs, extra, link):
        o = OBJ / name
        o.mkdir(exist_ok=True)
        rsp = OBJ / f'{name}.rsp'
        rsp.write_text('\n'.join(f'"{s}"' for s in srcs), encoding='utf-8')
        return (f'cl {cflags} {extra} /Fo"{o}\\\\" @"{rsp}" /link /NOLOGO {link} > "{OBJ / name}.log" 2>&1 '
                f'|| (type "{OBJ / name}.log" & exit /b 1)')

    def compile_only(name, srcs, extra):
        """Только компиляция группы в OBJ/<name> (связывание — отдельным шагом)."""
        o = OBJ / name
        if o.exists():
            shutil.rmtree(o)        # старые .obj не должны попасть в связывание
        o.mkdir()
        rsp = OBJ / f'{name}.rsp'
        rsp.write_text('\n'.join(f'"{s}"' for s in srcs), encoding='utf-8')
        return (f'cl {cflags} /c {extra} /Fo"{o}\\\\" @"{rsp}" > "{OBJ / name}.log" 2>&1 '
                f'|| (type "{OBJ / name}.log" & exit /b 1)')

    # q2esp (только --port): движок, игра и рендер одной программой — как будет на ESP, где библиотек
    # DLL нет: GAME_HARD_LINKED и REF_HARD_LINKED (их предусмотрел id) убирают из игры и рендера
    # запасные Sys_Error/Com_Printf, sys_q2m.c и vid_q2m.c зовут GetGameAPI/GetRefAPI напрямую.
    # Рендер — ref_esp (patches/p06_ref_esp.py): второй проход ref_bz без программного рисования id,
    # bz_front.c с BZ_ONLY. Три группы со своими ключами компилируются отдельно, потом связываются.
    # q_shared.c, m_flash.c (есть и у игры) и q_shwin.c (у рендера) — по одной копии, в движке.
    esp_steps = []
    if ref_bz:
        import p06_ref_esp
        espdir = p06_ref_esp.make(SRC / 'ref_soft', GEN / 'ref_esp')
        # «Диета» памяти ESP: MAX_PARTICLES 3072 вместо 4096 (−41 КБ) — с 2048 в demo2 полной игры частицы
        # терялись (792 кадра), с 3072 demo1, demo2 и бои base1/city3 = ref_bz (4096) кадр в кадр;
        # CON_TEXTSIZE 8192 вместо 32768 (160 строк истории консоли, −24 КБ и −24 КБ стека)
        hard = '/DGAME_HARD_LINKED /DREF_HARD_LINKED /DMAX_PARTICLES=3072 /DCON_TEXTSIZE=8192'
        # Одноимённые глобальные переменные (tools/symclash.py): у игры dedicated и maxclients, у рендера
        # vid_fullscreen и vid_gamma — такие же есть у движка. Это cvar, которые все берут по одному имени
        # (указатель один и тот же), но MSVC сливает их молча, а GCC на ESP не свяжет вовсе. Поэтому у
        # игры и рендера они переименованы ключами сборки (во всех файлах группы одинаково).
        game_ren = '/Ddedicated=game_dedicated /Dmaxclients=game_maxclients'
        ref_ren = '/Dvid_fullscreen=r_vid_fullscreen /Dvid_gamma=r_vid_gamma'
        groups = [
            ('q2esp_eng', exe, f'{hard} /Zi /Fd"{OBJ / "q2esp_eng"}\\\\" /FI"{MEASURE / "q2m.h"}" '
                               f'/I"{MEASURE}" /I"{SRC / "qcommon"}"'),
            ('q2esp_game', [g for g in game if pathlib.Path(g).name not in ('q_shared.c', 'm_flash.c')],
             f'{hard} {game_ren} /Zi /Fd"{OBJ / "q2esp_game"}\\\\"'),
            ('q2esp_ref', sorted(str(p) for p in espdir.glob('*.c')) +
             [str(GPU / 'front/bz_front.c'), str(GPU / 'front/swimp_bz.c'), str(GPU / 'front/bz_crash.c'),
              str(GPU / 'model/bomzh.c')],
             f'{hard} {ref_ren} /DBZ_ONLY /DBZ_TEX_RESIDENT /Zi /Fd"{OBJ / "q2esp_ref"}\\\\" /I"{espdir}" /I"{SRC / "ref_soft"}" '
             f'/I"{GPU / "front"}" /I"{GPU / "model"}"'),
        ]
        esp_steps = [compile_only(n, s, e) for n, s, e in groups] + [
            f'link /NOLOGO /DEBUG /MAP:"{OBJ / "q2esp.map"}" /OUT:"{BIN / "q2esp.exe"}" ' +
            ' '.join(f'"{OBJ / n}\\*.obj"' for n, _, _ in groups) +
            f' ws2_32.lib wsock32.lib winmm.lib user32.lib advapi32.lib dbghelp.lib > "{OBJ / "q2esp.log"}" 2>&1 '
            f'|| (type "{OBJ / "q2esp.log"}" & exit /b 1)']

    steps = [
        target('gamex86', game, '/LD', f'/DEF:"{SRC / "game/game.def"}" /OUT:"{BIN / "gamex86.dll"}"'),
        target('ref_cnt', ref_gl, f'/LD /FI"{MEASURE / "q2m_ref.h"}" /I"{SRC / "ref_gl"}"',
               f'/DEF:"{SRC / "ref_gl/ref_gl.def"}" /OUT:"{BIN / "ref_cnt.dll"}" user32.lib winmm.lib'),
        target('ref_soft', ref_soft, f'/LD /I"{SRC / "ref_soft"}"',
               f'/DEF:"{SRC / "ref_soft/ref_soft.def"}" /OUT:"{BIN / "ref_soft.dll"}" user32.lib winmm.lib'),
    ] + ([
        target('ref_bz', ref_bz, f'/LD /Zi /Fd"{OBJ / "ref_bz"}\\\\" /I"{GEN / "ref_bz"}" /I"{SRC / "ref_soft"}" /I"{GPU / "front"}" /I"{GPU / "model"}"',
               f'/DEF:"{GEN / "ref_bz/ref_soft.def"}" /OUT:"{BIN / "ref_bz.dll"}" /DEBUG user32.lib winmm.lib dbghelp.lib'),
    ] if ref_bz else []) + [
        target('q2m', exe, f'/FI"{MEASURE / "q2m.h"}" /I"{MEASURE}" /I"{SRC / "qcommon"}"',
               f'/OUT:"{BIN / "q2m.exe"}" ws2_32.lib wsock32.lib winmm.lib user32.lib advapi32.lib'),
    ] + esp_steps
    bat = OBJ / 'build.bat'
    bat.write_text('\r\n'.join(['@echo off', f'set TMP={TMP}', f'set TEMP={TMP}',
                                f'call "{VCVARS}" >nul || exit /b 1'] + steps + ['echo OK']) + '\r\n',
                   encoding='utf-8')
    r = subprocess.run(['cmd.exe', '/d', '/c', str(bat)], capture_output=True)
    out = (r.stdout + r.stderr).decode('cp866', 'replace')
    lines = [ln for ln in out.splitlines() if 'error' in ln.lower() or 'OK' == ln.strip()
             or 'unresolved' in ln.lower()]
    print('\n'.join(lines[-60:]) if lines else out[-4000:])
    return r.returncode


if __name__ == '__main__':
    os.chdir(ROOT)
    if '--port' in sys.argv:
        # наш движок: собирается прямо из port\, вывод — в build\port
        SRC = ROOT / 'port'
        GEN = BUILD / 'port/gen'
        BIN = BUILD / 'port/bin'
        OBJ = BUILD / 'port/obj'
        GEN.mkdir(parents=True, exist_ok=True)
        gen_qgl()
    else:
        prepare()
    sys.exit(build())
