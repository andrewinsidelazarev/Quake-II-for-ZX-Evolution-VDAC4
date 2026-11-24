"""Цифры для отчёта: память ESP и SDRAM по картам, поток LINK, площадь, время ESP по коэффициенту.

python report_numbers.py [папка]   — читает summary.json (analyze.py)
"""
import json
import pathlib
import statistics
import sys

RUN = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(r'E:\zx\Quake2\results\run')
S = json.loads((RUN / 'summary.json').read_text(encoding='utf-8'))
MB = 1024 * 1024

# Замер xtensa_size.py (GCC 15.2 Xtensa, -O2): код+константы и статические массивы без таблиц карты
CODE = 234230 + 9384 + 219585 + 66320      # движок + игра (text+data)
CODE_SOFT = 74715 + 7300                     # программный рендер id
STATIC_OTHER = 1681718 + 45097               # .bss движка без map_* + .bss игры
FB_360 = 640 * 360 * 2 * 2 + 640 * 360 * 2   # два кадра RGB565 + z 16 бит
FB_240 = 320 * 240 * 2 * 2 + 320 * 240 * 2


def esp_map(r, snd):
    return r['cmodel'] + r['world'] + r['models'] + r[snd] + r['game'] + r['zone_other']


def sdram_map(r):
    return r['tex_wall'] + r['tex_skin'] + r['tex_sprite'] + r['tex_pic'] + r['tex_sky'] + r['lux_all']


def main():
    maps = S['mem_sp'] + S['mem_dm']
    print('== память по картам (47 карт: 39 одиночных + 8 сетевых), МБ: середина / максимум (карта)')
    for title, fn in [
        ('ESP данные, звук 22к/16', lambda r: esp_map(r, 'snd_22k16')),
        ('ESP данные, звук 11к/8', lambda r: esp_map(r, 'snd_11k8')),
        ('ESP данные без звука', lambda r: esp_map(r, 'snd_11k8') - r['snd_11k8']),
        ('SDRAM текстуры+lightmap', sdram_map),
    ]:
        v = [fn(r) for r in maps]
        top = max(maps, key=fn)
        print(f'  {title:26s} {statistics.median(v) / MB:5.2f} / {max(v) / MB:5.2f} ({top["map"]})')
    print(f'  код движок+игра {CODE / MB:.2f} МБ, программный рендер ещё {CODE_SOFT / MB:.2f}; '
          f'статика без таблиц карты {STATIC_OTHER / MB:.2f}; кадры 640×360 {FB_360 / MB:.2f}, 320×240 {FB_240 / MB:.2f}')
    for key in ('cmodel', 'world', 'models', 'snd_11k8', 'snd_22k8', 'snd_22k16', 'game', 'zone_other',
                'tex_wall', 'tex_skin', 'tex_sprite', 'tex_pic', 'tex_sky', 'lux_all'):
        v = [r[key] for r in maps]
        print(f'    {key:11s} {statistics.median(v) / MB:5.2f} / {max(v) / MB:5.2f}')

    print('\n== поток и площадь по сценам (640×480 у счётчика)')
    print('  сцена      кадров  мн-ков ср/p99   вершин ср/p99   перерис. ср/p99  мир ср   LINK A ср/p99 КБ  B ср/p99 КБ  частиц p99')
    for name, d in S['scenes'].items():
        st = d['stream']
        print(f'  {name:9s} {st["frames"]:6d}  {st["prims3d"]["mean"]:6.0f}/{st["prims3d"]["p99"]:5.0f}  '
              f'{st["verts3d"]["mean"]:7.0f}/{st["verts3d"]["p99"]:6.0f}  {st["overdraw3d"]["mean"]:5.2f}/'
              f'{st["overdraw3d"]["p99"]:5.2f}  {st["overdraw_world"]["mean"]:5.2f}  '
              f'{st["bytes_A"]["mean"] / 1024:6.0f}/{st["bytes_A"]["p99"] / 1024:5.0f}  '
              f'{st["bytes_B"]["mean"] / 1024:6.0f}/{st["bytes_B"]["p99"] / 1024:5.0f}  {st["parts"]["p99"]:6.0f}')
    print('  площадь по видам (доля экрана, среднее):')
    for name, d in S['scenes'].items():
        bc = d['stream']['by_cat']
        sa = d['stream']['screen'][0] * d['stream']['screen'][1]
        print('   ', name, ' '.join(f'{c}:{bc[c]["area"] / sa:.2f}' for c in bc if bc[c]['area'] > 0))

    print('\n== перерисовка без вспышек и SDRAM (7 Б на точку при 30 к/с + вывод 60 Гц RGB565), МБ/с')
    for name, d in S['scenes'].items():
        st = d['stream']
        o, u, lm = st['overdraw_noblend'], st['utex_bytes'], st['dlm_luxels']
        bw = lambda w, h, od: (w * h * od * 7 * 30 + w * h * 2 * 60) / 1e6
        print(f'  {name:9s} перерис. ср {o["mean"]:.2f} p99 {o["p99"]:.2f} max {o["max"]:.2f} | 640×360 ср '
              f'{bw(640, 360, o["mean"]):4.0f} p99 {bw(640, 360, o["p99"]):4.0f} | 320×240 p99 {bw(320, 240, o["p99"]):3.0f}'
              f' | текстур за кадр ср {u["mean"] / 1024:4.0f} p99 {u["p99"] / 1024:4.0f} КБ | дин. lightmap p99 {lm["p99"]:5.0f}')

    print('\n== время ПК (мкс на кадр) и ESP при K = 40…80 (мс), кадров/с = 1000/мс')
    for name, d in S['scenes'].items():
        for run, label in (('cpu_soft240', 'всё на ESP 320×240'), ('cpu_soft360', 'всё на ESP 640×360'),
                           ('cpu_cnt', 'ESP+ПЛИС без подготовки'), ('cpu_count', 'ESP+ПЛИС с подготовкой')):
            t = d[run]['total_us']
            sv = d[run]['sv_us']['mean'] + d[run]['snd_us']['mean']
            m, p = t['mean'], t['p99']
            print(f'  {name:9s} {label:26s} ПК ср {m:6.0f} p99 {p:6.0f} | ESP ср {m * 40 / 1000:5.1f}…{m * 80 / 1000:5.1f} мс '
                  f'({1e6 / (m * 80):4.0f}…{1e6 / (m * 40):4.0f} к/с), p99 {p * 40 / 1000:5.1f}…{p * 80 / 1000:5.1f} мс '
                  f'| сервер+звук {sv / m * 100:4.1f} %')


if __name__ == '__main__':
    main()
