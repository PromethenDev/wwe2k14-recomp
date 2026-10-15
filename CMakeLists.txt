
# wwe2k14 - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(wwe2k14 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(WWE2K14_SOURCES
    src/main.cpp
    src/cloth_handshake.cpp
    src/cage_hang_probe.cpp
    src/freeze_guest_state.cpp
    src/present_interval.cpp
    src/resolve_coherency.cpp
    src/unlock_all.cpp
    src/shader_cache_seed.cpp
    src/thread_profiler.cpp
)

if(WIN32)
    add_executable(wwe2k14 WIN32 ${WWE2K14_SOURCES})
else()
    add_executable(wwe2k14 ${WWE2K14_SOURCES})
endif()

rexglue_setup_target(wwe2k14)
