"""Все замеры Quake 2 для VDAC4 одним прогоном.

python run_all.py [папка]   (по умолчанию E:\\zx\\Quake2\\results\\run)

Сценарии (q2m.exe, виртуальные часы 30 кадров/с игрового времени):
  mem_sp / mem_dm      — память на каждой карте одиночной игры (39) и сетевой (8);
  <демо>_cnt           — demo1/demo2, считающий рендер: поток для ПЛИС и площадь (640×480);
  <демо>_cnt_time      — то же без подсчёта: процессорное время подготовки кадра (×3);
  <демо>_soft240/360   — программный рендер id 320×240 и 640×360 (×3);
  fight_<карта>_*      — 60 с боя (бессмертие, всё оружие, бег, стрельба) теми же рендерами.
"""
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(r'E:\zx\Quake2')
EXE = ROOT / 'build/bin/q2m.exe'
BASE = ['+set', 'basedir', 'E:/zx/Quake2/data']
CNT = ['+set', 'vid_ref', 'cnt', '+set', 'gl_mode', '3']
SP_MAPS = ('base1 base2 base3 train bunk1 ware1 ware2 jail1 jail2 jail3 jail4 jail5 security mintro '
           'mine1 mine2 mine3 mine4 fact1 fact2 fact3 power1 power2 cool1 waste1 waste2 waste3 biggun '
           'hangar1 hangar2 lab command strike space city1 city2 city3 boss1 boss2').split()
DM_MAPS = [f'q2dm{i}' for i in range(1, 9)]
FIGHT_MAPS = 'base1 base3 jail4 mine1 fact2 command hangar2 city3'.split()


def run(out, name, scenario, extra):
    t = time.time()
    cmd = [str(EXE), str(out / name)] + scenario + ['--'] + BASE + extra
    r = subprocess.run(cmd, capture_output=True, cwd=EXE.parent)
    text = (r.stdout + r.stderr).decode('utf-8', 'replace').strip().splitlines()
    print(f'{name:32s} {time.time() - t:6.1f} с  {text[-1] if text else ""}', flush=True)
    if r.returncode:
        print('\n'.join(text[-20:]))
        sys.exit(f'{name}: код {r.returncode}')


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'results/run'
    out.mkdir(parents=True, exist_ok=True)
    run(out, 'mem_sp', ['mapstats', ','.join(SP_MAPS)], CNT)
    run(out, 'mem_dm', ['mapstats', ','.join(DM_MAPS)], CNT)
    for demo in ('demo1', 'demo2'):
        run(out, f'{demo}_cnt', ['demo', f'{demo}.dm2'], CNT)
        for i in range(3):
            run(out, f'{demo}_cnt_time{i}', ['demo', f'{demo}.dm2'], CNT + ['+set', 'q2m_count', '0'])
            run(out, f'{demo}_soft240_{i}', ['demo', f'{demo}.dm2'], ['+set', 'vid_ref', 'soft', '+set', 'sw_mode', '0'])
            run(out, f'{demo}_soft360_{i}', ['demo', f'{demo}.dm2'], ['+set', 'vid_ref', 'soft', '+set', 'sw_mode', '10'])
    for m in FIGHT_MAPS:
        run(out, f'fight_{m}_cnt', ['fight', m, '60'], CNT)
        run(out, f'fight_{m}_cnt_time', ['fight', m, '60'], CNT + ['+set', 'q2m_count', '0'])
        run(out, f'fight_{m}_soft240', ['fight', m, '60'], ['+set', 'vid_ref', 'soft', '+set', 'sw_mode', '0'])
        run(out, f'fight_{m}_soft360', ['fight', m, '60'], ['+set', 'vid_ref', 'soft', '+set', 'sw_mode', '10'])


if __name__ == '__main__':
    main()
