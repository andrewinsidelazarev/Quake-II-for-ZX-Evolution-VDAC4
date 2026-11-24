"""Одинаковые глобальные имена в разных частях программы q2esp (движок, игра, рендер).

ЗАЧЕМ
  У id движок, игра (gamex86.dll) и рендер (ref_*.dll) — разные библиотеки, и у каждой свои глобальные
  имена. В q2esp (и на ESP) они собраны в одну программу. Одноимённые ФУНКЦИИ связыватель заметит сам
  (ошибка «уже определено»), а одноимённые ПЕРЕМЕННЫЕ без начального значения (в C это «общие» символы,
  common) MSVC молча сливает в одну: если игра и движок объявили каждый свою `cvar_t *x`, они станут
  одной переменной — ошибка без предупреждения. GCC для ESP по умолчанию (-fno-common) такое не
  свяжет, но лучше увидеть заранее.

КАК
  dumpbin /symbols по .obj каждой группы (build/port/obj/q2esp_eng, _game, _ref); глобальный символ
  определён группой, если он лежит в её секции (SECTn) или он «общий» (UNDEF с ненулевым размером).
  Печатаются имена, определённые больше чем одной группой.

  python tools/symclash.py
"""
import collections
import pathlib
import re
import subprocess
import sys

OBJ = pathlib.Path(r'E:\zx\Quake2\build\port\obj')
TMP = pathlib.Path(r'E:\zx\cache\tmp\quake2')
VCVARS = pathlib.Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat')
GROUPS = ('q2esp_eng', 'q2esp_game', 'q2esp_ref')

# строка dumpbin: «008 00000000 SECT3  notype       External     | _name» или «… UNDEF  notype  External | _x»
# встроенные (inline) функции из stdio.h Windows: каждая единица компиляции несёт свою одинаковую копию
CRT_INLINE = re.compile(r'^(\?_OptionsStorage@.*|___local_stdio_\w+|__v\w+_l|_v?f?s?n?printf|_v?s?scanf)$')
LINE = re.compile(r'^[0-9A-F]+ ([0-9A-F]+) (SECT[0-9A-F]+|UNDEF)\s+(?:notype|\w+)(?: \(\))?\s+External\s+\| (\S+)')


def main():
    TMP.mkdir(parents=True, exist_ok=True)
    defined = collections.defaultdict(set)     # имя → группы, где определено
    kinds = {}
    for g in GROUPS:
        out = TMP / f'{g}.sym.txt'
        bat = TMP / 'symclash.bat'
        bat.write_text(f'@echo off\r\ncall "{VCVARS}" >nul || exit /b 1\r\n'
                       f'dumpbin /nologo /symbols "{OBJ / g}\\*.obj" > "{out}"\r\n', encoding='utf-8')
        subprocess.run(['cmd.exe', '/d', '/c', str(bat)], check=True)
        for ln in out.read_text('cp866', 'replace').splitlines():
            m = LINE.match(ln)
            if not m:
                continue
            value, sect, name = m.groups()
            if sect == 'UNDEF' and int(value, 16) == 0:
                continue                         # просто ссылка на чужое имя
            if name.startswith(('??', '__real@', '__xmm@', '$')):
                continue                         # служебные имена компилятора
            if CRT_INLINE.match(name):
                continue                         # встроенные функции stdio.h (COMDAT): копии одинаковые
            defined[name].add(g)
            kinds[name] = 'common' if sect == 'UNDEF' else kinds.get(name, 'sect')
    clash = sorted(n for n, gs in defined.items() if len(gs) > 1)
    for n in clash:
        print(f'{n:40s} {kinds[n]:7s} {" ".join(sorted(defined[n]))}')
    print(f'имён в двух и более группах: {len(clash)}')
    return 1 if clash else 0


if __name__ == '__main__':
    sys.exit(main())
