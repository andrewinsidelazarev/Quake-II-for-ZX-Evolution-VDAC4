"""Порт VDAC4, этап 1 («диета» памяти), шаг 1: таблицы модели столкновений — по размеру карты.

python p01_cmodel_tables.py   (правит E:\\zx\\Quake2\\port; повторно не запускать — проверки найдут, что уже правлено)

У id таблицы карты в qcommon/cmodel.c — статические массивы на максимум (5,75 МБ): на ESP это
.bss во внутренней памяти и PSRAM, которых нет. Теперь CM_LoadMap выделяет их по длинам lump-ов
(Z_TagMalloc с меткой CM_TAG) и освобождает при следующей карте.

Ловушка: клиент держит cl.model_clip[] — указатели в map_cmodels прежней карты — и между загрузкой
карты сервером и своим CL_PrepRefresh ещё кадр предсказывает движение (cl_pred.c). У id массивы
статические, и указатели оставались в живой памяти. Теперь у карты есть поколение (CM_Generation):
клиент запоминает его вместе с model_clip и чужого поколения не берёт.

Исходники id — ASCII, вставки — русские комментарии: файлы пишутся в UTF-8 (сборка MSVC с /utf-8,
GCC Xtensa — UTF-8 по умолчанию). Концы строк файла сохраняются.
"""
import pathlib

PORT = pathlib.Path(r'E:\zx\Quake2\port')


def patch(rel, pairs, count=1):
    path = PORT / rel
    raw = path.read_bytes()
    crlf = b'\r\n' in raw
    t = raw.decode('utf-8').replace('\r\n', '\n')
    for old, new in pairs:
        n = t.count(old)
        assert n == count, (rel, n, old[:80])
        t = t.replace(old, new)
    path.write_bytes(t.replace('\n', '\r\n' if crlf else '\n').encode('utf-8'))
    print('patched', rel)


DECL_OLD = '''int			numbrushsides;
cbrushside_t map_brushsides[MAX_MAP_BRUSHSIDES];

int			numtexinfo;
mapsurface_t	map_surfaces[MAX_MAP_TEXINFO];

int			numplanes;
cplane_t	map_planes[MAX_MAP_PLANES+6];		// extra for box hull

int			numnodes;
cnode_t		map_nodes[MAX_MAP_NODES+6];		// extra for box hull

int			numleafs = 1;	// allow leaf funcs to be called without a map
cleaf_t		map_leafs[MAX_MAP_LEAFS];
int			emptyleaf, solidleaf;

int			numleafbrushes;
unsigned short	map_leafbrushes[MAX_MAP_LEAFBRUSHES];

int			numcmodels;
cmodel_t	map_cmodels[MAX_MAP_MODELS];

int			numbrushes;
cbrush_t	map_brushes[MAX_MAP_BRUSHES];

int			numvisibility;
byte		map_visibility[MAX_MAP_VISIBILITY];
dvis_t		*map_vis = (dvis_t *)map_visibility;

int			numentitychars;
char		map_entitystring[MAX_MAP_ENTSTRING];

int			numareas = 1;
carea_t		map_areas[MAX_MAP_AREAS];

int			numareaportals;
dareaportal_t map_areaportals[MAX_MAP_AREAPORTALS];
'''

DECL_NEW = '''// Порт VDAC4: таблицы карты — по размеру карты, а не статические на максимум (у id — 5,75 МБ .bss).
// CM_LoadMap выделяет их Z_TagMalloc с меткой CM_TAG по длинам lump-ов (+ место «коробки»
// CM_InitBoxHull) и освобождает при следующей карте. Пока карты нет (и у пустого имени — сервер
// кинематики) — крошечные запасные таблицы none_*, их хватает и на «коробку».
#define	CM_TAG	767

static cbrushside_t	none_brushsides[6];
static mapsurface_t	none_surfaces[1];
static cplane_t		none_planes[12];
static cnode_t		none_nodes[6];
static cleaf_t		none_leafs[2];
static unsigned short	none_leafbrushes[1];
static cmodel_t		none_cmodels[1];
static cbrush_t		none_brushes[1];
static int			none_visibility[sizeof(dvis_t) / sizeof(int) + 1];
static char			none_entitystring[1];
static carea_t		none_areas[1];
static dareaportal_t	none_areaportals[1];

int			numbrushsides;
cbrushside_t *map_brushsides = none_brushsides;

int			numtexinfo;
mapsurface_t	*map_surfaces = none_surfaces;

int			numplanes;
cplane_t	*map_planes = none_planes;		// +12 плоскостей «коробки»

int			numnodes;
cnode_t		*map_nodes = none_nodes;		// +6 узлов «коробки»

int			numleafs = 1;	// allow leaf funcs to be called without a map
cleaf_t		*map_leafs = none_leafs;
int			emptyleaf, solidleaf;

int			numleafbrushes;
unsigned short	*map_leafbrushes = none_leafbrushes;

int			numcmodels;
cmodel_t	*map_cmodels = none_cmodels;

int			numbrushes;
cbrush_t	*map_brushes = none_brushes;

int			numvisibility;
byte		*map_visibility = (byte *)none_visibility;
dvis_t		*map_vis = (dvis_t *)none_visibility;

int			numentitychars;
char		*map_entitystring = none_entitystring;

int			numareas = 1;
carea_t		*map_areas = none_areas;

int			numareaportals;
dareaportal_t *map_areaportals = none_areaportals;
'''

FUNCS = '''/*
==================
Порт VDAC4: таблицы карты по её размеру
==================
*/
static int	cm_generation;		// растёт с каждой новой загруженной картой (CM_Generation)

int CM_Generation (void)
{
	return cm_generation;
}

// Назад к запасным таблицам none_*: таблицы прежней карты освобождаются
static void CM_FreeTables (void)
{
	Z_FreeTags (CM_TAG);
	map_brushsides = none_brushsides;
	map_surfaces = none_surfaces;
	map_planes = none_planes;
	map_nodes = none_nodes;
	map_leafs = none_leafs;
	map_leafbrushes = none_leafbrushes;
	map_cmodels = none_cmodels;
	map_brushes = none_brushes;
	map_visibility = (byte *)none_visibility;
	map_vis = (dvis_t *)none_visibility;
	map_entitystring = none_entitystring;
	map_areas = none_areas;
	map_areaportals = none_areaportals;
	cm_generation++;
}

// Обнулённая таблица на count элементов (хотя бы на один): Z_TagMalloc сам чистит память
static void *CM_Table (int count, int size)
{
	return Z_TagMalloc ((count > 0 ? count : 1) * size, CM_TAG);
}

// Таблицы карты с заголовком h: размеры — из длин lump-ов, плюс «коробка» CM_InitBoxHull
static void CM_AllocTables (dheader_t *h)
{
#define	CM_COUNT(lump, type)	(h->lumps[lump].filelen / (int)sizeof(type))
	int	vis = h->lumps[LUMP_VISIBILITY].filelen;

	map_surfaces = CM_Table (CM_COUNT (LUMP_TEXINFO, texinfo_t), sizeof(mapsurface_t));
	map_leafs = CM_Table (CM_COUNT (LUMP_LEAFS, dleaf_t) + 1, sizeof(cleaf_t));
	map_leafbrushes = CM_Table (CM_COUNT (LUMP_LEAFBRUSHES, unsigned short) + 1, sizeof(unsigned short));
	map_planes = CM_Table (CM_COUNT (LUMP_PLANES, dplane_t) + 12, sizeof(cplane_t));
	map_brushes = CM_Table (CM_COUNT (LUMP_BRUSHES, dbrush_t) + 1, sizeof(cbrush_t));
	map_brushsides = CM_Table (CM_COUNT (LUMP_BRUSHSIDES, dbrushside_t) + 6, sizeof(cbrushside_t));
	map_cmodels = CM_Table (CM_COUNT (LUMP_MODELS, dmodel_t), sizeof(cmodel_t));
	map_nodes = CM_Table (CM_COUNT (LUMP_NODES, dnode_t) + 6, sizeof(cnode_t));
	map_areas = CM_Table (CM_COUNT (LUMP_AREAS, darea_t), sizeof(carea_t));
	map_areaportals = CM_Table (CM_COUNT (LUMP_AREAPORTALS, dareaportal_t), sizeof(dareaportal_t));
	map_visibility = CM_Table (vis > (int)sizeof(dvis_t) ? vis : (int)sizeof(dvis_t), 1);
	map_vis = (dvis_t *)map_visibility;
	map_entitystring = CM_Table (h->lumps[LUMP_ENTITIES].filelen + 1, 1);
#undef	CM_COUNT
}

/*
==================
CM_LoadMap
'''

patch('qcommon/cmodel.c', [
    (DECL_OLD, DECL_NEW),
    ('/*\n==================\nCM_LoadMap\n', FUNCS),
    ('	// free old stuff\n	numplanes = 0;\n',
     '	// free old stuff\n	CM_FreeTables ();		// порт VDAC4: таблицы прежней карты\n	numplanes = 0;\n'),
    ('		numareas = 1;\n		*checksum = 0;\n		return &map_cmodels[0];			// cinematic servers won\'t have anything at all\n',
     '		numareas = 1;\n		*checksum = 0;\n		CM_InitBoxHull ();		// порт VDAC4: «коробка» — в запасных таблицах\n'
     '		return &map_cmodels[0];			// cinematic servers won\'t have anything at all\n'),
    ('	cmod_base = (byte *)buf;\n', '	CM_AllocTables (&header);	// порт VDAC4: таблицы по размеру карты\n	cmod_base = (byte *)buf;\n'),
])
patch('qcommon/qcommon.h', [(
    'cmodel_t	*CM_LoadMap (char *name, qboolean clientload, unsigned *checksum);\n',
    'cmodel_t	*CM_LoadMap (char *name, qboolean clientload, unsigned *checksum);\n'
    'int			CM_Generation (void);	// порт VDAC4: растёт с каждой новой загруженной картой\n')])
patch('client/client.h', [(
    '	struct cmodel_s	*model_clip[MAX_MODELS];\n',
    '	struct cmodel_s	*model_clip[MAX_MODELS];\n'
    '	int			clip_generation;	// порт VDAC4: CM_Generation карты, к которой относятся model_clip\n')])
patch('client/cl_view.c', [(
    '		if (name[0] != \'*\')\n			Com_Printf ("                                     \\r");\n	}\n',
    '		if (name[0] != \'*\')\n			Com_Printf ("                                     \\r");\n	}\n'
    '	cl.clip_generation = CM_Generation ();	// порт VDAC4: model_clip — этой карты\n')])
patch('client/cl_pred.c', [(
    'cmodel = cl.model_clip[ent->modelindex];',
    'cmodel = cl.clip_generation == CM_Generation () ? cl.model_clip[ent->modelindex] : NULL;	// порт VDAC4: не прежней карты')],
    count=2)
print('ok')
