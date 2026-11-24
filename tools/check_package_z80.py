#!/usr/bin/env python3
"""Проверка собранного пакета (tools/package.py) в эмуляторе Z80 — тем же стендом, что тесты
запускателя VDAC4 (Network/tests/test_esp_app_spg_data_z80.py): настоящий код Q2.SPG и драйвер FAT32
над образом карты Evo, модель канала SPI и «прошивки» ESP.

    E:\\zx\\esp\\pio-venv\\Scripts\\python.exe tools/check_package_z80.py

Проверяется именно эта сборка (E:/zx/cache/tmp/q2esp/package — папка сборки пакета):
  1. с флешкой: в ESP уходят ядро Q2.BIN (флаг 2), Q2.ELF и PAK0…PAK2 (флаг 4) — байт в байт, в этом
     порядке, потом APP_START Q2.ELF;
  2. без флешки (прошивка 0.1.166, ответ 5): pak не передаются, после APP_START куски по запросу —
     байт в байт.
pak на образе карты — заменители (не файлы id): важны длина, кластеры и байты, а не содержимое.
"""
import sys
import unittest
from pathlib import Path

NET_TESTS = Path(r'E:\zx\Tang Nano 20k VDAC\VDAC4\ESP32 S3 Zero N8R8\Network\tests')
sys.path.insert(0, str(NET_TESTS))

import test_esp_app_spg_data_z80 as t                      # noqa: E402

STAGE = Path(r'E:\zx\cache\tmp\q2esp\package')
ELF = (STAGE / 'Q2.ELF').read_bytes()
CORE = (STAGE / 'Q2.BIN').read_bytes()
PAK0 = bytes((i * 29 + (i >> 10)) & 255 for i in range(300_001))
PAK1 = bytes((i * 7 + 3) & 255 for i in range(70_000))
PAK2 = bytes((i * 3 + 1) & 255 for i in range(45_055))     # как у полной версии: 45 055 байт


def card() -> tuple:
    c = t.Card(spc=8)
    games = c.mkdir(2, 'GAMES')
    folder = c.mkdir(games, 'QUAKE2')
    c.put(folder, 'Q2.SPG', b'SPG' * 100)
    c.put(folder, 'Q2.ELF', ELF)
    c.put(folder, 'Q2.BIN', CORE)
    c.put(folder, 'PAK0.PAK', PAK0, fragmented=True)
    c.put(folder, 'PAK1.PAK', PAK1)
    c.put(folder, 'PAK2.PAK', PAK2)
    return c, folder


class PackageTest(unittest.TestCase):
    def test_with_usb_drive(self) -> None:
        c, _ = card()
        esp = t.DataEsp(paths={})
        h = t.DataSpgMachine(STAGE, c, esp).run()
        self.assertEqual(esp.started, 'Q2.ELF')
        self.assertEqual([(q[0], q[3]) for q in esp.queries],
                         [(2, 'Q2.BIN'), (0, 'Q2.ELF'), (4, 'PAK0.PAK'), (4, 'PAK1.PAK'), (4, 'PAK2.PAK')])
        for name, data in (('Q2.BIN', CORE), ('Q2.ELF', ELF), ('PAK0.PAK', PAK0), ('PAK1.PAK', PAK1),
                           ('PAK2.PAK', PAK2)):
            self.assertEqual(bytes(esp.files[name]['data']), data, name)
        self.assertEqual(h.writes, 0)

    def test_without_usb_drive(self) -> None:
        c, _ = card()
        # как просит прошивка: с начала сектора, целыми секторами (кроме хвоста файла), до 32 КБ
        reads = [('PAK0.PAK', 0, 512), ('PAK0.PAK', 4096, 32768), ('PAK2.PAK', 44544, 511),
                 ('PAK1.PAK', 65536, 4464), ('PAK0.PAK', 299520, 481)]
        esp = t.RemoteEsp(reads)
        h = t.RemoteMachine(STAGE, c, esp).run()
        self.assertEqual(esp.started, 'Q2.ELF')
        self.assertEqual(sorted(esp.files), ['Q2.BIN', 'Q2.ELF'], 'pak не передаются')
        self.assertEqual(bytes(esp.files['Q2.ELF']['data']), ELF)
        self.assertEqual(bytes(esp.files['Q2.BIN']['data']), CORE)
        files = {'PAK0.PAK': PAK0, 'PAK1.PAK': PAK1, 'PAK2.PAK': PAK2}
        self.assertEqual(len(esp.results), len(reads))
        for what, offset, data in esp.results:
            self.assertEqual(data, files[what][offset:offset + len(data)], (what, offset))
        self.assertEqual(h.writes, 0)


if __name__ == '__main__':
    unittest.main()
