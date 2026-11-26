#!/usr/bin/env python3
"""Сборка горячего модуля Q2HOT.ELF (26.11.2026) — после сборки Q2.ELF (esp/build.ps1 зовёт сам).

    python build_hot.py ПАПКА_ВЫВОДА        (та же, что у build.ps1: build\\esp; в ней idf\\compile_commands.json)

1. Ключи компиляции — те же, что у gpu/front/bz_front.c в сборке Q2.ELF (берутся из compile_commands.json):
   типы и раскладка структур у модуля и у Q2.ELF обязаны совпасть (модуль ещё и сверяет размеры — bz_int.h).
   Плюс Q2HOT_MODULE (точка входа main) и папка esp/hot.
2. Связывание и обрезка — как у project_elf прошивки (components/elf_loader/elf_loader.cmake).
3. Проверки:
   - check_module_symbols.py прошивки: все внешние символы — из таблиц прошивки, секции — те, что кладёт загрузчик;
   - неопределённые символы — только функции прошивки (memcpy, деление float …): ссылка на символ Q2.ELF
     значила бы, что в q2hot_remap.h не хватает имени;
   - .text ≤ 16 КБ (code_ram: гарантирован кусок ZM_CODE_RAM_RESERVED);
   - в .text нет таблиц переходов и данных: модуль собран с -fno-jump-tables (ключи Q2.ELF), а данные GCC
     кладёт в .rodata/.data; байтовых чтений из .text (шина команд внутреннего ОЗУ их не умеет) — нет: в
     дизассемблере ищутся l8ui/l16ui/l16si по адресу, взятому литералом из .text.
Итог — ПАПКА\\Q2HOT.ELF и ПАПКА\\hot\\q2hot_dbg.elf (без обрезки — для дизассемблера и профиля).
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]                          # E:/zx/Quake2
VDAC4 = Path(r'E:\zx\Tang Nano 20k VDAC\VDAC4')
CHECK = VDAC4 / 'ESP32 S3 Zero N8R8' / 'Sound' / 'tools' / 'check_module_symbols.py'
CODE_RAM = 16 * 1024


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        print(r.stdout[-4000:], r.stderr[-4000:])
        raise SystemExit(f'не прошло: {cmd if isinstance(cmd, str) else " ".join(map(str, cmd))[:200]}')
    return r.stdout


def main():
    out = Path(sys.argv[1]).resolve()
    idf = out / 'idf'
    hot = out / 'hot'
    hot.mkdir(parents=True, exist_ok=True)

    cc = json.loads((idf / 'compile_commands.json').read_text(encoding='utf-8'))
    ent = next(e for e in cc if e['file'].replace('\\', '/').endswith('gpu/front/bz_front.c'))
    cmd = ent['command']
    i = cmd.rindex(' -o ')
    prefix = cmd[:i]                                                # компилятор и все ключи bz_front.c
    gcc = prefix.split(' ', 1)[0]
    tool = gcc[:-len('gcc.exe')] if gcc.endswith('gcc.exe') else gcc[:-3]

    src = ROOT / 'esp' / 'hot' / 'q2hot.c'
    obj = hot / 'q2hot.o'
    run(f'{prefix} -DQ2HOT_MODULE -I{(ROOT / "esp" / "hot").as_posix()} -o {obj.as_posix()} -c {src.as_posix()}',
        cwd=ent['directory'])

    link = [gcc, '-nostartfiles', '-nostdlib', '-fPIC', '-shared', '-e', 'app_main', '-fdata-sections',
            '-ffunction-sections', '-Wl,--gc-sections', '-fvisibility=hidden']
    dbg = hot / 'q2hot_dbg.elf'
    elf = out / 'Q2HOT.ELF'
    run(link + [str(obj), '-o', str(dbg)])
    run(link + ['-Wl,--strip-all', '-Wl,--strip-debug', '-Wl,--strip-discarded', str(obj), '-o', str(elf)])
    run([tool + 'strip.exe', '--strip-unneeded', '--remove-section=.comment', '--remove-section=.got.loc',
         '--remove-section=.dynamic', '--remove-section=.xt.lit', '--remove-section=.xt.prop',
         '--remove-section=.xtensa.info', str(elf)])

    ok = True
    # 1. символы и секции — проверкой прошивки
    r = subprocess.run([sys.executable, str(CHECK), str(elf)], capture_output=True, text=True, encoding='utf-8',
                       errors='replace')
    print(r.stdout.strip())
    ok &= r.returncode == 0

    # 2. неопределённые символы
    und = run([tool + 'nm.exe', '-u', str(dbg)]).split()
    und = [s for s in und if s not in ('U', 'w')]
    print('неопределённые символы модуля:', ' '.join(und) or '—')

    # 3. размер .text
    sizes = run([tool + 'size.exe', '-A', str(elf)])
    text = int(re.search(r'^\.text\s+(\d+)', sizes, re.M).group(1))
    print(sizes.strip())
    print(f'.text {text} байт из {CODE_RAM} (code_ram)')
    if text > CODE_RAM:
        print('ОШИБКА: .text не помещается во внутреннее ОЗУ')
        ok = False

    # 4. байтовые и 16-битные чтения по адресу из литерала, указывающего в .text
    hdr = run([tool + 'objdump.exe', '-h', str(dbg)])
    m = re.search(r'\s\.text\s+([0-9a-f]+)\s+([0-9a-f]+)', hdr)
    t_lo, t_hi = int(m.group(2), 16), int(m.group(2), 16) + int(m.group(1), 16)
    dis = run([tool + 'objdump.exe', '-d', '-z', str(dbg)])
    tbin = hot / 'text.bin'
    run([tool + 'objcopy.exe', '-O', 'binary', '--only-section=.text', str(dbg), str(tbin)])
    tdata = tbin.read_bytes()

    def literal(addr):
        """Значение литерала по его адресу (литералы лежат в .text)."""
        o = addr - t_lo
        return int.from_bytes(tdata[o:o + 4], 'little') if 0 <= o <= len(tdata) - 4 else None

    lit = {}                                                          # регистр → значение литерала
    bad = []
    for ln in dis.splitlines():
        mm = re.match(r'\s*([0-9a-f]+):\s+[0-9a-f ]+\s+(\w[\w.]*)\s+(.*)', ln)
        if not mm:
            lit.clear()
            continue
        op, args = mm.group(2), mm.group(3)
        if op == 'l32r':
            a = re.match(r'(a\d+),\s*([0-9a-f]+)', args)
            if a:
                lit[a.group(1)] = literal(int(a.group(2), 16))
            continue
        if op in ('l8ui', 'l16ui', 'l16si'):
            a = re.match(r'a\d+,\s*(a\d+),', args)
            v = lit.get(a.group(1)) if a else None
            if v is not None and t_lo <= v < t_hi:
                bad.append(ln.strip())
        # команда пишет в первый регистр (у записей в память он — источник; забыть его — только осторожнее)
        a = re.match(r'(a\d+)\b', args)
        if a:
            lit.pop(a.group(1), None)
    if bad:
        print('ОШИБКА: чтения байт/полуслов по адресу внутри .text:')
        print('\n'.join(bad[:20]))
        ok = False
    print(f'.text {t_lo:#x}–{t_hi:#x}; таблиц переходов нет (-fno-jump-tables в ключах: '
          f'{"да" if "-fno-jump-tables" in prefix else "НЕТ"})')
    ok &= '-fno-jump-tables' in prefix
    print('Q2HOT.ELF:', elf, 'OK' if ok else 'ОШИБКИ')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
