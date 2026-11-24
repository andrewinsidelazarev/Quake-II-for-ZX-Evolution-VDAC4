"""Hor+ для широкого экрана: fov из игры (90) считается горизонтальным углом для 4:3, а на 16:9
кадр расширяется вбок, вертикальный угол остаётся как у 4:3 (на 424×240: 106° по горизонтали,
73,7° по вертикали). Так делают все порты Quake 2 под широкий экран. На 4:3 (320×240) формула
даёт ровно прежний угол, поэтому кадры id в регрессии (tools/regress.py) не меняются.

Правится port/client/cl_view.c (наш движок), байтами, концы строк сохраняются. Повторный запуск
ничего не делает.
"""
import pathlib

PATH = pathlib.Path(r'E:\zx\Quake2\port\client\cl_view.c')
OLD = b'\t\tcl.refdef.fov_y = CalcFov (cl.refdef.fov_x, cl.refdef.width, cl.refdef.height);'
NEW = ('\t\t/* Hor+ (VDAC4, 16:9): fov игры — это угол для 4:3; шире экран — шире угол по горизонтали,\n'
       '\t\t   вертикальный остаётся прежним (measure/patches/p05_hor_plus.py) */\n'
       '\t\tif (cl.refdef.width * 3 != cl.refdef.height * 4)\n'
       '\t\t\tcl.refdef.fov_x = atan (tan (cl.refdef.fov_x * M_PI / 360) * (cl.refdef.width * 3.0) /\n'
       '\t\t\t                        (cl.refdef.height * 4.0)) * 360 / M_PI;\n'
       '\t\tcl.refdef.fov_y = CalcFov (cl.refdef.fov_x, cl.refdef.width, cl.refdef.height);').encode('utf-8')

raw = PATH.read_bytes()
crlf = b'\r\n' in raw
t = raw.replace(b'\r\n', b'\n')
if b'p05_hor_plus' in t:
    print('уже стоит')
else:
    assert t.count(OLD) == 1
    t = t.replace(OLD, NEW)
    PATH.write_bytes(t.replace(b'\n', b'\r\n') if crlf else t)
    print('ok')
