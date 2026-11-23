
# wwe2k14 - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(wwe2k14 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Xbox Series X (UWP app family, card C3): the UWP SDK has no SDL3; satisfy its config-time find_dependency only.
if(REX_UWP)
    set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbox/no-sdl3")
endif()

include(generated/rexglue.cmake)

# Sources
set(WWE2K14_SOURCES
    src/main.cpp
    src/cloth_handshake.cpp
    src/cage_hang_probe.cpp
    src/d5_reset_probe.cpp
    src/tiling_probe.cpp
    src/autopilot_input.cpp
    src/freeze_guest_state.cpp
    src/present_interval.cpp
    src/h1_cache_copy.cpp
    src/resolve_coherency.cpp
    src/unlock_all.cpp
    src/hiac_wheel.cpp
    src/head_edit.cpp
    src/arena_page.cpp
    src/universe_guard.cpp
    src/univ_fresh_stories.cpp
    src/univ_group_log.cpp
    src/test_guest_watch.cpp
    src/univ_card_probe.cpp
    src/titantron_guard.cpp
    src/pool_fail_log.cpp
    src/memory_share.cpp
    src/heap43_grow.cpp
    src/dlc_offer_mask.cpp
    src/xmp_state_log.cpp
    src/bpe_guard.cpp
    src/multi_shadows.cpp
    src/test_scenario.cpp
    src/test_direct_match.cpp
    src/timer_queue_sleep.cpp
    src/ultrawide_aspect.cpp
    src/ultrawide_probe.cpp
    src/shader_cache_seed.cpp
    src/thread_profiler.cpp
    src/keep_awake.cpp
    src/keyboard_text.cpp
    src/hitch_catcher.cpp
    src/test_env.cpp
    src/cool_mode.cpp
    src/game_profiler.cpp
    src/timeline.cpp
    src/timeline_core.cpp
    src/overlay_input.cpp
    src/settings_menu_test.cpp
)
if(REX_UWP)
    list(APPEND WWE2K14_SOURCES src/uwp_env.cpp)  # Xbox: packaged native-env.txt + LocalState toml -> environment
endif()

if(WIN32)
    add_executable(wwe2k14 WIN32 ${WWE2K14_SOURCES})
else()
    add_executable(wwe2k14 ${WWE2K14_SOURCES})
endif()

rexglue_setup_target(wwe2k14)
if(REX_UWP)
    # Xbox: the D3D12-only SDK installs no Vulkan headers; the app's Vulkan probe code (never run on Xbox) still needs
    # the type declarations, so use nr-plume's vendored header-only copy.
    target_include_directories(wwe2k14 PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/nr-plume/plume/contrib/Vulkan-Headers/include")
    # An app-family SDK header pulls in windows.h before the renderer's std::min/max uses (wide_core.h).
    target_compile_definitions(wwe2k14 PRIVATE NOMINMAX)
endif()

# Windows exe icon (owner 2026-10-24, GitHub #37). The .ico is made from game art, so it stays out of Git
# (out/icon/wwe2k14.ico, regenerated locally); the resource is added only when the file exists.
set(WWE2K14_ICON "${CMAKE_SOURCE_DIR}/out/icon/wwe2k14.ico" CACHE FILEPATH "Windows exe icon (.ico)")
if(WIN32 AND EXISTS "${WWE2K14_ICON}")
    enable_language(RC)
    file(WRITE "${CMAKE_BINARY_DIR}/wwe2k14_icon.rc" "1 ICON \"${WWE2K14_ICON}\"\n")
    target_sources(wwe2k14 PRIVATE "${CMAKE_BINARY_DIR}/wwe2k14_icon.rc")
endif()

# Native renderer "nr" (src/native/nr, imported from the WWE '13 nr branch; docs/reports/20261013-nr-import.md).
# Linux/Vulkan only. Inert at runtime unless WWE2K14_NR=shadow|replace is set (the frontend registers the SDK
# DrawObserver from a static initializer only then). Default OFF here; the linux-amd64-localsdk preset turns it on
# (it needs the private SDK's rex/graphics/draw_observer.h). The GPU half (nr_backend.cpp + nr's modified plume,
# MIT, vendored ignored under dependencies/nr-plume) is built by tools/nr/build-backend.sh, which partially links
# it and localises plume's own volk/VMA/vk* symbols so they do not collide with the SDK's (same as WWE '13 nr).
option(WWE2K14_HAS_NR "Build the native renderer nr (off at runtime unless WWE2K14_NR is set)" OFF)
if(WWE2K14_HAS_NR AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
  set(NR_PLUME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/nr-plume/plume" CACHE PATH "nr's modified plume copy")
  set(NR_XXHASH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/nr-plume/xxHash" CACHE PATH "xxHash single header dir")
  if(NOT EXISTS "${NR_PLUME_DIR}/plume_vulkan.cpp" OR NOT EXISTS "${NR_XXHASH_DIR}/xxhash.h")
    message(FATAL_ERROR "WWE2K14_HAS_NR: missing ${NR_PLUME_DIR}/plume_vulkan.cpp or ${NR_XXHASH_DIR}/xxhash.h "
                        "(see docs/reports/20261013-nr-import.md, 'Vendored dependencies')")
  endif()
  set(NR_BACKEND_DIR "${CMAKE_CURRENT_BINARY_DIR}/nr_backend")
  if(WIN32)
    # Windows cross build (win-amd64-localsdk): COFF symbol renaming instead of ld -r + localise;
    # Vulkan only (docs/reports/20261013-windows-package.md).
    set(NR_BACKEND_ARCHIVE "${NR_BACKEND_DIR}/nr_backend.lib")
    set(NR_BACKEND_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/nr/build-backend-win.sh")
  else()
    set(NR_BACKEND_ARCHIVE "${NR_BACKEND_DIR}/libnr_backend.a")
    set(NR_BACKEND_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/nr/build-backend.sh")
  endif()
  # Xbox (UWP): same backend sources compiled for the app family (plume's REX_UWP guards: no LoadLibraryA, no HWND).
  set(NR_BACKEND_UWP_ENV "")
  if(REX_UWP)
    set(NR_BACKEND_UWP_ENV "CXX_FLAGS=-std=c++17 -O2 -g --target=x86_64-pc-windows-msvc -march=x86-64-v3 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -DWINAPI_FAMILY=WINAPI_FAMILY_APP -DREX_UWP=1 -D_CRT_USE_WINAPI_FAMILY_DESKTOP_APP=1 -fms-runtime-lib=static")
  endif()
  # Public builds (WWE2K14_TEST_TOOLS OFF) compile the backend's test switches out too (11-09: WWE2K14_TEST_RT_REFUSE).
  if(WWE2K14_TEST_TOOLS)
    set(NR_BACKEND_DEFINES "")
  else()
    set(NR_BACKEND_DEFINES "-DWWE2K14_NO_TEST_TOOLS=1")
  endif()
  add_custom_command(
    OUTPUT "${NR_BACKEND_ARCHIVE}"
    COMMAND env "PLUME_DIR=${NR_PLUME_DIR}" "XXHASH_DIR=${NR_XXHASH_DIR}" "BACKEND_DEFINES=${NR_BACKEND_DEFINES}"
            ${NR_BACKEND_UWP_ENV}
            "BACKEND_SRC=${CMAKE_CURRENT_SOURCE_DIR}/src/native/nr/nr_backend.cpp"
            "OUT_ARCHIVE=${NR_BACKEND_ARCHIVE}" "BUILD_DIR=${NR_BACKEND_DIR}"
            "CXX=${CMAKE_CXX_COMPILER}" "XWIN_DIR=${XWIN_DIR}" "LLVM_BIN=${LLVM_BIN}"
            "${NR_BACKEND_SCRIPT}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/nr/nr_backend.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/native/nr/nr_backend.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/native/nr/nr_record_queue.h"
            "${NR_BACKEND_SCRIPT}"
            "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
            "${NR_PLUME_DIR}/plume_vulkan.cpp"
            "${NR_PLUME_DIR}/plume_vulkan.h"
            "${NR_PLUME_DIR}/plume_d3d12.cpp"
            "${NR_PLUME_DIR}/plume_render_interface_types.h"
            "${NR_XXHASH_DIR}/xxhash.h"
    COMMENT "Building the nr native renderer backend (plume, symbol-localised)"
    VERBATIM)
  add_custom_target(nr_backend DEPENDS "${NR_BACKEND_ARCHIVE}")
  target_sources(wwe2k14 PRIVATE src/native/nr/nr_frontend.cpp src/native/nr/nr_shared.cpp)
  target_compile_definitions(wwe2k14 PRIVATE WWE2K14_HAS_NR=1)
  add_dependencies(wwe2k14 nr_backend)
# Timer thread sleep (src/timer_queue_sleep.cpp, owner 11-06) replaces the SDK timer queue object and needs its ring header.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/timer_queue_sleep.cpp" PROPERTIES INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/rexglue/thirdparty/disruptorplus/include")

  if(WIN32)
    target_link_libraries(wwe2k14 PRIVATE "${NR_BACKEND_ARCHIVE}" d3d12 dxgi dxguid)  # decision 0007 D3D12 backend
  else()
    target_link_libraries(wwe2k14 PRIVATE "${NR_BACKEND_ARCHIVE}" X11 ${CMAKE_DL_LIBS})
  endif()
endif()

# D3D-fed renderer (decision 0006, src/native/d3d): stage 1-2 observe/shadow code, off at runtime unless WWE2K14_D3D_*
# switches are set. Each work package owns its own files here; the glob lets parallel workers add files without
# editing this list (re-run cmake configure after adding a file).
file(GLOB WWE2K14_D3D_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/d3d/*.cpp")
# Owner 2026-10-26: public release builds carry no test tools. OFF compiles the T2 glitch detectors to empty stubs.
option(WWE2K14_TEST_TOOLS "Build test-build-only diagnostics (T2 glitch detectors)" ON)
if(NOT WWE2K14_TEST_TOOLS)
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/native/d3d/glitch_detectors.cpp"
    PROPERTIES COMPILE_DEFINITIONS WWE2K14_NO_TEST_TOOLS=1)
  # Stall catcher: public builds keep only the "[hitch] build=" id line (src/hitch_catcher.h).
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/hitch_catcher.cpp"
    PROPERTIES COMPILE_DEFINITIONS WWE2K14_NO_TEST_TOOLS=1)
  # Lane all-1104 test tools (TEST-ENV.txt overrides, TEST-PROFILE.txt profiler, Cool mode): no-ops in public builds.
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/test_env.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/cool_mode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/game_profiler.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/test_direct_match.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/settings_menu_test.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/test_guest_watch.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/univ_card_probe.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/pool_fail_log.cpp"
    # 11-19 crowd decompile step C1: WWE2K14_D3D_CROWD_CALLERS call-chain census, test builds only.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/native/d3d/crowd_callers.cpp"
    # 11-17 memory_share.cpp: the safety net ships; its fill report + EVERY test switch stay test-only.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/memory_share.cpp"
    # 11-15 (Fable 1.6.1 item 12): UNIV_PREFER, UNLOCK_TRACE and the OWN_MAIN/OWN_GRID grid experiments stay out of public builds.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/univ_fresh_stories.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/unlock_all.cpp"
    PROPERTIES COMPILE_DEFINITIONS WWE2K14_NO_TEST_TOOLS=1)
endif()
if(WWE2K14_TEST_TOOLS)
  # TEMPORARY #122 diagnostic (owner 2026-11-10): event-scene arena log, test builds only; remove when #122 is settled.
  target_sources(wwe2k14 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pool_diag.cpp")
  # 2K15 move proof (11-15, docs/tasks/P15LOOP1-launch.md): WWE2K14_TEST_MOVELOG move/phase log, test builds only.
  target_sources(wwe2k14 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/test_movelog.cpp")
  # P25-DIAG12 (2026-11-16, docs/tasks/P25-DIAG12.md): read-only ch104 loader allocation log; inert unless
  # WWE2K14_TEST_P25_MODEL_ALLOC=1. Test builds only; no public-build or renderer/SDK source.
  target_sources(wwe2k14 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/native/diag/p25_model_alloc.cpp")
  # P25-VB17 (2026-11-16, docs/tasks/P25-VB17.md): read-only index/vertex-buffer create failure log, same env switch.
  target_sources(wwe2k14 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/native/diag/p25_vb_ib.cpp")
endif()
# w13r (decision 0008, owner 2026-11-21): the WWE '13 v1.6 renderer (src/native/w13r, GPL-3.0) as its own exe build.
# It hooks the same D3D functions as src/native/d3d, so that renderer (and nr) stay out of a w13r build.
option(WWE2K14_W13R "Build with the WWE '13 v1.6 renderer instead of the shipped native renderer" OFF)
if(WWE2K14_D3D_SOURCES AND NOT WWE2K14_W13R)
  target_sources(wwe2k14 PRIVATE ${WWE2K14_D3D_SOURCES})
endif()
if(WWE2K14_W13R)
  if(WWE2K14_HAS_NR)
    message(FATAL_ERROR "WWE2K14_W13R needs WWE2K14_HAS_NR=OFF")
  endif()
  include(cmake/w13r_renderer.cmake)
  set(WWE2K14_W13R_API "vulkan" CACHE STRING "w13r graphics API for this exe: vulkan | d3d12 | d3d11")
  if(WWE2K14_W13R_API STREQUAL "d3d12")
    wwe13_add_native_renderer(wwe2k14 D3D12)
  elseif(WWE2K14_W13R_API STREQUAL "d3d11")
    wwe13_add_native_renderer(wwe2k14 D3D11)
  else()
    wwe13_add_native_renderer(wwe2k14)
  endif()
  target_compile_definitions(wwe2k14 PRIVATE WWE2K14_W13R=1)
  # D3D12 / D3D11 w13r builds reuse the generated game code compiled by the Vulkan w13r build (same flags, the API only
  # changes the renderer sources): WWE2K14_W13R_GEN_FROM=<that build dir> links its objects instead of compiling them.
  set(WWE2K14_W13R_GEN_FROM "" CACHE PATH "w13r: build dir whose generated/*.obj this build links (D3D12/D3D11 exes)")
  if(WWE2K14_W13R_GEN_FROM)
    get_target_property(_srcs wwe2k14 SOURCES)
    set(_keep "")
    set(_objs "")
    foreach(_s IN LISTS _srcs)
      if(_s MATCHES "generated/(wwe2k14_[a-z0-9_.]+)\\.cpp$")
        list(APPEND _objs "${WWE2K14_W13R_GEN_FROM}/CMakeFiles/wwe2k14.dir/RelWithDebInfo/generated/${CMAKE_MATCH_1}.cpp.obj")
      else()
        list(APPEND _keep "${_s}")
      endif()
    endforeach()
    set_property(TARGET wwe2k14 PROPERTY SOURCES ${_keep})
    set_source_files_properties(${_objs} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(wwe2k14 PRIVATE ${_objs})
    list(LENGTH _objs _n)
    message(STATUS "w13r: linking ${_n} generated objects from ${WWE2K14_W13R_GEN_FROM}")
  endif()
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/timer_queue_sleep.cpp" PROPERTIES INCLUDE_DIRECTORIES
      "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/rexglue/thirdparty/disruptorplus/include")
endif()
