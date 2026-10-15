cmake_minimum_required(VERSION 3.24)
project(wwe2k14_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

get_filename_component(WWE2K14_REXGLUE_THIRDPARTY
  "${CMAKE_CURRENT_LIST_DIR}/../../tools/rexglue/thirdparty" ABSOLUTE)
if(NOT EXISTS "${WWE2K14_REXGLUE_THIRDPARTY}/sdl3/CMakeLists.txt")
  # Nested worktrees sit below .worktrees/<name>; use the shared sibling tools directory.
  get_filename_component(WWE2K14_REXGLUE_THIRDPARTY
    "${CMAKE_CURRENT_LIST_DIR}/../../../../tools/rexglue/thirdparty" ABSOLUTE)
endif()
set(WWE2K14_SDL3_SOURCE "${WWE2K14_REXGLUE_THIRDPARTY}/sdl3")
set(WWE2K14_IMGUI_SOURCE "${WWE2K14_REXGLUE_THIRDPARTY}/imgui")
if(NOT EXISTS "${WWE2K14_SDL3_SOURCE}/CMakeLists.txt" OR NOT EXISTS "${WWE2K14_IMGUI_SOURCE}/imgui.cpp")
  message(FATAL_ERROR "The ReXGlue SDK source tree must provide thirdparty/sdl3 and thirdparty/imgui")
endif()

# The SDK's installed SDL is intentionally built without video. Build the UI's own static SDL
# from the SDK source with only the video, renderer, dialogs, events, and controller APIs enabled.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build launcher dependencies statically" FORCE)
set(SDL_SHARED OFF CACHE BOOL "Do not build SDL shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL static library" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "Do not install launcher SDL" FORCE)
set(SDL_UNINSTALL OFF CACHE BOOL "Do not add SDL uninstall target" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Do not build SDL tests" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "Do not build SDL test helpers" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "Do not build SDL examples" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "Launcher does not use SDL audio" FORCE)
set(SDL_VIDEO ON CACHE BOOL "Launcher needs SDL video" FORCE)
set(SDL_RENDER ON CACHE BOOL "Launcher uses SDL_Renderer" FORCE)
set(SDL_DIALOG ON CACHE BOOL "Launcher uses native file dialogs" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "Launcher supports controllers" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "Enable controller HID support" FORCE)
set(SDL_HIDAPI_JOYSTICK ON CACHE BOOL "Enable controller HID drivers" FORCE)
set(SDL_HIDAPI_LIBUSB OFF CACHE BOOL "Avoid an extra libusb dependency" FORCE)
set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "Avoid the optional X11 screen-saver dependency" FORCE)
set(SDL_GPU OFF CACHE BOOL "Launcher does not use SDL_GPU" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "Launcher does not use cameras" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "Launcher does not use haptics" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "Launcher does not use sensors" FORCE)
set(SDL_TRAY OFF CACHE BOOL "Launcher does not use tray APIs" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "Launcher does not use SDL Vulkan" FORCE)
# Hardware-accelerated SDL_Renderer: Direct3D 11 on Windows, OpenGL elsewhere (both loaded at run time,
# no extra link dependencies); SDL falls back to its software renderer if neither can start. Software
# only was too slow on high-DPI laptops (~190 ms per frame at 2560x1520).
if(WIN32)
  set(SDL_OPENGL OFF CACHE BOOL "Launcher uses Direct3D 11 on Windows" FORCE)
  set(SDL_RENDER_D3D11 ON CACHE BOOL "Launcher draws with Direct3D 11" FORCE)
else()
  set(SDL_OPENGL ON CACHE BOOL "Launcher draws with OpenGL" FORCE)
  set(SDL_RENDER_D3D11 OFF CACHE BOOL "Not available outside Windows" FORCE)
endif()
set(SDL_OPENGLES OFF CACHE BOOL "Launcher does not use OpenGL ES" FORCE)
set(SDL_RENDER_GPU OFF CACHE BOOL "Launcher does not use the SDL_GPU renderer" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "Launcher does not use the Vulkan renderer" FORCE)
set(SDL_RENDER_D3D OFF CACHE BOOL "Launcher does not use Direct3D 9" FORCE)
set(SDL_RENDER_D3D12 OFF CACHE BOOL "Launcher does not use Direct3D 12" FORCE)
add_subdirectory("${WWE2K14_SDL3_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdl3")

add_library(wwe2k14_launcher_imgui STATIC
  "${WWE2K14_IMGUI_SOURCE}/imgui.cpp"
  "${WWE2K14_IMGUI_SOURCE}/imgui_draw.cpp"
  "${WWE2K14_IMGUI_SOURCE}/imgui_tables.cpp"
  "${WWE2K14_IMGUI_SOURCE}/imgui_widgets.cpp"
  "${WWE2K14_IMGUI_SOURCE}/backends/imgui_impl_sdl3.cpp"
  "${WWE2K14_IMGUI_SOURCE}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(wwe2k14_launcher_imgui PUBLIC
  "${WWE2K14_IMGUI_SOURCE}" "${WWE2K14_IMGUI_SOURCE}/backends")
target_link_libraries(wwe2k14_launcher_imgui PUBLIC SDL3::SDL3-static)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
find_program(WWE2K14_HOST_CXX_COMPILER NAMES clang++ g++ REQUIRED NO_CMAKE_FIND_ROOT_PATH)
set(WWE2K14_HOST_FONT_COMPRESSOR "${CMAKE_CURRENT_BINARY_DIR}/wwe2k14-font-compressor-host")
add_custom_command(OUTPUT "${WWE2K14_HOST_FONT_COMPRESSOR}"
  COMMAND "${WWE2K14_HOST_CXX_COMPILER}" -std=c++11
    "${WWE2K14_IMGUI_SOURCE}/misc/fonts/binary_to_compressed_c.cpp"
    -o "${WWE2K14_HOST_FONT_COMPRESSOR}"
  DEPENDS "${WWE2K14_IMGUI_SOURCE}/misc/fonts/binary_to_compressed_c.cpp"
  VERBATIM)
add_custom_target(wwe2k14_font_compressor DEPENDS "${WWE2K14_HOST_FONT_COMPRESSOR}")
set(WWE2K14_EMBEDDED_FONTS "${CMAKE_CURRENT_BINARY_DIR}/launcher_embedded_fonts.h")
set(WWE2K14_FONT_FILES
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/Oswald-Medium.ttf"
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/Oswald-SemiBold.ttf"
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/Oswald-Bold.ttf"
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/Barlow-Regular.ttf"
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/Barlow-Medium.ttf"
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/Barlow-SemiBold.ttf"
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/NotoSans-Variable.ttf"
  "${CMAKE_CURRENT_LIST_DIR}/ui/fonts/NotoSansSymbols2-Regular.ttf")
add_custom_command(OUTPUT "${WWE2K14_EMBEDDED_FONTS}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/ui/generate_fonts.py"
    "${WWE2K14_HOST_FONT_COMPRESSOR}" "${CMAKE_CURRENT_LIST_DIR}/ui/fonts"
    "${WWE2K14_EMBEDDED_FONTS}"
  DEPENDS wwe2k14_font_compressor "${CMAKE_CURRENT_LIST_DIR}/ui/generate_fonts.py" ${WWE2K14_FONT_FILES}
  VERBATIM)

# The launcher shows this version and the auto-updater compares it with the newest GitHub release.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/wwe2k14_version.cmake")
configure_file("${CMAKE_CURRENT_LIST_DIR}/launcher_version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/version/launcher_version.h" @ONLY)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/version")

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/core/CMakeLists.txt")
  add_subdirectory(core)
endif()

# While CORE's contract implementation is still landing, builds default to the private test stub.
# As soon as all modules from the shared contract are present, the default changes to real CORE.
set(_wwe2k14_stub_core_default ON)
foreach(_module settings gpus game_files saves music launch bug_report disc_image packages)
  if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/core/${_module}.cpp")
    set(_wwe2k14_stub_core_default ON)
    break()
  endif()
  set(_wwe2k14_stub_core_default OFF)
endforeach()
option(WWE2K14_LAUNCHER_STUB_CORE "Use the UI-only demonstration core" ${_wwe2k14_stub_core_default})

if(NOT WWE2K14_LAUNCHER_STUB_CORE AND NOT TARGET wwe2k14_launcher_core)
  message(FATAL_ERROR "The real launcher core is unavailable; configure with -DWWE2K14_LAUNCHER_STUB_CORE=ON for UI testing")
endif()

add_executable(wwe2k14-launcher WIN32 ui/main.cpp ui/native_keys.cpp "${WWE2K14_EMBEDDED_FONTS}")
add_dependencies(wwe2k14-launcher wwe2k14_font_compressor)
target_include_directories(wwe2k14-launcher PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/core" "${CMAKE_CURRENT_LIST_DIR}/ui"
  "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(wwe2k14-launcher PRIVATE wwe2k14_launcher_imgui)
if(WWE2K14_LAUNCHER_STUB_CORE)
  target_sources(wwe2k14-launcher PRIVATE ui/dev_stub_core.cpp)
  target_compile_definitions(wwe2k14-launcher PRIVATE WWE2K14_LAUNCHER_STUB_CORE=1)
else()
  target_link_libraries(wwe2k14-launcher PRIVATE wwe2k14_launcher_core)
endif()
if(WIN32)
  target_link_libraries(wwe2k14-launcher PRIVATE shell32 ole32 user32)
elseif(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  target_link_libraries(wwe2k14-launcher PRIVATE X11::X11)
  # Keep the C++ runtime with the standalone UI where the toolchain supports static runtime pieces.
  target_link_options(wwe2k14-launcher PRIVATE -static-libgcc -static-libstdc++)
endif()
