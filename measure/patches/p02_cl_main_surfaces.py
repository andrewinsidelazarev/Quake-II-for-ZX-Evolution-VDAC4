"""Порт VDAC4, этап 1, шаг 1 (дополнение): cl_main.c объявлял map_surfaces массивом.

После p01 таблицы карты — указатели; объявление «extern mapsurface_t map_surfaces[]» заставило бы
код читать сам указатель как данные таблицы (проверка текстур перед скачиванием, CL_RequestNextDownload).
"""
import pathlib

P = pathlib.Path(r'E:\zx\Quake2\port\client\cl_main.c')
raw = P.read_bytes()
crlf = b'\r\n' in raw
t = raw.decode('utf-8').replace('\r\n', '\n')
old = '		extern mapsurface_t	map_surfaces[];\n'
new = '		extern mapsurface_t	*map_surfaces;	// порт VDAC4: таблица карты — указатель (cmodel.c)\n'
assert t.count(old) == 1
t = t.replace(old, new)
P.write_bytes(t.replace('\n', '\r\n' if crlf else '\n').encode('utf-8'))
print('ok')
