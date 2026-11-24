"""Бюджет памяти Quake 2 на VDAC4 после «диеты»: что лежит в PSRAM ESP, что в SDRAM Tang.

python budget.py [папка замеров]   (по умолчанию E:\\zx\\Quake2\\results\\run)

Берёт настоящие количества по каждой из 47 карт (замер mapstats: модель столкновений, мир, модели,
картинки, звуки) и считает память при сжатой раскладке. Раскладка — предложение, а не готовый код:
каждый размер ниже объяснён в комментарии. Модели md2 разбираются по заголовкам из pak.
"""
import json
import pathlib
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import pak  # noqa: E402
from analyze import wav_info, _PAK  # noqa: E402  (заголовки WAV и оглавление pak)

RUN = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(r'E:\zx\Quake2\results\run')
MB = 1024 * 1024

# --- ESP: код и статика (замер xtensa_size.py, GCC 15.2 -O2)
CODE = 234230 + 9384 + 219585 + 66320 + 60 * 1024   # движок + игра + ~60 КБ своего рендера-подготовки
# Статика 1,68 МБ без таблиц карты; урезаем пределы (только одиночная игра; объектов — 1024, 512 мало):
#   cl_entities 283 → 141, particles 246 → 61 (1024 частицы), r_particles 82 → 20, s_rawsamples 64 → 0
#   (кинематики без звука из потока), con 33 → 8, s_pmi 90 → 0 (не нужен без плагина звука id),
#   baselines в sv (86) → 43, cl_parse_entities 86 → 43. Итого примерно −0,63 МБ.
STATIC = 1681718 + 45097 - (142 + 185 + 62 + 64 + 25 + 90 + 43 + 43) * 1024
ZONE_MISC = 0.37 * MB          # прочая зона движка (cvar, команды, оглавление pak) — замер

# --- SDRAM: кадры и таблицы
FB_240 = 320 * 240 * 2 * 2 + 320 * 240 * 2      # два кадра RGB565 + z 16 бит
FB_360 = 640 * 360 * 2 * 2 + 640 * 360 * 2
TABLES = 64 * 256 + 256 * 256 + 256 * 4          # colormap, таблица прозрачности, палитра


def md2_parts(name):
    """(кадры анимации, остальное ESP) в байтах по заголовку md2; None — не md2."""
    hit = _PAK.get(name.lower())
    if not hit or not name.endswith('.md2'):
        return None
    with open(hit[0], 'rb') as f:
        f.seek(hit[1])
        h = f.read(68)
    v = [int.from_bytes(h[i:i + 4], 'little', signed=True) for i in range(0, 68, 4)]
    framesize, num_xyz, num_glcmds, num_frames = v[4], v[6], v[9], v[10]
    frames = num_frames * num_xyz * 4                 # сжатые вершины: 3 байта + индекс нормали
    esp = 68 + num_frames * 40 + num_glcmds * 4       # заголовок, масштаб/сдвиг кадров, полосы и веера
    return frames, esp


def budget_map(eng, ref):
    w, cm = ref['world'], eng['cmodel']
    n = lambda k: w[k][0]
    c = lambda k: cm[k][0]
    # Модель столкновений: плоскости и PVS общие с рендером (один и тот же lump карты).
    # Узлы и листья столкновений входят в узлы и листья рендера (ниже), строка сущностей
    # освобождается после появления объектов.
    coll = c('leafbrushes') * 2 + c('brushes') * 16 + c('brushsides') * 8 + c('texinfo') * 24 \
        + c('cmodels') * 40 + c('areas') * 16 + c('areaportals') * 8
    shared = n('planes') * 20 + w['vis_lump']
    # Мир для подготовки кадра (сжатые структуры):
    #   вершина 12 Б (3 float), ребро 4 Б (2 × uint16), surfedge 4 Б, поверхность 32 Б (плоскость,
    #   рёбра, texinfo, флаги, texturemins/extents, стили, адрес lightmap в SDRAM, visframe),
    #   texinfo 40 Б, узел 32 Б (bbox int16, плоскость, дети, поверхности, visframe, данные
    #   столкновений), лист 36 Б (bbox, кластер, область, marksurfaces, contents, кисти), marksurface 2 Б.
    world = n('vertexes') * 12 + n('edges') * 4 + n('surfedges') * 4 + n('surfaces') * 32 \
        + n('texinfo') * 40 + n('nodes') * 32 + n('leafs') * 36 + n('marksurfaces') * 2 + n('submodels') * 56
    frames = esp_models = 0
    for name, typ, extra, _ in ref['models']:
        if typ == 3:
            parts = md2_parts(name)
            if parts:
                frames += parts[0]
                esp_models += parts[1]
        elif typ == 2:
            esp_models += extra
    secs = 0.0
    for s in eng['sounds']:
        if s[1]:
            info = wav_info(s[0])
            if info and info[0]:
                secs += info[3] / info[0]
    snd_11k8 = int(secs * 11025)
    tags = eng['zone_tags']
    # 1024 объекта: замер 23.11 — space занимает 879, бои до 687 (tools/edicts_peak.py)
    game = tags.get('765', [0])[0] + tags.get('766', [0])[0]
    tex = 0
    for name, typ, wd, ht, cur in ref['images']:
        tex += wd * ht * 85 // 64 if typ == 2 else wd * ht
    return {
        'map': eng['map'], 'coll': coll, 'shared': shared, 'world': world, 'models_esp': esp_models,
        'md2_frames': frames, 'snd_11k8': snd_11k8, 'game': game,
        'tex': tex, 'lightmap': w['lux_all'],
    }


def main():
    rows = []
    for pre in ('mem_sp', 'mem_dm'):
        eng = [json.loads(s) for s in (RUN / f'{pre}.engine.jsonl').read_text().splitlines()]
        ref = {r['map']: r for r in (json.loads(s) for s in (RUN / f'{pre}.ref.jsonl').read_text().splitlines())}
        rows += [budget_map(e, ref[e['map']]) for e in eng]

    fixed_esp = CODE + STATIC + ZONE_MISC
    print(f'ESP постоянно: код {CODE / MB:.2f} + статика {STATIC / MB:.2f} + зона {ZONE_MISC / MB:.2f} '
          f'= {fixed_esp / MB:.2f} МБ')
    print(f'SDRAM постоянно: кадры 320×240 {FB_240 / MB:.2f} (640×360 {FB_360 / MB:.2f}) + таблицы '
          f'{TABLES / MB:.2f} МБ\n')

    def show(title, esp_fn, sdram_fn):
        e = [esp_fn(r) for r in rows]
        s = [sdram_fn(r) for r in rows]
        we = max(rows, key=esp_fn)
        ws = max(rows, key=sdram_fn)
        print(f'{title}\n  ESP   середина {statistics.median(e) / MB:5.2f}  макс {max(e) / MB:5.2f} ({we["map"]})'
              f'\n  SDRAM середина {statistics.median(s) / MB:5.2f}  макс {max(s) / MB:5.2f} ({ws["map"]})')

    base_esp = lambda r: fixed_esp + r['coll'] + r['shared'] + r['world'] + r['models_esp'] + r['game']
    base_sd = lambda r: FB_240 + TABLES + r['tex'] + r['lightmap']
    show('А. Кадры моделей и звук в ESP', lambda r: base_esp(r) + r['md2_frames'] + r['snd_11k8'], base_sd)
    show('Б. Звук в SDRAM, кадры моделей в ESP', lambda r: base_esp(r) + r['md2_frames'],
         lambda r: base_sd(r) + r['snd_11k8'])
    show('В. Звук и кадры моделей в SDRAM', base_esp, lambda r: base_sd(r) + r['snd_11k8'] + r['md2_frames'])
    show('В при 640×360', base_esp,
         lambda r: base_sd(r) - FB_240 + FB_360 + r['snd_11k8'] + r['md2_frames'])
    # Г. Гибко: звук и кадры моделей кладутся в SDRAM, пока там есть место (8 МБ минус 0,25 МБ на
    # очереди и кольцо команд), остаток — в ESP. Делится по целым моделям и звукам, здесь — по байтам.
    cap = 8 * MB - MB // 4
    flex_esp = lambda r: base_esp(r) + max(0, r['md2_frames'] + r['snd_11k8'] - (cap - base_sd(r)))
    show('Г. Гибко: SDRAM до 7,75 МБ, остаток в ESP (320×240)', flex_esp,
         lambda r: min(cap, base_sd(r) + r['md2_frames'] + r['snd_11k8']))
    over = sorted(rows, key=flex_esp, reverse=True)[:5]
    print('  тяжелее всех для ESP:', ', '.join(f'{r["map"]} {flex_esp(r) / MB:.2f}' for r in over))

    print('\nпо частям, МБ: середина / максимум')
    for k in ('coll', 'shared', 'world', 'models_esp', 'md2_frames', 'snd_11k8', 'game', 'tex', 'lightmap'):
        v = [r[k] for r in rows]
        print(f'  {k:11s} {statistics.median(v) / MB:5.2f} / {max(v) / MB:5.2f}')
    (RUN / 'budget.json').write_text(json.dumps(rows, ensure_ascii=False, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
