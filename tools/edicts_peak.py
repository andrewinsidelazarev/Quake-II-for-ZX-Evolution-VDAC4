"""Сколько объектов игры (edict) занято: 47 карт после загрузки и 8 боёв по 60 с (порт).

python edicts_peak.py   — results\\edicts; печатает пики. Нужен, чтобы решить: 512 объектов вместо 1024.
"""
import csv
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(r'E:\zx\Quake2')
sys.path.insert(0, str(ROOT / 'measure'))
import run_all  # noqa: E402

OUT = ROOT / 'results/edicts'
EXE = ROOT / 'build/port/bin/q2m.exe'


def run(name, scen):
    r = subprocess.run([str(EXE), str(OUT / name)] + scen + ['--'] + run_all.BASE + run_all.CNT,
                       capture_output=True, cwd=EXE.parent)
    if r.returncode:
        sys.exit(f'{name}: код {r.returncode}')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    run('mem_sp', ['mapstats', ','.join(run_all.SP_MAPS)])
    run('mem_dm', ['mapstats', ','.join(run_all.DM_MAPS)])
    peaks = []
    for pre in ('mem_sp', 'mem_dm'):
        for s in (OUT / f'{pre}.engine.jsonl').read_text().splitlines():
            e = json.loads(s)
            peaks.append((e['edicts'], e['map']))
    peaks.sort(reverse=True)
    print('после загрузки карты (объектов занято), больше всех:', ', '.join(f'{m} {n}' for n, m in peaks[:8]))
    print(f'середина {peaks[len(peaks) // 2][0]}, меньше всех {peaks[-1][0]} ({peaks[-1][1]})')
    for m in run_all.FIGHT_MAPS:
        run(f'fight_{m}', ['fight', m, '60'])
        with open(OUT / f'fight_{m}.frames.csv', newline='') as f:
            rows = [r for r in csv.DictReader(f) if r['measuring'] == '1']
        print(f'бой {m:8s}: пик за 60 с {max(int(r["edicts"]) for r in rows)}')


if __name__ == '__main__':
    main()
