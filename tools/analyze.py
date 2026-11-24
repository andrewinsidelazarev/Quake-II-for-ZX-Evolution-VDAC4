"""Разбор замеров Quake 2 для VDAC4: память по картам, поток для ПЛИС, время частей кадра.

python analyze.py [папка]   (по умолчанию E:\\zx\\Quake2\\results\\run)
Пишет <папка>\\summary.json и печатает таблицы.
"""
import csv
import json
import pathlib
import statistics
import sys

RUN = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(r'E:\zx\Quake2\results\run')
CATS = ['world', 'warp', 'sky', 'alias', 'bmodel', 'sprite', 'part', 'alpha', 'blend', '2d']
CATS3D = CATS[:-1]

# ---------------------------------------------------------------- память

DATA = pathlib.Path(r'E:\zx\Quake2\data\baseq2')
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import pak  # noqa: E402

_PAK = {}
for _p in ('pak0.pak', 'pak1.pak', 'pak2.pak'):      # поздний pak главнее, как в FS id
    for _n, _pos, _size in pak.entries(DATA / _p):
        _PAK[_n.lower()] = (DATA / _p, _pos, _size)


def wav_info(name):
    """(частота, байт на отсчёт, каналов, отсчётов) из заголовка WAV в pak; None — нет файла."""
    path = name[1:] if name.startswith('#') else 'sound/' + name
    hit = _PAK.get(path.lower())
    if not hit:
        return None
    with open(hit[0], 'rb') as f:
        f.seek(hit[1])
        raw = f.read(hit[2])
    pos, rate, width, ch, samples = 12, 0, 0, 0, 0
    while pos + 8 <= len(raw):
        cid, size = raw[pos:pos + 4], int.from_bytes(raw[pos + 4:pos + 8], 'little')
        if cid == b'fmt ':
            ch = int.from_bytes(raw[pos + 10:pos + 12], 'little')
            rate = int.from_bytes(raw[pos + 12:pos + 16], 'little')
            width = int.from_bytes(raw[pos + 22:pos + 24], 'little') // 8
        elif cid == b'data':
            samples = min(size, len(raw) - pos - 8) // max(1, width * ch)
        pos += 8 + size + (size & 1)
    return rate, width, ch, samples


def mem(prefix):
    eng = [json.loads(s) for s in (RUN / f'{prefix}.engine.jsonl').read_text().splitlines()]
    ref = {r['map']: r for r in (json.loads(s) for s in (RUN / f'{prefix}.ref.jsonl').read_text().splitlines())}
    rows = []
    for e in eng:
        r = ref[e['map']]
        cm = e['cmodel']
        cmodel = sum(cm[k][0] * cm[k][1] for k in cm if isinstance(cm[k], list))
        w = r['world']
        world = sum(w[k][0] * w[k][1] for k in ('submodels', 'planes', 'leafs', 'vertexes', 'edges', 'nodes',
                                               'texinfo', 'surfaces', 'surfedges', 'marksurfaces'))
        world += w['vis_lump']
        models = sum(m[2] for m in r['models'] if m[1] in (2, 3))
        nmodels = sum(1 for m in r['models'] if m[1] == 3)
        snd = [s for s in e['sounds'] if s[1]]
        # по заголовкам WAV: секунды звука и память в разных форматах
        secs = orig = sfx_zone = 0
        rates = {}
        for s in snd:
            info = wav_info(s[0])
            if not info or not info[0]:
                continue
            rate, width, ch, samples = info
            secs += samples / rate
            orig += samples * width * ch
            rates[f'{rate}/{width * 8}'] = rates.get(f'{rate}/{width * 8}', 0) + 1
            # snd_mem.c: Z_Malloc (samples / (rate / dma.speed) * исходная ширина * каналы + sfxcache_t)
            sfx_zone += int(samples / (rate / e['dma_speed'])) * width * ch + 24
        s22_8 = int(secs * 22050)
        tags = e['zone_tags']
        zone0 = tags.get('0', [0, 0])[0]
        game = tags.get('765', [0, 0])[0] + tags.get('766', [0, 0])[0]
        img = {'wall': 0, 'skin': 0, 'sprite': 0, 'pic': 0, 'sky': 0}
        names = ['skin', 'sprite', 'wall', 'pic', 'sky']
        for name, typ, wd, ht, cur in r['images']:
            if typ == 2:
                img['wall'] += wd * ht * 85 // 64      # WAL: 4 уровня mip
            else:
                img[names[typ]] += wd * ht
        rows.append({
            'map': e['map'],
            'cmodel': cmodel, 'world': world, 'world_vis': w['vis_lump'], 'glpoly': w['glpoly_bytes'],
            'models': models, 'nmodels': nmodels, 'nsounds': len(snd), 'snd_seconds': secs,
            'snd_orig': orig, 'snd_rates': rates, 'snd_id_zone': sfx_zone,
            'snd_11k8': s22_8 // 2, 'snd_22k8': s22_8, 'snd_22k16': s22_8 * 2,
            'game': game, 'zone_other': zone0 - sfx_zone,
            'tex_wall': img['wall'], 'tex_skin': img['skin'], 'tex_sprite': img['sprite'],
            'tex_pic': img['pic'], 'tex_sky': img['sky'],
            'lux_base': w['lux_base'], 'lux_all': w['lux_all'], 'lighting_lump': w['lighting_lump'],
            'surfaces': w['surfaces'][0], 'vertexes': w['vertexes'][0],
        })
    return rows


# ----------------------------------------------------------- кадры


def read_csv(path):
    with open(path, newline='') as f:
        return list(csv.DictReader(f))


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))]


def stat(v):
    return {'mean': statistics.fmean(v), 'p50': pct(v, 0.5), 'p95': pct(v, 0.95), 'p99': pct(v, 0.99), 'max': max(v)}


# Байт на кадр для LINK, два способа передачи (см. отчёт):
#  A «по вершинам» (как GL): заголовок примитива 8 Б; вершина мира 24 Б (x,y, 1/z, s/z, t/z, ls/z, lt/z),
#    вершина модели 16 Б (x,y, 1/z, s/z, t/z, свет); частица 8 Б; картинка 2D 16 Б; вспышка 8 Б.
#  B «по плоскостям» (как программный рендер Quake): многоугольник мира — 8 Б заголовок + 36 Б градиенты
#    1/z, s/z, t/z + 8 Б lightmap + 4 Б на вершину (x,y); модели и прочее — как в A.
def stream_bytes(r):
    g = lambda c, k: int(r[f'{c}_{k}'])
    parts = g('part', 'tris')
    surf = ['world', 'warp', 'sky', 'bmodel', 'alpha']
    a = sum(8 * g(c, 'prims') + 24 * g(c, 'verts') for c in surf)
    b = sum(52 * g(c, 'prims') + 4 * g(c, 'verts') for c in surf)
    rest = 8 * g('alias', 'prims') + 16 * g('alias', 'verts') + 8 * g('sprite', 'prims') + 16 * g('sprite', 'verts') \
        + 8 * parts + 16 * g('2d', 'prims') + 8 * g('blend', 'prims')
    return a + rest, b + rest


def frames(name):
    fr = {int(r['frame']): r for r in read_csv(RUN / f'{name}.frames.csv') if r['measuring'] == '1'}
    rf = [r for r in read_csv(RUN / f'{name}.ref.csv') if int(r['frame']) in fr]
    out = {'frames': len(rf)}
    if not rf:
        return out
    sa = int(rf[0]['w']) * int(rf[0]['h'])
    cols = {}
    for c in CATS:
        for k in ('prims', 'verts', 'tris', 'area'):
            cols[f'{c}_{k}'] = [float(r[f'{c}_{k}']) for r in rf]
    tot = lambda k, cats: [sum(float(r[f'{c}_{k}']) for c in cats) for r in rf]
    out['screen'] = [int(rf[0]['w']), int(rf[0]['h'])]
    out['prims3d'] = stat(tot('prims', CATS3D))
    out['verts3d'] = stat(tot('verts', CATS3D))
    out['tris3d'] = stat(tot('tris', CATS3D))
    out['overdraw3d'] = stat([a / sa for a in tot('area', CATS3D)])
    # без полноэкранных вспышек (урон, подбор, вода): в ПЛИС их дешевле делать на выводе кадра
    out['overdraw_noblend'] = stat([a / sa for a in tot('area', [c for c in CATS3D if c != 'blend'])])
    out['overdraw_world'] = stat([a / sa for a in cols['world_area']])
    out['by_cat'] = {c: {k: statistics.fmean(cols[f'{c}_{k}']) for k in ('prims', 'verts', 'tris', 'area')} for c in CATS}
    out['by_cat_max'] = {c: {k: max(cols[f'{c}_{k}']) for k in ('prims', 'verts', 'tris', 'area')} for c in CATS}
    ab = [stream_bytes(r) for r in rf]
    out['bytes_A'] = stat([x[0] for x in ab])
    out['bytes_B'] = stat([x[1] for x in ab])
    for k in ('ents', 'parts', 'dlights', 'binds', 'utex', 'utex_bytes', 'ulm', 'dlm_luxels'):
        out[k] = stat([float(r[k]) for r in rf])
    return out


TIMES = ['total_us', 'sv_us', 'game_us', 'cl_us', 'scr_us', 'rf_us', 'snd_us']


def times(names):
    """Время частей кадра; при нескольких повторах — по каждому кадру меньшее (шум Windows вверх)."""
    runs = []
    for n in names:
        rows = [r for r in read_csv(RUN / f'{n}.frames.csv') if r['measuring'] == '1']
        runs.append({int(r['frame']): r for r in rows})
    keys = sorted(set.intersection(*(set(r) for r in runs)))
    best = {k: {} for k in keys}
    for k in keys:
        for t in TIMES:
            best[k][t] = min(float(r[k][t]) for r in runs)
    v = lambda t: [best[k][t] for k in keys]
    out = {t: stat(v(t)) for t in TIMES}
    cl_other = [best[k]['cl_us'] - best[k]['scr_us'] - best[k]['snd_us'] for k in keys]
    scr_2d = [best[k]['scr_us'] - best[k]['rf_us'] for k in keys]
    out['cl_other_us'] = stat(cl_other)
    out['scr_2d_us'] = stat(scr_2d)
    out['frames'] = len(keys)
    return out


def main():
    summary = {'mem_sp': mem('mem_sp'), 'mem_dm': mem('mem_dm'), 'scenes': {}}
    scenes = ['demo1', 'demo2'] + [p.name[len('fight_'):-len('_cnt.frames.csv')]
                                   for p in sorted(RUN.glob('fight_*_cnt.frames.csv'))]
    for s in scenes:
        pre = s if s.startswith('demo') else f'fight_{s}'
        rep = lambda suf: [f'{pre}_{suf}{i}' for i in range(3)] if s.startswith('demo') else [f'{pre}_{suf}']
        summary['scenes'][s] = {
            'stream': frames(f'{pre}_cnt'),
            'cpu_count': times([f'{pre}_cnt']),
            'cpu_cnt': times(rep('cnt_time') if s.startswith('demo') else [f'{pre}_cnt_time']),
            'cpu_soft240': times(rep('soft240_') if s.startswith('demo') else [f'{pre}_soft240']),
            'cpu_soft360': times(rep('soft360_') if s.startswith('demo') else [f'{pre}_soft360']),
        }
    (RUN / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding='utf-8')

    kb = lambda x: x / 1024
    print('== память, КБ (одиночная игра, 39 карт): середина / максимум')
    for key in ('cmodel', 'world', 'models', 'snd_orig', 'snd_id_zone', 'snd_11k8', 'snd_22k8', 'snd_22k16',
                'game', 'zone_other',
                'tex_wall', 'tex_skin', 'tex_sprite', 'tex_pic', 'tex_sky', 'lux_base', 'lux_all', 'glpoly'):
        v = [r[key] for r in summary['mem_sp']]
        top = max(summary['mem_sp'], key=lambda r: r[key])
        print(f'{key:12s} {kb(statistics.median(v)):8.0f} {kb(max(v)):8.0f}  ({top["map"]})')
    for s, d in summary['scenes'].items():
        st = d['stream']
        print(f'\n== {s}: кадров {st["frames"]}, экран {st["screen"]}')
        for k in ('prims3d', 'verts3d', 'tris3d', 'overdraw3d', 'overdraw_world', 'bytes_A', 'bytes_B',
                  'parts', 'dlights', 'utex', 'utex_bytes', 'dlm_luxels'):
            x = st[k]
            print(f'  {k:15s} mean {x["mean"]:10.1f} p95 {x["p95"]:10.1f} p99 {x["p99"]:10.1f} max {x["max"]:10.1f}')
        for run in ('cpu_cnt', 'cpu_count', 'cpu_soft240', 'cpu_soft360'):
            t = d[run]
            print(f'  {run:11s} total mean {t["total_us"]["mean"]:7.1f} p99 {t["total_us"]["p99"]:7.1f} max '
                  f'{t["total_us"]["max"]:7.1f} | rf {t["rf_us"]["mean"]:7.1f} | sv {t["sv_us"]["mean"]:6.1f} '
                  f'game {t["game_us"]["mean"]:6.1f} | cl_other {t["cl_other_us"]["mean"]:6.1f} '
                  f'2d {t["scr_2d_us"]["mean"]:6.1f} snd {t["snd_us"]["mean"]:5.1f}')


if __name__ == '__main__':
    main()
