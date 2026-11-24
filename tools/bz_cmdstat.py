"""Из чего состоит поток команд «бомж-3D» (трасса ref_bz .bzcmd) и сколько даст его ужатие.

python bz_cmdstat.py <трасса.bzcmd> [кадров]

По кадрам считает слова по видам команд (POLY мира с lightmap, модели со светом вершин, небо, вода,
прозрачное, 2D; TEX, LMAP, POINT, PIC, FILL, WRITE) и оценивает варианты ужатия LINK:
  А. TEX/LMAP только при смене (передний край сейчас шлёт их перед каждым многоугольником);
  Б. градиенты A, B — по 16 бит с общим порядком на величину (C — 32 бита): 3 слова на величину → 2;
  В. LMAP в 2 слова (адрес + клеток/строк/сдвиг; smin/tmin — 16-битные в одном слове);
  Г. частица в 2 слова;
  Д. модели полосами: вершина (xy, zi, s|t, l) один раз, треугольник ≈ одна новая вершина;
     градиенты треугольника считает ПЛИС (оценка: 4 слова на треугольник вместо 13).
Загрузки (WRITE) считаются отдельно: их можно отдать второму каналу (SPI Evo или карта Tang).
"""
import collections
import struct
import sys

OPS = {1: 'TEX', 2: 'LMAP', 3: 'POLY', 4: 'POINT', 5: 'PIC', 6: 'FILL', 7: 'FRAME', 8: 'TIME',
       9: 'WRITE', 10: 'SETUP', 11: 'SCISSOR'}
FIX = {1: 3, 2: 5, 4: 3, 5: 7, 6: 4, 7: 2, 8: 2, 10: 9, 11: 3}


def frames(path, limit):
    with open(path, 'rb') as f:
        k = 0
        while k < limit:
            h = f.read(8)
            if len(h) < 8:
                return
            n, _ = struct.unpack('<Ii', h)
            yield struct.unpack(f'<{n}I', f.read(4 * n))
            k += 1


def poly_kind(mode):
    light = (mode >> 1) & 3
    if mode & (1 << 5):
        return 'POLY небо'
    if (mode >> 7) & 3:
        return 'POLY прозрачные'
    if mode & (1 << 6):
        return 'POLY вода'
    if light == 0:
        return 'POLY мир (lightmap)'
    if light == 1:
        return 'POLY модели (свет вершин)'
    if not mode & 1:
        return 'POLY 2D/плитка'
    return 'POLY спрайты, лучи, оболочки'


def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 1 << 30
    tot = collections.Counter()
    cnt = collections.Counter()
    nfr = 0
    save = collections.Counter()
    per_frame = []
    for w in frames(path, limit):
        i = 0
        last_tex = last_lm = None
        fr = collections.Counter()
        has3d = False
        while i < len(w):
            op = w[i] >> 24
            if op == 3:
                n, mode = w[i] & 0xFF, (w[i] >> 8) & 0xFFFF
                ln = 1 + n + 9 + (3 if ((mode >> 1) & 3) == 1 else 0)
                k = poly_kind(mode)
                fr[k] += ln
                cnt[k] += 1
                if ((mode >> 1) & 3) == 1:
                    save['Д'] += ln - 4              # треугольник модели: ~4 слова вместо 1+3+12
                else:
                    save['Б'] += 3                   # 3 слова → 2 на величину (у моделей это уже в Д)
                if mode & 1:
                    has3d = True
            elif op in (12, 13, 14):
                # пачка модели (SPEC 0.3): VBUF — 3 слова на вершину, STBUF — 1, TRIS — 2 на треугольник
                cnt_n = w[i] & (0xFF if op == 14 else 0x3FF)
                ln = 1 + cnt_n * {12: 3, 13: 1, 14: 2}[op]
                k = {12: 'VBUF вершины моделей', 13: 'STBUF шкурки моделей', 14: 'TRIS треугольники моделей'}[op]
                fr[k] += ln
                cnt[k] += cnt_n
                has3d = True
            elif op == 9:
                ln = 2 + (w[i] & 0xFFFFFF)
                fr['WRITE'] += ln
            elif op in FIX:
                ln = FIX[op]
                name = OPS[op]
                if op == 1:
                    key = tuple(w[i:i + 3])
                    if key == last_tex:
                        save['А'] += 3
                    last_tex = key
                elif op == 2:
                    key = tuple(w[i:i + 5])
                    if key == last_lm:
                        save['А'] += 5
                    else:
                        save['В'] += 3
                    last_lm = key
                elif op == 4:
                    save['Г'] += 1
                fr[name] += ln
                cnt[name] += 1
            else:
                raise SystemExit(f'неизвестная команда {w[i]:08x}')
            i += ln
        tot.update(fr)
        if has3d:
            per_frame.append(fr)
        nfr += 1
    n3 = max(1, len(per_frame))
    allw = sum(sum(f.values()) - f['WRITE'] for f in per_frame)
    print(f'{path}: кадров {nfr}, с 3D {len(per_frame)}')
    print(f'  команды без WRITE: {allw / n3 * 4 / 1024:.1f} КБ на кадр с 3D')
    for k, v in sorted(tot.items(), key=lambda x: -x[1]):
        print(f'    {k:30s} {v / n3 * 4 / 1024:7.1f} КБ/кадр  ({cnt[k] / n3:7.0f} шт.)')
    print('  ужатие (КБ на кадр с 3D):')
    names = {'А': 'TEX/LMAP только при смене', 'Б': 'градиенты A,B по 16 бит', 'В': 'LMAP в 2 слова',
             'Г': 'частица в 2 слова', 'Д': 'модели полосами, градиенты в ПЛИС'}
    left = allw
    for k in 'АБВГД':
        print(f'    {k}. {names[k]:38s} −{save[k] / n3 * 4 / 1024:6.1f}')
        left -= save[k]
    print(f'  осталось бы: {left / n3 * 4 / 1024:.1f} КБ на кадр ({left / max(1, allw) * 100:.0f} %)')


if __name__ == '__main__':
    main()
