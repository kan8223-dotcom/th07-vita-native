cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK to the VitaSDK directory")
  endif()
endif()
# No build type: the flags below (-O2, -O3 -marm for the graphics adapter) are the
# ones the measured build used; a Release type would add -O3 -DNDEBUG everywhere.
project(th07_vita_native LANGUAGES C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TH07_VITA_TITLEID "TH07VT300" CACHE STRING "Vita title ID")
set(TH07_VITA_VERSION "1.0.0")
set(TH07_SHARED "${CMAKE_CURRENT_SOURCE_DIR}/third_party/touhou-modern-vita")
set(TH07_TDLS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/touhou_dls_common")
# Keep build-machine paths out of the executable (__FILE__ in asserts).
add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL_STACK REQUIRED sdl2 SDL2_image SDL2_ttf)

# ---- game engine (some100/th07 portable lineage) and the Vita platform layer
file(GLOB TH07_ENGINE CONFIGURE_DEPENDS src/*.cpp src/pbg4/*.cpp src/thirdparty/*.cpp)
add_library(th07_engine OBJECT ${TH07_ENGINE} vita/SoundPlayerVita.cpp vita/Platform.cpp
  vita/Startup.cpp vita/BootScreen.cpp vita/ExeIcon.cpp vita/MidiBuild.cpp vita/MidiBuildGame.cpp)
target_include_directories(th07_engine PUBLIC src vita ${SDL_STACK_INCLUDE_DIRS} ${TH07_SHARED} ${TH07_TDLS}/include)
target_compile_definitions(th07_engine PUBLIC TH07_VITA=1 SDL_MAIN_HANDLED=1)
set_source_files_properties(vita/Platform.cpp vita/Startup.cpp PROPERTIES COMPILE_DEFINITIONS
  "TH07_VITA_TITLEID=\"${TH07_VITA_TITLEID}\";TH07_VITA_VERSION=\"${TH07_VITA_VERSION}\"")

# Renderer and text options. The defaults are the configuration measured on the
# Vita; every option keeps the drawn output and the game logic unchanged.
option(TH07_TEXT_GLYPH_PREWARM "Warm selected-character dialogue glyphs while loading" ON)
option(TH07_VITA_STAGE_TEXT_ROWS "Precompute the final dialogue rows from the MSG strings" ON)
option(TH07_PACKED_NATIVE_SPRITES "Snapshot four sprite inputs, keeping the original six-vertex order" ON)
option(TH07_TEXT_EXACT_BLIT "Byte-exact coloured text compositor" ON)
option(TH07_TEXT_BOX_NEON "Exact integer eight-pixel text box averages" ON)
target_compile_definitions(th07_engine PUBLIC TH07_TEXT_GLYPH_PREWARM=$<BOOL:${TH07_TEXT_GLYPH_PREWARM}>
  TH07_VITA_STAGE_TEXT_ROWS=$<BOOL:${TH07_VITA_STAGE_TEXT_ROWS}> TH07_PACKED_NATIVE_SPRITES=$<BOOL:${TH07_PACKED_NATIVE_SPRITES}>)
target_compile_options(th07_engine PUBLIC -fno-short-enums PRIVATE -O2 -ffp-contract=off -fno-strict-aliasing -Wformat)
set_source_files_properties(src/TextHelper.cpp PROPERTIES COMPILE_DEFINITIONS
  "TH07_TEXT_EXACT_BLIT=$<BOOL:${TH07_TEXT_EXACT_BLIT}>;TH07_TEXT_BOX_NEON=$<BOOL:${TH07_TEXT_BOX_NEON}>")

# ---- graphics adapter (vita/GxmGraphics.cpp) on the shared GXM renderer
add_executable(th07 vita/main.cpp vita/GxmGraphics.cpp vita/SurfaceCopy.cpp vita/FontRam.cpp
  ${TH07_SHARED}/GxmRenderer.cpp ${TH07_SHARED}/ModernGxmRenderer.cpp ${TH07_SHARED}/CoreRuntime.cpp)
set(TH07_GFX_ON TH07_COARSE_GXM_TIMING TH07_THRESHOLD_CACHE TH07_DEPTH_RLE_MASK TH07_DEPTH_POWER2_WRAP
  TH07_SNAPSHOT_ARENA TH07_REUSE_COMMAND_STORAGE TH07_SPARSE_MULTIPASS_COLOURS TH07_SIMPLE_NATIVE_BATCHES
  TH07_SIMPLE_SCREEN_CONVERT TH07_VIEWPORT_INSIDE_BITS TH07_VERTEX_COLOUR_CACHE TH07_CPU_SCRATCH_REUSE
  TH07_COLOUR_WORD_COMPARE)
set(TH07_GFX_OFF TH07_DEPTH_RECT_OUTLINE TH07_BARY_INTERPOLATE_NEON TH07_BARY_SCALAR_BITS TH07_DEPTH_RUN_WINDOW
  TH07_BARY_CLASSIFY_NEON TH07_BARY_PACKED_FLAGS TH07_DEPTH_FAN_REUSE TH07_SCREEN_QUAD_CULL TH07_QUAD_AXIS_REUSE
  TH07_SCREEN_AXIS_CACHE TH07_SIMPLE_UV_STREAM TH07_DIRECT_UV_STAGING TH07_PLAIN_RUN_REORDER
  TH07_DUPLICATE_SPRITE_VERTICES)
foreach(o ${TH07_GFX_ON})
  option(${o} "graphics adapter option (see vita/GxmGraphics.cpp)" ON)
endforeach()
foreach(o ${TH07_GFX_OFF})
  option(${o} "graphics adapter option (see vita/GxmGraphics.cpp)" OFF)
endforeach()
foreach(o ${TH07_GFX_ON} ${TH07_GFX_OFF})
  target_compile_definitions(th07 PRIVATE ${o}=$<BOOL:${${o}}>)
endforeach()
target_compile_options(th07 PRIVATE -fno-short-enums -O2 -ffp-contract=off -fno-strict-aliasing -Wformat)
set_source_files_properties(vita/GxmGraphics.cpp PROPERTIES COMPILE_OPTIONS "-O3;-marm")

# ---- MIDI: gm.dls synthesizer (touhou_dls_common) and its glue, without the game's flags
add_library(th07_vita_midi STATIC vita/VitaMidi.cpp
  ${TH07_TDLS}/src/blob.cpp ${TH07_TDLS}/src/synth_tables.cpp ${TH07_TDLS}/src/msgs_control.cpp ${TH07_TDLS}/src/synth.cpp
  ${TH07_TDLS}/src/zun_seq.cpp ${TH07_TDLS}/src/smf_scan.cpp ${TH07_TDLS}/src/subset.cpp ${TH07_TDLS}/src/dls.cpp
  ${TH07_TDLS}/src/manifest.cpp ${TH07_TDLS}/src/sha256.cpp)
target_include_directories(th07_vita_midi PRIVATE ${TH07_TDLS}/include)
target_compile_options(th07_vita_midi PRIVATE -O2 -fno-exceptions -fno-rtti -ffp-contract=off)

target_link_libraries(th07 PRIVATE th07_engine th07_vita_midi -Wl,--start-group
    ${SDL_STACK_STATIC_LIBRARIES} vita2d pthread SceGxm_stub SceDisplay_stub
    SceCtrl_stub SceTouch_stub SceCommonDialog_stub SceSysmodule_stub
    SceAppMgr_stub SceIofilemgr_stub SceProcessmgr_stub SceKernelThreadMgr_stub
    SceLibKernel_stub ScePower_stub m -Wl,--end-group)
# pthread_once: libstdc++ thread-safe statics call it through a weak reference; left
# unresolved, the call becomes a no-op and the first guarded local static crashes.
target_link_options(th07 PRIVATE -Wl,-Map=th07.map -Wl,--defsym=__sce_headroom=0x10000 -Wl,--wrap=fopen
    -Wl,--wrap=sceKernelCreateThread -Wl,-u,pthread_cancel -Wl,-u,pthread_once
    -Wl,--wrap=_ZN10AnmManager23CopySurfaceToBackBufferEiiiii
    -Wl,--wrap=_ZN10AnmManager14DrawEndingRectEiiiiiii
    -Wl,--wrap=_ZN10AnmManager11LoadSurfaceEiPKc
    -Wl,--wrap=_ZN10AnmManager14ReleaseSurfaceEi
    -Wl,--wrap=TTF_OpenFont)
vita_create_self(eboot.bin th07 UNSAFE)

set(TH07_SHADERS ${TH07_SHARED}/shaders)
file(GLOB_RECURSE TH07_LICENSE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS licenses/*)
set(TH07_LICENSE_ARGS)
foreach(f ${TH07_LICENSE_FILES})
  list(APPEND TH07_LICENSE_ARGS ${f} ${f})
endforeach()
vita_create_vpk(th07-vita-native.vpk ${TH07_VITA_TITLEID} eboot.bin
  VERSION "01.00" NAME "Touhou 07 Native Vita"
  FILE assets/icon0.png sce_sys/icon0.png
  ${TH07_SHADERS}/draw_v.gxp shaders/draw_v.gxp
  ${TH07_SHADERS}/draw_f.gxp shaders/draw_f.gxp
  ${TH07_SHADERS}/proven_uniform_f.gxp shaders/proven_uniform_f.gxp
  ${TH07_SHADERS}/proven_diffuse_f.gxp shaders/proven_diffuse_f.gxp
  ${TH07_SHADERS}/proven_tint_f.gxp shaders/proven_tint_f.gxp
  assets/NotoSansJP-Regular.ttf NotoSansJP-Regular.ttf
  README.md README.md README_EN.md README_EN.md LICENSE LICENSE CREDITS.md CREDITS.md THIRD_PARTY.md THIRD_PARTY.md
  ${TH07_LICENSE_ARGS})
