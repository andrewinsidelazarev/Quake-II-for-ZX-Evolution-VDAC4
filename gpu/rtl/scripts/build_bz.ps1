# =====================================================================================================
# build_bz.ps1 — собрать ядро «бомж-3D» в Gowin в отдельной стадии на диске E:.
# =====================================================================================================
#   powershell -File gpu\rtl\scripts\build_bz.ps1 [-StageA [-No3D]] [-Stage E:\zx\cache\tmp\q2gpu_build]
#
# Что делает по шагам:
#   1. Создаёт стадию <Stage>\source (старую стадию удаляет целиком — только свою папку).
#   2. Копирует туда RTL (gpu\rtl\*.sv, gpu\rtl\vdac2\*.sv), ограничения и gowin_bz.tcl.
#   3. По умолчанию рисует этап B (bz_gpu). С ключом -StageA дописывает в начало копии bz_top.sv строку
#      `define BZ_STAGEA (ядро этапа A), с -No3D ещё и в копию bz_core.sv `define BZ_NO3D (без 3D).
#   4. Запускает gw_sh с TMP/TEMP на E: (диск C: маленький — временные файлы Gowin туда не пишем);
#      журнал — <Stage>\build.log, в конце строка «exit N» (N = 0 — сборка прошла).
# Итог Gowin — <Stage>\source\impl\pnr\bz_top.fs/.bin, отчёты .rpt.txt (заполнение) и .tr (время).
param(
    [switch]$StageA,
    [switch]$No3D,
    [string]$Stage = 'E:\zx\cache\tmp\q2gpu_build'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)     # E:\zx\Quake2\gpu
$gw = 'C:\Gowin\Gowin_V1.9.11.03_Education_x64\IDE\bin\gw_sh.exe'

if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
$src = Join-Path $Stage 'source'
New-Item -ItemType Directory -Force (Join-Path $src 'rtl\vdac2') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $src 'constraints') | Out-Null
Copy-Item (Join-Path $root 'rtl\*.sv') (Join-Path $src 'rtl')
Copy-Item (Join-Path $root 'rtl\vdac2\*.sv') (Join-Path $src 'rtl\vdac2')
Copy-Item (Join-Path $root 'rtl\constraints\*') (Join-Path $src 'constraints')
Copy-Item (Join-Path $root 'rtl\scripts\gowin_bz.tcl') $src

# дописать ключ в начало файла байтами: файл в UTF-8, концы строк — как были
function Add-Define([string]$file, [string]$name) {
    $f = Join-Path $src $file
    $bytes = [System.IO.File]::ReadAllBytes($f)
    $head = [System.Text.Encoding]::UTF8.GetBytes("``define $name`n")
    [System.IO.File]::WriteAllBytes($f, $head + $bytes)
}
if ($StageA) { Add-Define 'rtl\bz_top.sv' 'BZ_STAGEA' }
if ($StageA -and $No3D) { Add-Define 'rtl\bz_core.sv' 'BZ_NO3D' }

$env:TMP = 'E:\zx\cache\tmp'
$env:TEMP = 'E:\zx\cache\tmp'
Set-Location $src
$log = Join-Path $Stage 'build.log'
& $gw gowin_bz.tcl *> $log
"exit $LASTEXITCODE" | Out-File -Append -Encoding utf8 $log
