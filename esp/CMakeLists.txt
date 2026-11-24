# =====================================================================================================
# Q2.ELF — Quake 2 для ESP32-S3 платы VDAC4: модуль (приложение) прошивки ESP VDAC4.
# =====================================================================================================
# Сборка — esp/build.ps1 (ESP-IDF 6.0 из E:\zx\esp). Как у DOOM VDAC4: проект ESP-IDF с одним
# компонентом main, а project_elf (компонент elf_loader прошивки VDAC4, только чтение) связывает его
# в перемещаемый ELF, который прошивка грузит в PSRAM и связывает со своими таблицами символов.
cmake_minimum_required(VERSION 3.16)
set(VDAC4_ROOT "E:/zx/Tang Nano 20k VDAC/VDAC4" CACHE PATH "дерево VDAC4 (только чтение)")
set(Q2_ROOT "E:/zx/Quake2" CACHE PATH "проект Quake 2")
set(ZM_ROOT "${VDAC4_ROOT}/ESP32 S3 Zero N8R8/Firmware")
set(EXTRA_COMPONENT_DIRS "${ZM_ROOT}/components/elf_loader" "${ZM_ROOT}/components/cmake_utilities")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(q2_vdac4)
include("${ZM_ROOT}/components/elf_loader/elf_loader.cmake")
project_elf(q2_vdac4)
