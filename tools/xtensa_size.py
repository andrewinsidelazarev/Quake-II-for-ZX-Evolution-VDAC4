"""Размер кода и статических данных Quake 2 для ESP32-S3 (GCC Xtensa из ESP-IDF, -O2).

python xtensa_size.py [папка исходников]   (по умолчанию id-source; port — наш движок)
Компилирует исходники (C_ONLY) по группам: движок (сервер, клиент, звук, общее),
игра (gamex86), программный рендер ref_soft. Платформенные файлы не берутся. Печатает .text/.data/.bss
по группам и самые большие статические массивы. Объектные файлы — в E:\\zx\\cache\\tmp\\quake2\\xtensa.
"""
import pathlib
import subprocess
import sys

SRC = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(r'E:\zx\Quake2\id-source')
OUT = pathlib.Path(r'E:\zx\cache\tmp\quake2\xtensa') / SRC.name
# В порте стоят счётчики замеров (Q2M_BEGIN/END) — им нужен q2m.h, как у сборки MSVC (/FI)
EXTRA = [] if SRC.name == 'id-source' else ['-include', r'E:\zx\Quake2\measure\q2m.h']
BIN = pathlib.Path(r'E:\zx\esp\.espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin')
GCC = BIN / 'xtensa-esp32s3-elf-gcc.exe'
SIZE = BIN / 'xtensa-esp32s3-elf-size.exe'
NM = BIN / 'xtensa-esp32s3-elf-nm.exe'

GROUPS = {
    'engine': ['client/' + n for n in 'cl_cin cl_ents cl_fx cl_input cl_inv cl_main cl_newfx cl_parse cl_pred '
                                       'cl_scrn cl_tent cl_view console keys menu qmenu snd_dma snd_mem snd_mix'.split()]
              + ['qcommon/' + n for n in 'cmd cmodel common crc cvar files md4 net_chan pmove'.split()]
              + ['server/' + n for n in 'sv_ccmds sv_ents sv_game sv_init sv_main sv_send sv_user sv_world'.split()]
              + ['game/q_shared', 'game/m_flash'],
    'game': [f'game/{p.stem}' for p in sorted((SRC / 'game').glob('*.c')) if p.stem not in ('q_shared', 'm_flash')],
    'ref_soft': [f'ref_soft/{p.stem}' for p in sorted((SRC / 'ref_soft').glob('*.c'))],
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    flags = ['-std=gnu99', '-O2', '-mlongcalls', '-ffunction-sections', '-fdata-sections', '-DC_ONLY', '-fpermissive', '-w', '-c'] + EXTRA
    total = {}
    biggest = []
    for group, files in GROUPS.items():
        objs, failed = [], []
        for rel in files:
            src = SRC / f'{rel}.c'
            obj = OUT / f'{group}_{rel.replace("/", "_")}.o'
            flags_src = []
            if rel == 'game/g_items':
                # MSVC прощал «static» после «extern» в g_local.h; GCC — нет. Для замера размера — копия без static.
                text = src.read_text(encoding='latin-1').replace('static int\t', 'int\t')
                src = OUT / 'g_items.c'
                src.write_text(text, encoding='latin-1')
                flags_src = [f'-I{SRC / "game"}']
            r = subprocess.run([str(GCC), *flags, *flags_src, str(src), '-o', str(obj)], capture_output=True, text=True)
            if r.returncode:
                failed.append((rel, next((ln for ln in r.stderr.splitlines() if 'error' in ln), '?')))
            else:
                objs.append(obj)
        r = subprocess.run([str(SIZE), '-t', *map(str, objs)], capture_output=True, text=True)
        last = r.stdout.strip().splitlines()[-1].split()
        total[group] = tuple(int(x) for x in last[:3])
        print(f'{group:9s} text {total[group][0]:8d}  data {total[group][1]:7d}  bss {total[group][2]:8d}  '
              f'({len(objs)} файлов)')
        for rel, err in failed:
            print(f'   не собран {rel}: {err[:120]}')
        r = subprocess.run([str(NM), '-S', '--size-sort', '-t', 'd', *map(str, objs)], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[2] in 'bBdD':
                biggest.append((int(parts[1]), parts[3], group))
    print('\nсамые большие статические массивы (байт):')
    for size, name, group in sorted(biggest, reverse=True)[:25]:
        print(f'{size:9d}  {name:28s} {group}')


if __name__ == '__main__':
    sys.exit(main())
