"""Порт против id: память на каждой карте (47 карт подряд — заодно проверка смены карт).

python mem_compare.py   — results\\port_mem\\mem_{sp,dm}.* (порт) против results\\run\\mem_{sp,dm}.* (id)
Печатает: зона движка по меткам (у порта таблицы карты — метка 767), счётчики модели столкновений
(должны совпасть с id) и сколько байт заняли таблицы карты против статических массивов id.
"""
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(r'E:\zx\Quake2')
sys.path.insert(0, str(ROOT / 'measure'))
import run_all  # noqa: E402

OUT = ROOT / 'results/port_mem'
EXE = ROOT / 'build/port/bin/q2m.exe'


def run(name, maps):
    r = subprocess.run([str(EXE), str(OUT / name), 'mapstats', ','.join(maps), '--'] + run_all.BASE + run_all.CNT,
                       capture_output=True, cwd=EXE.parent)
    last = (r.stdout + r.stderr).decode('utf-8', 'replace').strip().splitlines()
    print(f'{name}: код {r.returncode}  {last[-1] if last else ""}')
    if r.returncode:
        sys.exit(1)


def load(path):
    return {e['map']: e for e in (json.loads(s) for s in path.read_text().splitlines())}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    run('mem_sp', run_all.SP_MAPS)
    run('mem_dm', run_all.DM_MAPS)
    worst = 0
    same = True
    for pre in ('mem_sp', 'mem_dm'):
        port = load(OUT / f'{pre}.engine.jsonl')
        ref = load(ROOT / 'results/run' / f'{pre}.engine.jsonl')
        for name, p in port.items():
            r = ref[name]
            for k in r['cmodel']:
                if r['cmodel'][k] != p['cmodel'][k]:
                    same = False
                    print(f'  {name}: cmodel {k} id {r["cmodel"][k]} порт {p["cmodel"][k]}')
            tables = p['zone_tags'].get('767', [0, 0])[0]
            worst = max(worst, tables)
            print(f'  {name:9s} таблицы карты {tables / 1024:7.0f} КБ, зона всего {p["zone_bytes"] / 1024:7.0f} КБ '
                  f'(id {r["zone_bytes"] / 1024:7.0f} КБ)')
    print(f'счётчики модели столкновений как у id: {"да" if same else "НЕТ"}; таблицы карты — до '
          f'{worst / 1024:.0f} КБ вместо 5 755 КБ статики id')


if __name__ == '__main__':
    main()
