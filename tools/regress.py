"""Проверка картинки: кадры программного рендера порта против исходного id, по контрольным суммам.

python regress.py id      — эталон: build\\bin (исходники id), результаты в results\\regress\\id
python regress.py port    — порт: build\\port\\bin, результаты в results\\regress\\port, сравнение с id

Сцены: demo1, demo2 (демо id), бои base1 и city3 по 60 с. Каждая сцена даёт <имя>.crc.txt —
строка на кадр «номер crc» (CRC-32 точек кадра 320×240 и палитры, см. measure/ref_soft/swimp_q2m.c).
Совпали все строки — порт рисует то же самое, что id, кадр в кадр.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(r'E:\zx\Quake2')
BASE = ['+set', 'basedir', 'E:/zx/Quake2/data', '+set', 'vid_ref', 'soft', '+set', 'sw_mode', '0']
SCENES = {
    'demo1': ['demo', 'demo1.dm2'],
    'demo2': ['demo', 'demo2.dm2'],
    'fight_base1': ['fight', 'base1', '60'],
    'fight_city3': ['fight', 'city3', '60'],
}


def run(kind, tag):
    exe = ROOT / ('build/bin' if kind == 'id' else 'build/port/bin') / 'q2m.exe'
    out = ROOT / 'results/regress' / tag
    out.mkdir(parents=True, exist_ok=True)
    for name, scen in SCENES.items():
        r = subprocess.run([str(exe), str(out / name)] + scen + ['--'] + BASE, capture_output=True,
                           cwd=exe.parent)
        last = (r.stdout + r.stderr).decode('utf-8', 'replace').strip().splitlines()
        print(f'{tag:8s} {name:12s} код {r.returncode}  {last[-1] if last else ""}')
    return out


def compare(a, b):
    ok = True
    for name in SCENES:
        x = (a / f'{name}.crc.txt').read_text().splitlines()
        y = (b / f'{name}.crc.txt').read_text().splitlines()
        diff = next((i for i, (p, q) in enumerate(zip(x, y)) if p != q), None)
        if diff is None and len(x) == len(y):
            print(f'  {name:12s} совпали все {len(x)} кадров')
        else:
            ok = False
            where = f'первое расхождение в строке {diff}: {x[diff]} / {y[diff]}' if diff is not None \
                else f'длина {len(x)} против {len(y)}'
            print(f'  {name:12s} РАЗНЫЕ: {where}')
    return ok


def main():
    kind = sys.argv[1] if len(sys.argv) > 1 else 'port'
    if kind == 'id':
        a = run('id', 'id')
        b = run('id', 'id_again')
        print('id против повтора id (детерминизм):')
        sys.exit(0 if compare(a, b) else 1)
    b = run('port', 'port')
    print('port против id:')
    sys.exit(0 if compare(ROOT / 'results/regress/id', b) else 1)


if __name__ == '__main__':
    main()
