"""Замеры на демо-версии Quake 2 (data-demo: карты demo1–demo3, запись q2demo1.dm2): прогон и сводка.

python demo_runs.py    — results\\demo: память по картам, запись и бой 60 с на demo1 (поток, время)
"""
import pathlib
import statistics
import subprocess
import sys

ROOT = pathlib.Path(r'E:\zx\Quake2')
OUT = ROOT / 'results/demo'
EXE = ROOT / 'build/bin/q2m.exe'
BASE = ['+set', 'basedir', 'E:/zx/Quake2/data-demo']
CNT = ['+set', 'vid_ref', 'cnt', '+set', 'gl_mode', '3']
SOFT = ['+set', 'vid_ref', 'soft', '+set', 'sw_mode', '0']


def run(name, scen, extra):
    r = subprocess.run([str(EXE), str(OUT / name)] + scen + ['--'] + BASE + extra, capture_output=True, cwd=EXE.parent)
    last = (r.stdout + r.stderr).decode('utf-8', 'replace').strip().splitlines()
    print(f'{name:22s} код {r.returncode}  {last[-1] if last else ""}')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    run('fight_cnt', ['fight', 'demo1', '60'], CNT)
    run('fight_cnt_time', ['fight', 'demo1', '60'], CNT + ['+set', 'q2m_count', '0'])
    run('fight_soft240', ['fight', 'demo1', '60'], SOFT)
    run('rec_cnt_time', ['demo', 'q2demo1.dm2'], CNT + ['+set', 'q2m_count', '0'])
    run('rec_soft240', ['demo', 'q2demo1.dm2'], SOFT)

    sys.argv = ['analyze.py', str(OUT)]
    sys.path.insert(0, str(ROOT / 'tools'))
    import analyze
    analyze.RUN = OUT
    MB = 1024 * 1024
    rows = analyze.mem('mem')
    print('\nпамять по картам демо, МБ:')
    for r in rows:
        esp = r['cmodel'] + r['world'] + r['models'] + r['snd_11k8'] + r['game'] + r['zone_other']
        sd = r['tex_wall'] + r['tex_skin'] + r['tex_sprite'] + r['tex_pic'] + r['tex_sky'] + r['lux_all']
        print(f"  {r['map']:6s} ESP данные (звук 11к/8) {esp / MB:5.2f}  SDRAM текстуры+lightmap {sd / MB:5.2f}")
    for scene, cnt, tm, soft in (('запись q2demo1', 'rec_cnt', ['rec_cnt_time'], ['rec_soft240']),
                                 ('бой demo1', 'fight_cnt', ['fight_cnt_time'], ['fight_soft240'])):
        st = analyze.frames(cnt)
        t1, t2 = analyze.times(tm), analyze.times(soft)
        print(f'\n{scene}: кадров {st["frames"]}; многоугольников ср {st["prims3d"]["mean"]:.0f} p99 '
              f'{st["prims3d"]["p99"]:.0f}; перерисовка без вспышек ср {st["overdraw_noblend"]["mean"]:.2f} '
              f'p99 {st["overdraw_noblend"]["p99"]:.2f}; поток B ср {st["bytes_B"]["mean"] / 1024:.0f} '
              f'p99 {st["bytes_B"]["p99"] / 1024:.0f} КБ')
        print(f'  ПК мкс: для ПЛИС без подготовки ср {t1["total_us"]["mean"]:.0f} p99 {t1["total_us"]["p99"]:.0f}; '
              f'программный 320×240 ср {t2["total_us"]["mean"]:.0f} p99 {t2["total_us"]["p99"]:.0f}')


if __name__ == '__main__':
    main()
