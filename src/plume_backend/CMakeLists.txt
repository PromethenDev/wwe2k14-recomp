cmake_minimum_required(VERSION 3.20)
project(wwe2k14_plume_backend LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The initial Plume backend target supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(WWE2K14_NATIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PLUME_SOURCE_DIR "${WWE2K14_NATIVE_ROOT}/dependencies/plume-clean-snapshot" CACHE PATH
    "Ignored source snapshot imported from the pinned Plume parent revision")
if(NOT EXISTS "${PLUME_SOURCE_DIR}/plume_vulkan.cpp" OR
   NOT EXISTS "${PLUME_SOURCE_DIR}/UPSTREAM.txt")
    message(FATAL_ERROR
        "Pinned Plume snapshot missing at ${PLUME_SOURCE_DIR}; follow docs/decisions/0002-plume-foundation.md")
endif()

set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUME_SDL_VULKAN_ENABLED OFF CACHE BOOL "" FORCE)
add_subdirectory("${PLUME_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)
find_package(X11 REQUIRED)

add_library(wwe2k14_plume_backend STATIC backend.cpp)
target_include_directories(wwe2k14_plume_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(wwe2k14_plume_backend PUBLIC plume X11::X11)

add_executable(plume_backend_link_check
    link_check.cpp)
target_link_libraries(plume_backend_link_check PRIVATE wwe2k14_plume_backend)

enable_testing()
add_test(NAME plume_backend_help_only COMMAND plume_backend_link_check --help)
add_test(NAME plume_backend_device_name_selection COMMAND plume_backend_link_check --selection-check)
