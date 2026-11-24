"""Сводка прогона ref_bz (кадр модели «бомж-3D» против ref_soft) и оценка SDRAM и LINK.

python bz_summary.py <prefix.bz.csv> [ширина высота]

Берёт счётчики C-модели по кадрам (gpu/model/bomzh.c) и считает, сколько тактов SDRAM нужно кадру.
Модель доступа (SDRAM 32 бита, CL3, случайная строка на каждую пачку — с запасом, без чередования
банков):
  накладные на пачку чтения 7 тактов (ACT, tRCD, READ, CL, PRE), записи 5;
  z пачка 32 байта = 8 слов; цвет 16 байт = 4 слова; строка кэша текстур 64 байта = 16 слов;
  выборка 2×2 клеток lightmap — две строки по одному слову;
  вывод HDMI: кадр W×H байт читается 60 раз в секунду, длинными пачками (+10 %).
Частоты: SDRAM 121,5 МГц (как FT-ядро 1.45 со своей rPLL и подбором фазы), конвейер 60,75 МГц,
1 точка за такт. Игра — 30 кадров в секунду.
"""
import csv
import statistics
import sys

SD_HZ = 121.5e6
PIPE_HZ = 60.75e6
GAME_FPS = 30


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))]


def main():
    path = sys.argv[1]
    w, h = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (320, 240)
    rows = [r for r in csv.DictReader(open(path)) if int(r['faces']) > 0]
    g = lambda r, k: int(r[k])

    def sd_clk(r):
        return (g(r, 'seg_zread') * (8 + 7) + g(r, 'seg_zwrite') * (8 + 5) + g(r, 'seg_cwrite') * (4 + 5) +
                g(r, 'seg_cread') * (4 + 7) + g(r, 'tex_miss') * (16 + 7) + g(r, 'lm_fetch') * 2 * (1 + 7) +
                (g(r, 'tex_bytes') + g(r, 'lm_bytes') + g(r, 'other_bytes')) / 4 * 1.1)

    scan = w * h / 4 * 1.1 * 60 / GAME_FPS          # вывод HDMI за один кадр игры
    clk = [sd_clk(r) + scan for r in rows]
    pipe = [g(r, 'px_raster') + g(r, 'px_fill') / 4 + g(r, 'seg') * 2 for r in rows]
    link = [g(r, 'cmd_bytes') + g(r, 'tex_bytes') + g(r, 'lm_bytes') + g(r, 'other_bytes') for r in rows]
    diff = [g(r, 'diff') for r in rows]
    vis = [g(r, 'diff_vis') for r in rows]
    n = w * h
    print(f'{path}: кадров с 3D {len(rows)}, экран {w}×{h}')
    print(f'  расхождение с ref_soft: индекс {statistics.mean(diff) / n * 100:.1f} % точек (p99 '
          f'{pct(diff, .99) / n * 100:.1f} %), заметно (>32 по каналу) {statistics.mean(vis) / n * 100:.2f} % '
          f'(p99 {pct(vis, .99) / n * 100:.2f} %)')
    for name, v, unit in (('точек растеризатора', [g(r, 'px_raster') for r in rows], 1),
                          ('записано точек', [g(r, 'px_written') for r in rows], 1),
                          ('промахов кэша текстур', [g(r, 'tex_miss') for r in rows], 1)):
        print(f'  {name}: ср {statistics.mean(v) / unit:.0f}, p99 {pct(v, .99) / unit:.0f}')
    hit = [1 - g(r, 'tex_miss') / max(1, g(r, 'tex_reads')) for r in rows]
    print(f'  попадания кэша текстур 8 КБ: ср {statistics.mean(hit) * 100:.1f} %')
    print(f'  SDRAM тактов на кадр: ср {statistics.mean(clk) / 1e3:.0f} тыс., p99 {pct(clk, .99) / 1e3:.0f} тыс. '
          f'→ при 30 к/с занято {statistics.mean(clk) * GAME_FPS / SD_HZ * 100:.0f} % (p99 '
          f'{pct(clk, .99) * GAME_FPS / SD_HZ * 100:.0f} %) от {SD_HZ / 1e6:.1f} МГц; '
          f'на 60,75 МГц было бы {pct(clk, .99) * GAME_FPS / 60.75e6 * 100:.0f} % (p99)')
    print(f'  конвейер (1 точка/такт, {PIPE_HZ / 1e6:.2f} МГц): p99 {pct(pipe, .99) / PIPE_HZ * 1e3:.1f} мс на кадр '
          f'из {1e3 / GAME_FPS:.1f}')
    print(f'  LINK (команды + загрузки): ср {statistics.mean(link) / 1024:.0f} КБ, p99 {pct(link, .99) / 1024:.0f} КБ, '
          f'макс {max(link) / 1024:.0f} КБ на кадр → при 30 к/с p99 {pct(link, .99) * GAME_FPS / 1e6:.1f} МБ/с')
    cmd = [g(r, 'cmd_bytes') for r in rows]
    print(f'  из них команды: ср {statistics.mean(cmd) / 1024:.0f} КБ, p99 {pct(cmd, .99) / 1024:.0f} КБ')


if __name__ == '__main__':
    main()
