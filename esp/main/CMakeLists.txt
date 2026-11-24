# =====================================================================================================
# Компонент main модуля Q2.ELF: движок, игра и рендер Quake 2 (из E:\zx\Quake2\port) + платформа ESP.
# =====================================================================================================
# Три группы, как у q2esp.exe на ПК (measure/build.py): движок, игра, рендер ref_esp. У id это разные
# библиотеки; здесь — одна программа, поэтому:
#   GAME_HARD_LINKED, REF_HARD_LINKED — id сам убирает из игры и рендера запасные Sys_Error/Com_Printf;
#   одноимённые глобальные переменные (tools/symclash.py) у игры и рендера переименованы ключами;
#   q_shared.c и m_flash.c (есть и у игры) — одна копия, в движке.
# Рендер ref_esp — файлы, сделанные measure/patches/p06_ref_esp.py (build.ps1 кладёт их в
# build/esp/gen/ref_esp), плюс передний край gpu/front/bz_front.c с BZ_ONLY и BZ_ESP.
# Во все файлы Quake 2 подставляются compat.h (недостающая libc модуля) и measure/q2m.h (метки замера).

set(Q2 "${Q2_ROOT}/port")
set(GEN "${Q2_ROOT}/build/esp/gen/ref_esp")

set(ENGINE
    client/cl_cin.c client/cl_ents.c client/cl_fx.c client/cl_input.c client/cl_inv.c client/cl_main.c
    client/cl_newfx.c client/cl_parse.c client/cl_pred.c client/cl_scrn.c client/cl_tent.c client/cl_view.c
    client/console.c client/keys.c client/menu.c client/qmenu.c client/snd_dma.c client/snd_mem.c
    client/snd_mix.c
    qcommon/cmd.c qcommon/cmodel.c qcommon/common.c qcommon/crc.c qcommon/cvar.c qcommon/files.c
    qcommon/md4.c qcommon/net_chan.c qcommon/pmove.c
    server/sv_ccmds.c server/sv_ents.c server/sv_game.c server/sv_init.c server/sv_main.c server/sv_send.c
    server/sv_user.c server/sv_world.c
    game/q_shared.c game/m_flash.c
    null/cd_null.c null/snddma_null.c)
list(TRANSFORM ENGINE PREPEND "${Q2}/")

file(GLOB GAME "${Q2}/game/*.c")
list(REMOVE_ITEM GAME "${Q2}/game/q_shared.c" "${Q2}/game/m_flash.c")

file(GLOB REF "${GEN}/*.c")
list(APPEND REF "${Q2_ROOT}/gpu/front/bz_front.c" "${CMAKE_CURRENT_LIST_DIR}/swimp_esp.c")

# compat.c — сама замена libc: compat.h подключает себе сам, с Q2ESP_COMPAT_IMPL (без макросов-подмен);
# подставлять его ключом -include ей нельзя — макросы встали бы раньше её #define
set(PLATFORM q2_app.c sys_esp.c vid_esp.c in_esp.c net_esp.c store_esp.c bz_link.c)

idf_component_register(SRCS ${PLATFORM} compat.c ${ENGINE} ${GAME} ${REF}
                       INCLUDE_DIRS "." "${Q2_ROOT}/measure")

set(FORCE "-include;${CMAKE_CURRENT_LIST_DIR}/compat.h;-include;${Q2_ROOT}/measure/q2m.h")
# код id 1997 года: предупреждений сотни, ошибками их не считать; GCC 14+ по умолчанию считает ошибкой
# несовместимые указатели и неявные объявления — для кода id это прежние предупреждения (-fpermissive)
set(QUIET "-Wno-error;-w;-fpermissive;-Wno-incompatible-pointer-types;-Wno-int-conversion;-Wno-implicit-function-declaration;-Wno-implicit-int;-Wno-return-mismatch")

set_source_files_properties(${ENGINE} ${PLATFORM} PROPERTIES COMPILE_OPTIONS "${FORCE}")
set_source_files_properties(${ENGINE} PROPERTIES COMPILE_OPTIONS "${FORCE};${QUIET}")
set_source_files_properties(${GAME} PROPERTIES COMPILE_OPTIONS
    "${FORCE};${QUIET};-Ddedicated=game_dedicated;-Dmaxclients=game_maxclients")
set_source_files_properties(${REF} PROPERTIES COMPILE_OPTIONS
    "${FORCE};${QUIET};-Dvid_fullscreen=r_vid_fullscreen;-Dvid_gamma=r_vid_gamma;-DBZ_ONLY;-DBZ_ESP;-DBZ_TEX_RESIDENT;-I${GEN};-I${Q2}/ref_soft;-I${Q2_ROOT}/gpu/front;-I${Q2_ROOT}/gpu/model")

# NDEBUG — как у сборки ПК (measure/build.py): assert id выключены
# MAX_PARTICLES 3072 (у id 4096), CON_TEXTSIZE 8192 (у id 32768) — «диета» памяти, как у q2esp.exe на ПК
# (там поток сверен с ref_bz: demo1, demo2, бои base1 и city3 — кадр в кадр)
target_compile_definitions(${COMPONENT_LIB} PRIVATE C_ONLY NDEBUG GAME_HARD_LINKED REF_HARD_LINKED
                           MAX_PARTICLES=3072 CON_TEXTSIZE=8192)
# -fno-common: одноимённые глобальные переменные частей — ошибка связывания, а не тихое слияние
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -O2 -fno-strict-aliasing -fwrapv -fno-common
                       -fstack-usage)
