# =====================================================================================================
# build.ps1 — собрать Q2.ELF (Quake 2 для ESP VDAC4).
# =====================================================================================================
#   powershell -File E:\zx\Quake2\esp\build.ps1 [-Out папка]
#   (-Out — своя папка вывода вместо build\esp, 26.11.2026: две сессии собирают разом и не мешают друг другу)
#
# Шаги:
#   1. measure/patches/p06_ref_esp.py делает рендер ref_esp из port/ref_soft в build/esp/gen/ref_esp
#      (программный рендер id с врезками «бомж-3D», без рисования — то же, что у q2esp.exe на ПК).
#   2. ESP-IDF (E:\zx\esp, окружение — idf-env.ps1) собирает проект esp/ в build/esp/idf и связывает
#      модуль (цель elf: project_elf из компонента elf_loader прошивки VDAC4).
#   3. Копия — build/esp/Q2.ELF; проверка check_module_symbols.py из дерева VDAC4: все внешние символы
#      модуля должны быть в таблицах прошивки, все секции — из тех, что кладёт загрузчик.
#   4. Размеры секций (.text — код, .data/.rodata — данные, .bss — нули при загрузке).
#   5. Горячий модуль Q2HOT.ELF (esp/hot/build_hot.py): код — во внутреннее ОЗУ ESP (code_ram).
# Временные файлы — E:\zx\cache\tmp\quake2 (диск C: не трогаем).
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$esp = $PSScriptRoot
$q2 = Split-Path -Parent $esp
$vdac4 = 'E:\zx\Tang Nano 20k VDAC\VDAC4'
$out = if ($Out) { $Out } else { Join-Path $q2 'build\esp' }

. 'E:\zx\esp\idf-env.ps1'
$env:TMP = 'E:\zx\cache\tmp\quake2'
$env:TEMP = $env:TMP
New-Item -ItemType Directory -Force $env:TMP, $out | Out-Null
$idfpy = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'

& 'C:\Users\andre\AppData\Local\Python\bin\python.exe' "$q2\measure\patches\p06_ref_esp.py" "$q2\port\ref_soft" "$out\gen\ref_esp"
if ($LASTEXITCODE) { throw 'ref_esp не сделан' }

$build = Join-Path $out 'idf'
& $idfpy "$env:IDF_PATH\tools\idf.py" -C $esp -B $build -D IDF_TARGET=esp32s3 -D "VDAC4_ROOT=$vdac4" -D "Q2_GEN=$($out -replace '\\','/')/gen/ref_esp" reconfigure
if ($LASTEXITCODE) { throw 'настройка ESP-IDF не прошла' }
& cmake --build $build --target elf
if ($LASTEXITCODE) { throw 'сборка не прошла' }

Copy-Item (Join-Path $build 'q2_vdac4.app.elf') (Join-Path $out 'Q2.ELF') -Force
& $idfpy "$vdac4\ESP32 S3 Zero N8R8\Sound\tools\check_module_symbols.py" (Join-Path $out 'Q2.ELF')
$symbols = $LASTEXITCODE

$size = Get-ChildItem 'E:\zx\esp\.espressif\tools\xtensa-esp-elf' -Recurse -Filter 'xtensa-esp32s3-elf-size.exe' |
    Select-Object -Last 1
& $size.FullName -A (Join-Path $out 'Q2.ELF')
if ($symbols) { throw 'проверка символов не прошла' }
Write-Host "Q2.ELF: $out\Q2.ELF"

# 5. Горячий модуль Q2HOT.ELF (26.11.2026): те же ключи, что у bz_front.c; проверки — в build_hot.py
& $idfpy "$esp\hot\build_hot.py" $out
if ($LASTEXITCODE) { throw 'Q2HOT.ELF не собран' }
