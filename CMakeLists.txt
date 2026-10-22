cmake_minimum_required(VERSION 3.20)
project(vkQuakeRT C)

# Quake: Ray Traced for macOS (SDL2 + RayTracedGL1 + MoltenVK)
set(RTGL1_SDK_PATH "" CACHE PATH "RayTracedGL1 source folder (for Include/)")
set(RTGL1_LIBRARY "" CACHE FILEPATH "Path to libRayTracedGL1.dylib")
find_package(SDL2 REQUIRED CONFIG)

set(Q Quake)
set(SRCS
    bgmusic.c cd_sdl.c cfgfile.c chase.c cl_demo.c cl_input.c cl_main.c cl_parse.c cl_tent.c
    cmd.c common.c console.c crc.c cvar.c gl_draw.c gl_fog.c gl_heap.c gl_mesh.c gl_model.c
    gl_refrag.c gl_rlight.c gl_rmain.c gl_rmisc.c gl_screen.c gl_sky.c gl_texmgr.c gl_vidsdl.c
    gl_warp.c host.c host_cmd.c image.c in_sdl.c keys.c main_sdl.c mathlib.c mdfour.c mem.c
    menu.c miniz.c net_bsd.c net_dgrm.c net_loop.c net_main.c net_udp.c palette.c pl_linux.c
    pr_cmds.c pr_edict.c pr_exec.c pr_ext.c r_alias.c r_brush.c r_part.c r_part_fte.c
    r_sprite.c r_world.c sbar.c snd_codec.c snd_dma.c snd_mem.c snd_mix.c snd_sdl.c snd_umx.c
    snd_wave.c snd_vorbis.c strlcat.c strlcpy.c sv_main.c sv_move.c sv_phys.c sv_user.c
    sys_sdl_unix.c tasks.c view.c wad.c world.c crash_macos.c)
list(TRANSFORM SRCS PREPEND "${Q}/")

add_executable(vkquake ${SRCS})
target_include_directories(vkquake PRIVATE ${Q} "${RTGL1_SDK_PATH}/Include")
target_compile_definitions(vkquake PRIVATE USE_SDL2 USE_CODEC_WAVE USE_CODEC_UMX NDEBUG)
if(APPLE)
    target_compile_definitions(vkquake PRIVATE RG_USE_SURFACE_METAL)
endif()
target_compile_options(vkquake PRIVATE -O2 -Wno-unused-function -Wno-deprecated-declarations
    -Wno-pointer-sign -Wno-incompatible-pointer-types-discards-qualifiers -Wno-missing-braces)

if(TARGET SDL2::SDL2)
    target_link_libraries(vkquake PRIVATE SDL2::SDL2)
else()
    target_link_libraries(vkquake PRIVATE SDL2::SDL2-static)
endif()
target_link_libraries(vkquake PRIVATE "${RTGL1_LIBRARY}" m)

# Optional Ogg Vorbis music support (id1/music/*.ogg)
option(QRT_WITH_VORBIS "Build with Ogg Vorbis music" OFF)
if(QRT_WITH_VORBIS)
    find_path(VORBISFILE_INCLUDE_DIR vorbis/vorbisfile.h REQUIRED)
    find_library(VORBISFILE_LIB NAMES vorbisfile REQUIRED)
    find_library(VORBIS_LIB NAMES vorbis REQUIRED)
    find_library(OGG_LIB NAMES ogg REQUIRED)
    target_include_directories(vkquake PRIVATE "${VORBISFILE_INCLUDE_DIR}")
    target_compile_definitions(vkquake PRIVATE USE_CODEC_VORBIS)
    target_link_libraries(vkquake PRIVATE "${VORBISFILE_LIB}" "${VORBIS_LIB}" "${OGG_LIB}")
endif()

set_target_properties(vkquake PROPERTIES
    BUILD_RPATH "@executable_path/../Frameworks"
    INSTALL_RPATH "@executable_path/../Frameworks")
