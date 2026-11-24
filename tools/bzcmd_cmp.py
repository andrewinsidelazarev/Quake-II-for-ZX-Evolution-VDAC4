"""Сравнить две трассы команд «бомж-3D» (.bzcmd) кадр за кадром.

Формат трассы (gpu/front/bz_front.c, BZ_EndFrame): на кадр — слово n (сколько слов команд), слово
цвета отладочной заливки (−1 — нет) и n слов команд. Совпадение двух трасс до бита значит, что ПЛИС
получит те же команды и нарисует тот же кадр.

  python tools/bzcmd_cmp.py <a.bzcmd> <b.bzcmd>
Печатает число кадров и слов у каждой и первый несовпавший кадр (номер слова, код команды).
"""
import struct
import sys


def frames(path):
    with open(path, 'rb') as f:
        data = f.read()
    pos, out = 0, []
    while pos + 8 <= len(data):
        n, clr = struct.unpack_from('<Ii', data, pos)
        pos += 8
        out.append((clr, data[pos:pos + 4 * n]))
        pos += 4 * n
    return out


def main(a, b):
    fa, fb = frames(a), frames(b)
    wa = sum(len(w) for _, w in fa) // 4
    wb = sum(len(w) for _, w in fb) // 4
    print(f'{a}: кадров {len(fa)}, слов {wa}')
    print(f'{b}: кадров {len(fb)}, слов {wb}')
    same = 0
    for i, (x, y) in enumerate(zip(fa, fb)):
        if x == y:
            same += 1
            continue
        wx, wy = x[1], y[1]
        k = next((j for j in range(0, min(len(wx), len(wy)), 4) if wx[j:j + 4] != wy[j:j + 4]),
                 min(len(wx), len(wy)))
        def word(w):
            return f'{struct.unpack_from("<I", w, k)[0]:08x}' if k + 4 <= len(w) else '—'
        print(f'первое расхождение: кадр {i}, слово {k // 4} (слов {len(wx) // 4} / {len(wy) // 4}), '
              f'{word(wx)} / {word(wy)}')
        break
    print(f'совпало кадров подряд с начала: {same}')
    return 0 if fa == fb else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
