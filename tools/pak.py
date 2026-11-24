"""Список и извлечение файлов из PAK Quake 2.

python pak.py list <pak> [фильтр]      — имена и размеры
python pak.py get <pak> <имя> <куда>   — извлечь один файл
"""
import struct
import sys


def entries(path):
    with open(path, 'rb') as f:
        magic, ofs, length = struct.unpack('<4sii', f.read(12))
        assert magic == b'PACK', path
        f.seek(ofs)
        raw = f.read(length)
    for i in range(0, length, 64):
        name, pos, size = struct.unpack('<56sii', raw[i:i + 64])
        yield name.split(b'\0')[0].decode('latin-1'), pos, size


def main():
    cmd, pak = sys.argv[1], sys.argv[2]
    if cmd == 'list':
        flt = sys.argv[3] if len(sys.argv) > 3 else ''
        for name, _, size in entries(pak):
            if flt in name:
                print(f'{size:10d} {name}')
    elif cmd == 'get':
        want, out = sys.argv[3], sys.argv[4]
        for name, pos, size in entries(pak):
            if name == want:
                with open(pak, 'rb') as f:
                    f.seek(pos)
                    open(out, 'wb').write(f.read(size))
                return
        sys.exit('нет ' + want)


if __name__ == '__main__':
    main()
