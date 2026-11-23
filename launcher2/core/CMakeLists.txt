cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(wwe2k14_launcher_core LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(WWE2K14_PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(WWE2K14_REXGLUE_ROOT "${WWE2K14_PROJECT_ROOT}/../tools/rexglue" ABSOLUTE)
if(NOT EXISTS "${WWE2K14_REXGLUE_ROOT}/thirdparty/volk/volk.c")
  # Nested worktrees sit below .worktrees/<name>; use the shared sibling tools directory.
  get_filename_component(WWE2K14_REXGLUE_ROOT "${WWE2K14_PROJECT_ROOT}/../../../tools/rexglue" ABSOLUTE)
endif()
set(WWE2K14_REXGLUE_THIRDPARTY "${WWE2K14_REXGLUE_ROOT}/thirdparty")

set(WWE2K14_MINIZ_VENDOR "${WWE2K14_PROJECT_ROOT}/launcher2/thirdparty/miniz/miniz.h")
if(NOT EXISTS "${WWE2K14_MINIZ_VENDOR}")
  message(FATAL_ERROR "launcher2/thirdparty/miniz/miniz.h is required by the core")
endif()
set(WWE2K14_MINIZ_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/miniz-core")
file(MAKE_DIRECTORY "${WWE2K14_MINIZ_BUILD_DIR}")
file(READ "${WWE2K14_MINIZ_VENDOR}" WWE2K14_MINIZ_CONFIGURED)
# The SDK copies miniz into SDL with its archive/inflate APIs disabled and
# SDL-specific allocator names. Generate a build-tree copy with the standalone
# ZIP/deflate API enabled; keep the vendored third-party file unmodified.
foreach(_miniz_macro MINIZ_NO_INFLATE_APIS MINIZ_NO_ARCHIVE_APIS
                     MINIZ_NO_ARCHIVE_WRITING_APIS MINIZ_NO_ZLIB_APIS
                     MINIZ_SDL_MALLOC MINIZ_STATIC_FUNCTIONS)
  string(REPLACE "#define ${_miniz_macro}\n" "" WWE2K14_MINIZ_CONFIGURED "${WWE2K14_MINIZ_CONFIGURED}")
endforeach()
file(WRITE "${WWE2K14_MINIZ_BUILD_DIR}/miniz.h" "${WWE2K14_MINIZ_CONFIGURED}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WWE2K14_MINIZ_VENDOR}")

add_library(wwe2k14_launcher_miniz STATIC internal/miniz_impl.c)
target_include_directories(wwe2k14_launcher_miniz PUBLIC
  "${WWE2K14_MINIZ_BUILD_DIR}")

# XCompress/LZX support for the player's local WWE 2K14 PAC files. libmspack is
# already vendored by the ReXGlue dependency tree; this launcher only builds its
# LZX decoder translation unit and carries no game-derived data.
get_filename_component(WWE2K14_REPO_ROOT "${WWE2K14_PROJECT_ROOT}/../.." ABSOLUTE)
set(WWE2K14_MSPACK_DIR
  "${WWE2K14_REPO_ROOT}/dependencies/rexglue/thirdparty/libmspack/cabextract/mspack")
if(NOT EXISTS "${WWE2K14_MSPACK_DIR}/lzxd.c" OR NOT EXISTS "${WWE2K14_MSPACK_DIR}/lzx.h")
  message(FATAL_ERROR "The vendored libmspack LZX decoder is required for WWE 2K14 PAC exports")
endif()
add_library(wwe2k14_launcher_lzx SHARED "${WWE2K14_MSPACK_DIR}/lzxd.c")
set_target_properties(wwe2k14_launcher_lzx PROPERTIES
  OUTPUT_NAME wwe2k14_lzx
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(wwe2k14_launcher_lzx PUBLIC "${WWE2K14_MSPACK_DIR}")
if(MSVC)
  target_compile_options(wwe2k14_launcher_lzx PRIVATE /w)
else()
  target_compile_options(wwe2k14_launcher_lzx PRIVATE -w)
endif()

add_library(wwe2k14_launcher_volk STATIC
  "${WWE2K14_REXGLUE_THIRDPARTY}/volk/volk.c")
target_include_directories(wwe2k14_launcher_volk PUBLIC
  "${WWE2K14_REXGLUE_THIRDPARTY}/volk"
  "${WWE2K14_REXGLUE_THIRDPARTY}/vulkan-headers/include")
if(CMAKE_DL_LIBS)
  target_link_libraries(wwe2k14_launcher_volk PUBLIC "${CMAKE_DL_LIBS}")
endif()

file(GLOB WWE2K14_LAUNCHER_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/internal/*.cpp")
# Stock-game wrestler export is outside the supported importer surface.
list(FILTER WWE2K14_LAUNCHER_CORE_SOURCES EXCLUDE REGEX "/wwe14_export\.cpp$")
add_library(wwe2k14_launcher_core STATIC ${WWE2K14_LAUNCHER_CORE_SOURCES})
option(WWE2K14_CREATION_SAMPLE_REPRODUCTION
  "Enable the opt-in, owner-sample-gated recreation of the game-made slot-00 CAW/arena pair" OFF)
target_include_directories(wwe2k14_launcher_core
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
  PRIVATE "${WWE2K14_REXGLUE_THIRDPARTY}/decoders"
          "${WWE2K14_PROJECT_ROOT}/launcher2/thirdparty/picosha2"
          "${CMAKE_CURRENT_LIST_DIR}/../thirdparty")
target_link_libraries(wwe2k14_launcher_core
  PUBLIC wwe2k14_launcher_volk wwe2k14_launcher_miniz wwe2k14_launcher_lzx)
if(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
  target_compile_definitions(wwe2k14_launcher_core PRIVATE WWE2K14_CREATION_SAMPLE_REPRODUCTION=1)
endif()
if(WIN32)
  # The auto-updater uses WinHTTP (a system DLL) for HTTPS; no extra dependency to ship.
  target_link_libraries(wwe2k14_launcher_core PRIVATE shell32 winhttp advapi32 user32)
endif()

if(MSVC)
  target_compile_options(wwe2k14_launcher_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(wwe2k14_launcher_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

file(GLOB WWE2K14_LAUNCHER_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/tests/*_test.cpp")
list(FILTER WWE2K14_LAUNCHER_TEST_SOURCES EXCLUDE REGEX "/wwe14_export_test\.cpp$")
enable_testing()
foreach(test_source IN LISTS WWE2K14_LAUNCHER_TEST_SOURCES)
  get_filename_component(test_name "${test_source}" NAME_WE)
  add_executable("${test_name}" "${test_source}")
  target_link_libraries("${test_name}" PRIVATE wwe2k14_launcher_core)
  if(WIN32 AND CMAKE_CROSSCOMPILING)
    # The xwin/Wine cross-toolchain exposes the build tree as Wine's Z:; keep test writes inside that build tree.
    set(WWE2K14_TESTDATA_PATH "Z:${CMAKE_CURRENT_BINARY_DIR}/testdata")
  else()
    set(WWE2K14_TESTDATA_PATH "${CMAKE_CURRENT_BINARY_DIR}/testdata")
  endif()
  target_compile_definitions("${test_name}" PRIVATE
    WWE2K14_LAUNCHER_TESTDATA="${WWE2K14_TESTDATA_PATH}"
    WWE2K14_LAUNCHER_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}"
    WWE2K14_WWE13_RECIPE="${CMAKE_CURRENT_LIST_DIR}/../data/wwe13-ports.tsv"
    WWE2K14_OWNER_CREATION_TESTDATA="${WWE2K14_PROJECT_ROOT}/../issues/owner-content-20261025"
    WWE2K14_STORY_TESTDATA="${WWE2K14_PROJECT_ROOT}/../story-samples/sample1"
    WWE2K14_R101_CAW_TESTDATA="${WWE2K14_PROJECT_ROOT}/../ref-r101-creations/reports/caw-import/up/545408B2/00000001")
  if(WWE2K14_CREATION_SAMPLE_REPRODUCTION)
    target_compile_definitions("${test_name}" PRIVATE WWE2K14_CREATION_SAMPLE_REPRODUCTION=1)
  endif()
  add_test(NAME "${test_name}" COMMAND "${test_name}")
endforeach()

add_executable(wwe13_import_cli tests/wwe13_import_cli.cpp)
target_link_libraries(wwe13_import_cli PRIVATE wwe2k14_launcher_core)

add_executable(wwe13_convert_probe tests/wwe13_convert_probe.cpp)
target_link_libraries(wwe13_convert_probe PRIVATE wwe2k14_launcher_core)

add_executable(wwe13_arena_probe tests/wwe13_arena_probe.cpp)
target_link_libraries(wwe13_arena_probe PRIVATE wwe2k14_launcher_core)

add_executable(wwe13_pipeline_probe tests/wwe13_pipeline_probe.cpp)
target_link_libraries(wwe13_pipeline_probe PRIVATE wwe2k14_launcher_core)

add_executable(wwe15_convert_probe tests/wwe15_convert_probe.cpp)
target_link_libraries(wwe15_convert_probe PRIVATE wwe2k14_launcher_core)

add_executable(grid_face_repair_probe tests/grid_face_repair_probe.cpp)
target_link_libraries(grid_face_repair_probe PRIVATE wwe2k14_launcher_core)
