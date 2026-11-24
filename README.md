# Quake II для ZX Evolution + VDAC4

Порт Quake II на ZX Evolution с платой VDAC4: ESP32-S3 (N8R8) считает игру и готовит кадр, ПЛИС Tang Nano 20K
рисует его своим ядром «бомж-3D» и выводит на HDMI, Z80 Evo запускает всё и читает файлы с карты.

Port of Quake II to ZX Evolution with the VDAC4 board: the ESP32-S3 runs the game and prepares frames, the
Tang Nano 20K FPGA draws them with its own 3D core and outputs HDMI. Game data (pak files) is not included.

## Что где

| Папка | Что | Лицензия |
|---|---|---|
| `port/` | исходники Quake II (id Software), изменённые под ESP32-S3 | GPL v2 (`port/COPYING`) |
| `esp/` | модуль Q2.ELF для прошивки ESP VDAC4 (ESP-IDF): LINK, видео, ввод, файлы | GPL v2 (`esp/COPYING`) |
| `gpu/front/` | связка рендера Quake II с ядром ПЛИС | GPL v2 (`gpu/front/COPYING`) |
| `measure/` | сборка Quake II на ПК для замеров памяти и времени, заплатки рендера | GPL v2 (`measure/COPYING`) |
| `dist/Q2/` | готовый пакет: Q2.SPG (запускатель), Q2.ELF, Q2.BIN | GPL v2 (`dist/COPYING`) |
| `gpu/rtl/` | ядро ПЛИС «бомж-3D» (SystemVerilog, Gowin), стенды Verilator, выпуски `release/` | MIT (`LICENSE`) |
| `gpu/model/`, `gpu/SPEC.md` | модель ядра на C и формат команд | MIT |
| `tools/` | сборка пакета, разбор трасс и замеров | MIT |
| `GPU.md`, `PLAN.md`, `REPORT.md`, `PROGRESS.md` | устройство, план, отчёт, журнал работы | MIT |

Q2.ELF собирается из кода `port/` и потому распространяется под GPL v2, как и пакет `dist/Q2`.
Оригинальные исходники: [id-Software/Quake-2](https://github.com/id-Software/Quake-2).

## Что нужно для игры

- ZX Evolution с VDAC4, карта Evo с Wild Commander;
- прошивка ESP VDAC4 **0.1.170 или новее** (на 0.1.164–0.1.169 первая загрузка нового ядра роняет ESP);
- `PAK0.PAK`, `PAK1.PAK` (и `PAK2.PAK`, если есть) из папки `baseq2` вашей копии Quake II — в одну папку с
  Q2.SPG и Q2.ELF (см. `dist/Q2/README.txt`).

## Состояние

Демо base2 и waste2 проходят на плате (ядро 0.02) и в эмуляторе Unreal NG. Ядро 0.03 (FIFO_FREE в коде Грея,
`gpu/rtl/release/0.03`) на плате загружается, игра на нём ещё проверяется. Подробности — `PROGRESS.md`.
