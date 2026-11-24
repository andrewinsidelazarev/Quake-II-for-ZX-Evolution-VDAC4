"""Порт VDAC4, этап 1, шаг 2: счётчик объектов игры для замеров (сколько из 1024 занято).

Дописывает Q2M_NumEdicts в server/sv_game.c порта; сборка id получает то же из measure/build.py.
"""
import pathlib

P = pathlib.Path(r'E:\zx\Quake2\port\server\sv_game.c')
raw = P.read_bytes()
crlf = b'\r\n' in raw
t = raw.decode('utf-8').replace('\r\n', '\n')
assert 'Q2M_NumEdicts' not in t
t += '''
/* q2m: сколько объектов игры занято сейчас (ge->num_edicts) — замеры перед 512 вместо 1024 */
int Q2M_NumEdicts (void)
{
	return ge ? ge->num_edicts : 0;
}
'''
P.write_bytes(t.replace('\n', '\r\n' if crlf else '\n').encode('utf-8'))
print('ok')
