#!/usr/bin/env python3
"""Пакет Quake 2 для карты Evo: Q2.SPG (запускатель ESP App SPG), Q2.ELF, Q2.BIN — в dist/Q2.

    python tools/package.py [--core путь/к/bz_top.bin] [--paks PAK0.PAK,PAK1.PAK,PAK2.PAK]

ЧТО В ПАКЕТЕ И ЗАЧЕМ
  Q2.SPG — общий запускатель приложений ESP (VDAC4 «Network/ESP App SPG», build.bat ИМЯ.ELF ЯДРО.BIN
  ДАННЫЕ…). Внутри — сам Q2.ELF и ядро ПЛИС Q2.BIN («бомж-3D», gpu/rtl). Запускатель:
    1. находит на карте Evo папку, где лежат копия Q2.ELF и PAK0.PAK (папку помнит ESP — path.cfg);
    2. шлёт в ESP ядро (флаг 2: Evo → ESP32 → LINK → BL616 → ПЛИС, мимо microSD Tang — правило
       пользователя 23.11), потом ELF;
    3. pak-файлы: есть флешка USB у ESP — копирует их туда один раз (/SYSTEM/CACHE/Q2/), нет флешки
       (прошивка 0.1.166) — оставляет на карте и, пока игра идёт, отдаёт ESP их куски по запросу;
    4. APP_START — Q2.ELF грузит ядро и начинает игру; любая клавиша Evo — выход (ввода пока нет).
  Q2.ELF и Q2.BIN рядом с SPG — копии: по Q2.ELF запускатель узнаёт папку, Q2.BIN — для справки.
  pak-файлы в пакет не входят: это файлы id Software, их кладёт сам владелец игры (полная версия —
  PAK0.PAK, PAK1.PAK, PAK2.PAK из baseq2; демо — только PAK0.PAK, тогда --paks PAK0.PAK).

КАК СОБИРАЕТСЯ (как build.bat запускателя, но во временной папке: сборки в репозитории VDAC4 не
трогаются)
  - образ ядра проверяется теми же правилами, что у прошивки ESP и BL616 (core_cache_client.cpp):
    синхрослово A5C3 и IDCODE GW2AR-18, адрес перехода multiboot 0, метка VDAC4 в UserCode;
  - prep.py --out (куски ELF и ядра по 16 КБ, app_cfg.inc, spgbld.ini), sjasmplus над копией
    esp_app.asm, spgbld (из ZiFi SPG/_spg, текущая папка — ESP App SPG/_spg: пути блоков в ini — от неё);
  - итог и SHA-256 — в dist/Q2, рядом README.txt: что куда класть на карту.
Временное — в E:/zx/cache/tmp/q2esp/package (диск C: не трогается).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]                        # E:/zx/Quake2
VDAC4_NET = Path(r'E:\zx\Tang Nano 20k VDAC\VDAC4\ESP32 S3 Zero N8R8\Network')
SPG_SRC = VDAC4_NET / 'ESP App SPG'
FAT32 = VDAC4_NET / 'shared' / 'fat32'
SPGBLD_DIR = VDAC4_NET / 'ZiFi SPG' / '_spg'
SJASM = Path(os.environ.get('SJASMPLUS', r'E:\zx\z80\zuma\sjasmplus.exe'))
STAGE = Path(r'E:\zx\cache\tmp\q2esp\package')
# Ядро 0.02 (24.11.2026, SPEC 0.4: кадры md2 — в SDRAM ядра, чтение по LINK) — выпуск с README и отчётами
# Gowin в gpu/rtl/release (раньше — сборка d прямо из стадии в кэше, ядро 0.01)
# Ядро 0.03 (24.11.2026): FIFO_FREE в коде Грея (LINK 0x1C), запись цвета в F без adv — тайминг
DEFAULT_CORE = ROOT / 'gpu' / 'rtl' / 'release' / '0.03' / 'Q2.BIN'
DIST = ROOT / 'dist' / 'Q2'


def check_core(bits: bytes) -> str:
    """Те же проверки, что checkImage прошивки: иначе ESP ответит «не ядро» (3) и ELF не уйдёт."""
    head, tail = bits[:128], bits[-64:]
    i = next((k for k in range(64) if head[k] == 0xA5 and head[k + 1] == 0xC3), None)
    if i is None or head[i + 2:i + 10] != bytes([6, 0, 0, 0, 0, 0, 8, 0x1B]):
        raise SystemExit('ядро: не битстрим GW2AR-18 (нет синхрослова или IDCODE)')
    j, jump = i + 10, None
    while j + 8 <= 128 and head[j] != 0x3B:
        if head[j:j + 4] == bytes([0xD2, 0, 0xFF, 0xFF]):
            jump = head[j + 4:j + 8]
            break
        j += 4
    if jump is None or any(jump):
        raise SystemExit('ядро: адрес перехода multiboot не 0 — не вернулось бы в слот 0')
    for k in range(57):
        if tail[k] == 0x0A and tail[k + 1:k + 4] == b'\0\0\0' and tail[k + 4:k + 7] == b'VD4':
            return 'VD4' + chr(tail[k + 7])
    raise SystemExit('ядро: нет метки VDAC4 в UserCode')


def run(args, cwd, env=None):
    result = subprocess.run([str(a) for a in args], cwd=cwd, env=env, capture_output=True)
    log = (result.stdout + result.stderr).decode('utf-8', errors='replace')
    if result.returncode != 0:
        raise SystemExit(f'{args[0]}: ошибка\n{log}')
    return log


def copy(src: Path, dst: Path) -> None:
    """Копия со временем оригинала (оно уходит в MOD_QUERY), но без «только чтение»: Gowin пишет
    bz_top.bin так, и следующая сборка не смогла бы его перезаписать или удалить."""
    if dst.exists():
        dst.chmod(0o644)
    shutil.copy2(src, dst)
    dst.chmod(0o644)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--core', type=Path, default=DEFAULT_CORE)
    ap.add_argument('--elf', type=Path, default=ROOT / 'build' / 'esp' / 'Q2.ELF')
    ap.add_argument('--paks', default='PAK0.PAK,PAK1.PAK,PAK2.PAK')
    a = ap.parse_args()
    paks = [p.strip().upper() for p in a.paks.split(',') if p.strip()]

    env = dict(os.environ, TMP=str(STAGE.parent), TEMP=str(STAGE.parent), SJASMPLUS=str(SJASM))
    shutil.rmtree(STAGE, ignore_errors=True)
    (STAGE / 'build').mkdir(parents=True)
    # исходники пакета: имена — как на карте (Q2.ELF, Q2.BIN), время файлов — как у оригиналов
    elf = STAGE / 'Q2.ELF'
    core = STAGE / 'Q2.BIN'
    copy(a.elf, elf)
    copy(a.core, core)
    mark = check_core(core.read_bytes())

    run([sys.executable, FAT32 / 'build.py'], cwd=FAT32, env=env)          # драйвер FAT32, если устарел
    print(run([sys.executable, SPG_SRC / 'prep.py', elf, core, *paks, '--out', STAGE / 'build'],
              cwd=SPG_SRC, env=env).strip())
    shutil.copyfile(SPG_SRC / 'esp_app.asm', STAGE / 'esp_app.asm')       # INCLUDE "build/…" — рядом
    run([SJASM, '--nologo', f'--inc={VDAC4_NET / "shared" / "z80"}', f'--inc={FAT32 / "include"}',
         f'--inc={SPG_SRC}', '--sym=build/esp_app.sym', 'esp_app.asm'], cwd=STAGE, env=env)
    spg_cwd = SPG_SRC / '_spg'
    spg_cwd.mkdir(exist_ok=True)
    before = set(spg_cwd.iterdir())
    env_spg = dict(env, PATH=str(SPGBLD_DIR) + os.pathsep + env.get('PATH', ''))
    run([SPGBLD_DIR / 'spgbld.exe', '-b', STAGE / 'build' / 'spgbld.ini', STAGE / 'build' / 'Q2.SPG'],
        cwd=spg_cwd, env=env_spg)
    left = set(spg_cwd.iterdir()) - before
    if left:
        print('spgbld оставил в _spg:', ', '.join(p.name for p in left))

    DIST.mkdir(parents=True, exist_ok=True)
    for name in ('Q2.SPG', 'Q2.ELF', 'Q2.BIN'):
        src = STAGE / 'build' / name if name == 'Q2.SPG' else STAGE / name
        copy(src, DIST / name)
    lines = [f'{sha(DIST / n)}  {n}  {(DIST / n).stat().st_size}' for n in ('Q2.SPG', 'Q2.ELF', 'Q2.BIN')]
    (DIST / 'SHA256.txt').write_bytes(('\n'.join(lines) + '\n').encode('ascii'))
    readme = f'''Quake 2 для VDAC4 (ZX Evolution + ESP32-S3 + Tang Nano 20K) — пакет для карты Evo

На карту Evo — в одну папку (любую, кроме корня), например GAMES/QUAKE2:
  Q2.SPG   запускатель (его и запускать из Wild Commander)
  Q2.ELF   игра для ESP (по нему запускатель узнаёт папку)
  Q2.BIN   ядро ПЛИС «бомж-3D» (то же, что внутри Q2.SPG)
  {"  ".join(paks)}   из папки baseq2 вашей копии Quake 2 (в пакет не входят)

Нужно: прошивка ESP vdac4-0.1.166 или новее. Флешка USB в разъёме OTG ESP — по желанию: с ней pak
копируется туда один раз (/SYSTEM/CACHE/Q2) и уровни грузятся за секунды; без неё pak читается с
карты Evo по ходу игры — загрузка уровня около минуты-полутора.

Бордюр: синий — связь и передача, голубой/белый — копирование pak на флешку, зелёный — игра
работает (любая клавиша — выход), красный — ошибка, жёлтый — прошивка ESP старше 0.1.166 без флешки.

Ядро: {mark}, {core.stat().st_size} байт. SHA-256 — SHA256.txt. С ядром 0.02 и новее кадры анимации
моделей лежат в SDRAM ядра (память ESP свободнее); со старым ядром игра держит их в памяти ESP, как раньше.
'''
    (DIST / 'README.txt').write_bytes(readme.replace('\n', '\r\n').encode('utf-8'))
    print('\n'.join(lines))
    print(f'пакет: {DIST}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
