# w13r (decision 0008): WWE '13 v1.6 fork's port/cmake/native_renderer.cmake (iMrBurn, GPL-3.0), adapted for 2K14.
# WWE '13 native renderer, taken from WWE '12 PC (iMrBurn/wwe12-pc, GPL-3.0) through WWE 2K15 PC, itself
# adapted from re:Blue (BSD-3-Clause, see src/reblue/LICENSE.reblue) through nefariousjosiah's SvR
# 2008-2010 ports (GPL-3.0).
#
# Draws through plume by observing the game's XDK D3D functions, instead of emulating the Xenos GPU.
# Three executables, as WWE '12 PC (the launcher's Graphics API picks one; AUTO tries them in turn):
#   wwe13_native.exe  Vulkan. With the dumped shaders (WWE13_VULKAN_BUILTIN_SHADERS, on by default when
#                     they are there) their SPIR-V is built into the exe, as WWE '12 PC. Without them,
#                     or for a shader missing from the dump, they are converted on the player's PC
#                     (XenosRecomp built in + dxcompiler.dll beside the exe, cache/native_shaders.bin).
#   wwe13_d3d12.exe   Direct3D 12 (re:Blue built REBLUE_D3D12). The game's shaders are converted at
#                     build time and built into the exe: DXIL from XenosRecomp, every spec-constant
#                     variant pre-linked by wwe13_prelink (no dxcompiler.dll / dxil.dll at run time).
#   wwe13_d3d11.exe   Direct3D 11 (REBLUE_D3D12 + REBLUE_D3D11, plume-d3d11.patch from Kaiko's SvR 2011
#                     PC port). The game's shaders as DXBC, compiled at build time with the Windows
#                     SDK's fxc (tools/convert_shaders_d3d11.py) and built into the exe.
# The D3D12 / D3D11 shader caches come from the containers WWE13_DUMP_SHADERS=<folder> writes
# (work/shaders/xsc, game-derived, never committed). Without them only wwe13_native.exe is built.
#
# The renderer's sources go straight into the executable: its REX_HOOK overrides are strong
# definitions nothing else references, so a static library would drop them at link time.

enable_language(C)  # zstd and xxHash are C

set(WWE13_NATIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/native/w13r")
# plume and reblue-XenosRecomp as WWE 2K15 PC has them (C:/Dev/native-deps, shared: never written to).
# Its XenosRecomp sources give wwe13_native.exe its run-time shader translator.
# Default: a native-deps folder beside this repository (prepared as in wwe13-recomp v1.6.0, docs/BUILDING.md).
set(WWE13_NATIVE_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../native-deps" CACHE PATH
    "Folder holding plume and reblue-XenosRecomp for the native renderer (WWE 2K15 PC's)")
# This project's own copy of them with the Direct3D 12 / 11 patches (port/tools/prepare_native_deps.ps1,
# run by build.ps1 -Native): its plume builds all three executables, its XenosRecomp.exe the
# build-time shader caches.
set(WWE13_RENDER_DEPS "${WWE13_NATIVE_DEPS}" CACHE PATH
    "Patched copy of plume and reblue-XenosRecomp (port/tools/prepare_native_deps.ps1)")
set(WWE13_XENOS_DIR "${WWE13_NATIVE_DEPS}/reblue-XenosRecomp")
set(WWE13_PLUME_DIR "${WWE13_RENDER_DEPS}/plume")
set(WWE13_XR_DIR "${WWE13_RENDER_DEPS}/reblue-XenosRecomp")
set(WWE13_NATIVE_GEN "${CMAKE_BINARY_DIR}/native_gen")
if(NOT EXISTS "${WWE13_PLUME_DIR}/plume_d3d11.cpp")
    message(FATAL_ERROR "${WWE13_PLUME_DIR}: patched plume missing, run port/tools/prepare_native_deps.ps1 "
                        "(build.ps1 -Native does)")
endif()

# The game's shader containers for the build-time caches (WWE13_DUMP_SHADERS).
set(WWE13_XSC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/w13r/xsc" CACHE PATH
    "The game's shader containers (.xsc) dumped with WWE13_DUMP_SHADERS")
file(GLOB WWE13_XSC_FILES CONFIGURE_DEPENDS "${WWE13_XSC_DIR}/*.xsc")
set(WWE13_XR_EXE "${WWE13_XR_DIR}/build/XenosRecomp/XenosRecomp.exe")
set(WWE13_XR_RUN "${WWE13_XR_EXE}")
if(NOT CMAKE_HOST_WIN32)
    # Linux cross-build: XenosRecomp built for Linux in <reblue-XenosRecomp>/build-linux (-DREBLUE_RECOMP).
    set(WWE13_XR_EXE "${WWE13_XR_DIR}/build-linux/XenosRecomp/XenosRecomp")
    set(WWE13_XR_RUN "${WWE13_NATIVE_ROOT}/tools/xenosrecomp-linux.sh" "${WWE13_XR_DIR}")
endif()
set(_d3d_default OFF)
if(WIN32 AND WWE13_XSC_FILES AND EXISTS "${WWE13_XR_EXE}")
    set(_d3d_default ON)
endif()
option(WWE13_D3D12_EXE "Also build wwe13_d3d12.exe (Direct3D 12 renderer)" ${_d3d_default})
option(WWE13_D3D11_EXE "Also build wwe13_d3d11.exe (Direct3D 11 renderer)" ${_d3d_default})
# wwe13_native.exe with the game's SPIR-V built in (as WWE '12 PC): no conversion at first launch and
# no dxcompiler.dll needed. A shader missing from the dump is still converted at run time when
# dxcompiler.dll is beside the exe (the build folder has it), else its draws are skipped and logged.
option(WWE13_VULKAN_BUILTIN_SHADERS "Build the game's shaders into wwe13_native.exe (Vulkan)" ${_d3d_default})
if((WWE13_D3D12_EXE OR WWE13_D3D11_EXE OR WWE13_VULKAN_BUILTIN_SHADERS) AND
   NOT (WWE13_XSC_FILES AND EXISTS "${WWE13_XR_EXE}"))
    message(WARNING "wwe13_d3d12 / wwe13_d3d11 and the built-in Vulkan shaders need the game's shaders in "
                    "${WWE13_XSC_DIR} (run wwe13_native.exe once with WWE13_DUMP_SHADERS=<that folder>) and "
                    "${WWE13_XR_EXE}: not built")
    set(WWE13_D3D12_EXE OFF CACHE BOOL "" FORCE)
    set(WWE13_D3D11_EXE OFF CACHE BOOL "" FORCE)
    set(WWE13_VULKAN_BUILTIN_SHADERS OFF CACHE BOOL "" FORCE)
endif()

# plume builds Vulkan and D3D12 into one library (each exe uses one); D3D11 is plume_d3d11.cpp, added
# to wwe13_d3d11 only.
if(WIN32 AND (WWE13_D3D12_EXE OR WWE13_D3D11_EXE))
    set(PLUME_D3D12_ENABLED ON CACHE BOOL "" FORCE)
else()
    set(PLUME_D3D12_ENABLED OFF CACHE BOOL "" FORCE)
endif()
# Linux (v1.7.1): WWE 2K14's SDK (0.7.3) opens a GTK window on X11, not an SDL window like WWE '13's SDK 0.10, so plume
# uses its Xlib surface (RenderWindow = Display* + Window; platform/native_window.cpp takes them from the GTK window).
if(NOT WIN32 AND NOT APPLE AND NOT ANDROID)
    set(PLUME_SDL_VULKAN_ENABLED OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${WWE13_PLUME_DIR}" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)
# w13r: plume's VMA beside the SDK's (different versions): rename plume's copy (compat/w13r_vma_rename.h).
target_compile_options(plume PRIVATE -include "${WWE13_NATIVE_ROOT}/compat/w13r_vma_rename.h")

# Small third-party pieces the renderer uses.
add_library(wwe13_native_thirdparty STATIC
    "${WWE13_NATIVE_ROOT}/third_party/reblue_thirdparty/zstd/zstddeclib.c"
    "${WWE13_XENOS_DIR}/thirdparty/xxHash/xxhash.c"
    "${WWE13_XENOS_DIR}/thirdparty/smol-v/source/smolv.cpp")
target_include_directories(wwe13_native_thirdparty PUBLIC
    "${WWE13_NATIVE_ROOT}/third_party/reblue_thirdparty/zstd"
    "${WWE13_XENOS_DIR}/thirdparty/xxHash"
    "${WWE13_XENOS_DIR}/thirdparty/smol-v/source")

# wwe13_native.exe: game shaders are converted on the player's PC (option chosen on 2026-10-27).
# XenosRecomp's translator (MIT) is built into the executable; DXC's dxcompiler.dll (shipped beside
# the exe) turns its HLSL into SPIR-V; the result is kept in cache/native_shaders.bin next to the exe
# (src/native/wwe13_shader_cache.cpp).
add_library(wwe13_xenosrecomp STATIC
    "${WWE13_XENOS_DIR}/XenosRecomp/shader_recompiler.cpp"
    "${WWE13_NATIVE_ROOT}/src/native_xr/xenos_translate.cpp")
# XenosRecomp's own fmt (header-only) stays private: the SDK's spdlog links another fmt build.
target_include_directories(wwe13_xenosrecomp PRIVATE
    "${WWE13_XENOS_DIR}/XenosRecomp"
    "${WWE13_XENOS_DIR}/thirdparty/fmt/include"
    "${WWE13_XENOS_DIR}/thirdparty/zstd/lib"
    "${WWE13_XENOS_DIR}/thirdparty/xxHash")
target_include_directories(wwe13_xenosrecomp PUBLIC
    "${WWE13_NATIVE_ROOT}/src/native_xr"
    "${WWE13_XENOS_DIR}/thirdparty/dxc-bin/inc"
    "${WWE13_XENOS_DIR}/thirdparty/smol-v/source")
target_compile_definitions(wwe13_xenosrecomp PRIVATE REBLUE_RECOMP FMT_HEADER_ONLY _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(wwe13_xenosrecomp PRIVATE "/FI${WWE13_XENOS_DIR}/XenosRecomp/pch.h"
        /clang:-Wno-switch /clang:-Wno-unused-variable /clang:-Wno-null-arithmetic)
else()
    target_compile_options(wwe13_xenosrecomp PRIVATE -include "${WWE13_XENOS_DIR}/XenosRecomp/pch.h"
        -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
endif()
# The HLSL prelude XenosRecomp prepends to every shader (shader_common.h), embedded as bytes.
file(READ "${WWE13_XENOS_DIR}/XenosRecomp/shader_common.h" _common_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _common_bytes "${_common_hex}")
file(WRITE "${WWE13_NATIVE_GEN}/shader_common_text.inc" "${_common_bytes}")
set(WWE13_DXCOMPILER_DLL "${WWE13_XENOS_DIR}/thirdparty/dxc-bin/bin/x64/dxcompiler.dll")

# Host shaders (copy, ImGui overlay, MSAA resolve, gamma) compiled to SPIR-V headers (Vulkan) and
# DXIL headers (D3D12) with the DXC shipped in reblue-XenosRecomp (the Windows SDK's dxc has no
# SPIR-V backend), and to DXBC headers (D3D11) with the Windows SDK's fxc. The HLSL keeps its
# Vulkan-only parts under #ifdef __spirv__.
set(WWE13_DXC "${WWE13_XENOS_DIR}/thirdparty/dxc-bin/bin/x64/dxc.exe")
if(NOT CMAKE_HOST_WIN32)
    # Linux cross-build (xwin): the Linux DXC of the same dxc-bin, through a wrapper for its libdxcompiler.so.
    set(WWE13_DXC "${WWE13_NATIVE_ROOT}/tools/dxc-linux.sh" "${WWE13_XENOS_DIR}/thirdparty/dxc-bin")
endif()
set(WWE13_HLSL_DIR "${WWE13_NATIVE_ROOT}/src/reblue/gpu/shaders/hlsl")
set(WWE13_HOST_SHADER_HEADERS "")
set(WWE13_HOST_SHADER_HEADERS_DXIL "")
set(WWE13_HOST_SHADER_HEADERS_DXBC "")
set(WWE13_FXC_LITE_DIR "" CACHE PATH
    "Linux cross-build: folder for fxc-lite.exe, holding Microsoft's D3DCompiler_47.dll (tools/fxc-lite)")
if(WIN32 AND WWE13_D3D11_EXE AND NOT CMAKE_HOST_WIN32)
    # Linux cross-build: fxc-lite (the fxc options these scripts use, on Microsoft's D3DCompiler_47.dll) under Wine.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT WWE13_FXC_LITE_DIR OR NOT EXISTS "${WWE13_FXC_LITE_DIR}/D3DCompiler_47.dll" OR NOT Python3_FOUND)
        message(WARNING "wwe13_d3d11 cross-build needs WWE13_FXC_LITE_DIR with D3DCompiler_47.dll, and Python 3: not built")
        set(WWE13_D3D11_EXE OFF CACHE BOOL "" FORCE)
    else()
        # fxc-lite is a Windows program built by this toolchain; a generated wrapper runs it under Wine
        # next to the genuine DLL.
        add_executable(wwe13_fxc_lite "${WWE13_NATIVE_ROOT}/tools/fxc-lite/fxc-lite.cpp")
        set_target_properties(wwe13_fxc_lite PROPERTIES WIN32_EXECUTABLE OFF  # w13r: 2K14 toolchain defaults to a GUI exe
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}")
        target_link_options(wwe13_fxc_lite PRIVATE "-Wl,/SUBSYSTEM:CONSOLE")
        target_compile_definitions(wwe13_fxc_lite PRIVATE _CRT_SECURE_NO_WARNINGS)
        set(WWE13_FXC "${CMAKE_BINARY_DIR}/fxc.sh")
        file(WRITE "${WWE13_FXC}" "#!/bin/sh\n"
            "cp -u \"${CMAKE_BINARY_DIR}/wwe13_fxc_lite.exe\" \"${WWE13_FXC_LITE_DIR}/fxc-lite.exe\" || exit 1\n"
            "exec \"${WWE13_NATIVE_ROOT}/tools/fxc-lite/fxc.sh\" \"${WWE13_FXC_LITE_DIR}\" \"$@\"\n")
        file(CHMOD "${WWE13_FXC}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
        set(WWE13_FXC_DEPENDS wwe13_fxc_lite)
    endif()
elseif(WIN32 AND WWE13_D3D11_EXE)
    file(GLOB WWE13_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    list(SORT WWE13_FXC_CANDIDATES)
    list(POP_BACK WWE13_FXC_CANDIDATES WWE13_FXC)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT WWE13_FXC OR NOT Python3_FOUND)
        message(WARNING "wwe13_d3d11 needs the Windows SDK's fxc.exe and Python 3: not built")
        set(WWE13_D3D11_EXE OFF CACHE BOOL "" FORCE)
    endif()
endif()
function(wwe13_host_shader STEM PROFILE)
    file(GLOB hlsl_includes "${WWE13_HLSL_DIR}/*.hlsli")
    set(args -spirv -fvk-use-dx-layout)
    if(PROFILE MATCHES "^vs")
        list(APPEND args -fvk-invert-y)
    endif()
    set(out "${WWE13_NATIVE_GEN}/src/gpu/shaders/hlsl/${STEM}.hlsl.spirv.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${WWE13_DXC} -T ${PROFILE} -HV 2021 -all-resources-bound -Wno-ignored-attributes
                ${args} -I "${WWE13_NATIVE_ROOT}" -Fh "${out}" "${WWE13_HLSL_DIR}/${STEM}.hlsl"
                -Vn g_${STEM}_spirv
        DEPENDS "${WWE13_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes}
        COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, SPIR-V)"
        VERBATIM)
    set(WWE13_HOST_SHADER_HEADERS ${WWE13_HOST_SHADER_HEADERS} "${out}" PARENT_SCOPE)
    if(WWE13_D3D12_EXE OR WWE13_D3D11_EXE)
        set(out_dxil "${WWE13_NATIVE_GEN}/src/gpu/shaders/hlsl/${STEM}.hlsl.dxil.h")
        add_custom_command(
            OUTPUT "${out_dxil}"
            COMMAND ${WWE13_DXC} -T ${PROFILE} -HV 2021 -all-resources-bound
                    -Wno-ignored-attributes -I "${WWE13_NATIVE_ROOT}" -Fh "${out_dxil}"
                    "${WWE13_HLSL_DIR}/${STEM}.hlsl" -Vn g_${STEM}_dxil
            DEPENDS "${WWE13_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes}
            COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, DXIL)"
            VERBATIM)
        set(WWE13_HOST_SHADER_HEADERS_DXIL ${WWE13_HOST_SHADER_HEADERS_DXIL} "${out_dxil}" PARENT_SCOPE)
    endif()
    if(WWE13_D3D11_EXE)
        string(REPLACE "_6_0" "_5_0" profile_sm5 "${PROFILE}")
        set(out_dxbc "${WWE13_NATIVE_GEN}/src/gpu/shaders/hlsl/${STEM}.hlsl.dxbc.h")
        add_custom_command(
            OUTPUT "${out_dxbc}"
            COMMAND "${Python3_EXECUTABLE}" -I "${WWE13_NATIVE_ROOT}/tools/host_shader_d3d11.py"
                    "${WWE13_FXC}" ${profile_sm5} "${WWE13_HLSL_DIR}/${STEM}.hlsl" "${out_dxbc}"
                    g_${STEM}_dxbc -I "${WWE13_NATIVE_ROOT}"
            DEPENDS "${WWE13_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes}
                    "${WWE13_NATIVE_ROOT}/tools/host_shader_d3d11.py" ${WWE13_FXC_DEPENDS}
            COMMENT "Compiling ${STEM}.hlsl (${profile_sm5}, DXBC)"
            VERBATIM)
        set(WWE13_HOST_SHADER_HEADERS_DXBC ${WWE13_HOST_SHADER_HEADERS_DXBC} "${out_dxbc}" PARENT_SCOPE)
    endif()
endfunction()
foreach(shader IN ITEMS copy_vs imgui_vs)
    wwe13_host_shader(${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS copy_color_ps copy_depth_ps gamma_correction_ps gamma_correction_lite_ps pfx_occlusion_count_ps
        imgui_ps resolve_msaa_color_2x resolve_msaa_color_4x resolve_msaa_color_8x
        resolve_msaa_depth_2x resolve_msaa_depth_4x resolve_msaa_depth_8x)
    wwe13_host_shader(${shader} ps_6_0)
endforeach()

# The build-time shader caches of wwe13_d3d12 / wwe13_d3d11 (as WWE '12 PC's tools/gen_shaders.ps1):
# XenosRecomp turns the dumped containers into shader_cache.cpp (DXIL + SPIR-V, the table re:Blue
# looks shaders up in) and leaves each shader's HLSL in native_gen/d3d11_hlsl for fxc.
set(WWE13_MINIZ_DIR "${WWE13_XENOS_DIR}/thirdparty/smol-v/testing/external/miniz")
set(WWE13_DXC_DIR "${WWE13_XENOS_DIR}/thirdparty/dxc-bin")
if(WWE13_D3D12_EXE OR WWE13_D3D11_EXE OR WWE13_VULKAN_BUILTIN_SHADERS)
    set(WWE13_SHADER_CACHE "${WWE13_NATIVE_GEN}/shader_cache.cpp")
    set(WWE13_D3D11_HLSL "${WWE13_NATIVE_GEN}/d3d11_hlsl")
    add_custom_command(
        OUTPUT "${WWE13_SHADER_CACHE}" "${WWE13_D3D11_HLSL}/.done"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${WWE13_D3D11_HLSL}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WWE13_D3D11_HLSL}"
        COMMAND ${WWE13_XR_RUN} "${WWE13_XSC_DIR}" "${WWE13_SHADER_CACHE}"
                "${WWE13_XR_DIR}/XenosRecomp/shader_common.h" "${WWE13_D3D11_HLSL}"
        COMMAND ${CMAKE_COMMAND} -E touch "${WWE13_D3D11_HLSL}/.done"
        DEPENDS ${WWE13_XSC_FILES} "${WWE13_XR_EXE}" "${WWE13_XR_DIR}/XenosRecomp/shader_common.h"
        WORKING_DIRECTORY "${WWE13_XR_DIR}"
        COMMENT "Converting the game's shaders with XenosRecomp (${WWE13_XSC_DIR})"
        VERBATIM)
endif()

# D3D12: every spec-constant variant of the game's DXIL shaders linked at build time by wwe13_prelink
# (port/src/prelink, WWE '12 PC's wwe12_prelink), compiled into wwe13_d3d12.exe as
# linked_shader_cache.cpp, so the exe needs no DXC at run time (dxcompiler.dll / dxil.dll are never
# shipped with it: Microsoft's dxil.dll licence). The tool runs DXC on the build machine; the two DLLs
# land next to it in the build folder, where the dev exe can still link a missing variant.
if(WWE13_D3D12_EXE)
    add_executable(wwe13_prelink
        "${WWE13_NATIVE_ROOT}/src/prelink/wwe13_prelink.cpp"
        "${WWE13_NATIVE_ROOT}/src/reblue/gpu/shaders/dxc_link.cpp"
        "${WWE13_SHADER_CACHE}"
        "${WWE13_NATIVE_ROOT}/third_party/reblue_thirdparty/zstd/zstddeclib.c"
        "${WWE13_MINIZ_DIR}/miniz.c")
    target_include_directories(wwe13_prelink PRIVATE
        "${WWE13_NATIVE_ROOT}/src/reblue"
        "${WWE13_NATIVE_ROOT}/third_party/reblue_thirdparty/zstd"
        "${WWE13_MINIZ_DIR}"
        "${WWE13_DXC_DIR}/inc")
    target_link_libraries(wwe13_prelink PRIVATE "${WWE13_DXC_DIR}/lib/x64/dxcompiler.lib")
    add_custom_command(TARGET wwe13_prelink POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WWE13_DXC_DIR}/bin/x64/dxcompiler.dll" "${WWE13_DXC_DIR}/bin/x64/dxil.dll"
            $<TARGET_FILE_DIR:wwe13_prelink>
        VERBATIM)
    set(WWE13_LINKED_SHADER_CACHE "${WWE13_NATIVE_GEN}/linked_shader_cache.cpp")
    if(CMAKE_HOST_WIN32)
        add_custom_command(
            OUTPUT "${WWE13_LINKED_SHADER_CACHE}"
            COMMAND wwe13_prelink "${WWE13_LINKED_SHADER_CACHE}"
            DEPENDS wwe13_prelink "${WWE13_SHADER_CACHE}"
            COMMENT "Pre-linking the D3D12 spec-constant shader variants"
            VERBATIM)
    else()
        # Linux cross-build: the same prelink sources built for the host against dxc-bin's libdxcompiler.so
        # (dxcompiler.dll under Wine failed every link).
        add_custom_command(
            OUTPUT "${WWE13_LINKED_SHADER_CACHE}"
            COMMAND "${WWE13_NATIVE_ROOT}/tools/prelink-linux.sh" "${WWE13_NATIVE_ROOT}" "${WWE13_XENOS_DIR}"
                    "${WWE13_MINIZ_DIR}" "${CMAKE_BINARY_DIR}/prelink-host" "${WWE13_SHADER_CACHE}"
                    "${WWE13_LINKED_SHADER_CACHE}"
            DEPENDS "${WWE13_SHADER_CACHE}" "${WWE13_NATIVE_ROOT}/src/prelink/wwe13_prelink.cpp"
                    "${WWE13_NATIVE_ROOT}/tools/prelink-linux.sh"
            COMMENT "Pre-linking the D3D12 spec-constant shader variants (Linux host)"
            VERBATIM)
    endif()
endif()

# D3D11: the game's shaders as DXBC (fxc), one per spec-constant subset (WWE '12 PC's method).
if(WWE13_D3D11_EXE)
    set(WWE13_DXBC_SHADER_CACHE "${WWE13_NATIVE_GEN}/dxbc_shader_cache.cpp")
    add_custom_command(
        OUTPUT "${WWE13_DXBC_SHADER_CACHE}"
        COMMAND ${CMAKE_COMMAND} -E env "WWE13_FXC=${WWE13_FXC}"
                "${Python3_EXECUTABLE}" -I "${WWE13_NATIVE_ROOT}/tools/convert_shaders_d3d11.py"
                "${WWE13_D3D11_HLSL}" "${WWE13_DXBC_SHADER_CACHE}"
        DEPENDS "${WWE13_D3D11_HLSL}/.done" "${WWE13_NATIVE_ROOT}/tools/convert_shaders_d3d11.py" ${WWE13_FXC_DEPENDS}
        COMMENT "Converting the game's shaders to DXBC (fxc)"
        VERBATIM)
endif()

file(GLOB_RECURSE WWE13_NATIVE_SOURCES CONFIGURE_DEPENDS
    "${WWE13_NATIVE_ROOT}/src/reblue/*.cpp" "${WWE13_NATIVE_ROOT}/src/reblue/*.h"
    "${WWE13_NATIVE_ROOT}/src/native/*.cpp" "${WWE13_NATIVE_ROOT}/src/native/*.h")

# wwe13_add_native_renderer(<target> [D3D12|D3D11]): D3D12 builds re:Blue with REBLUE_D3D12 (DXIL
# shaders); D3D11 adds REBLUE_D3D11 to it (plume_d3d11, DXBC shaders); without either, Vulkan.
function(wwe13_add_native_renderer target)
    cmake_parse_arguments(ARG "D3D12;D3D11" "" "" ${ARGN})
    set(sources ${WWE13_NATIVE_SOURCES})
    if(ARG_D3D11 OR ARG_D3D12)
        # Built-in shaders: no run-time conversion (nor XenosRecomp, nor dxcompiler.dll beside the exe).
        list(FILTER sources EXCLUDE REGEX "/native/wwe13_shader_cache\\.cpp$")
        target_sources(${target} PRIVATE "${WWE13_SHADER_CACHE}" "${WWE13_MINIZ_DIR}/miniz.c")
        target_include_directories(${target} PRIVATE "${WWE13_MINIZ_DIR}")
    endif()
    if(ARG_D3D11)
        set(host_shaders ${WWE13_HOST_SHADER_HEADERS_DXBC})
        target_compile_definitions(${target} PRIVATE REBLUE_D3D12=1 REBLUE_D3D11=1)
        target_link_libraries(${target} PRIVATE d3d11 dxgi dxguid)
        list(FILTER sources EXCLUDE REGEX "/gpu/shaders/(shader_linker|dxc_link)\\.(cpp|h)$")
        # plume's D3D11 backend (port/patches/plume-d3d11.patch, from the SvR 2011 PC port).
        target_sources(${target} PRIVATE "${WWE13_PLUME_DIR}/plume_d3d11.cpp" "${WWE13_DXBC_SHADER_CACHE}")
        set_source_files_properties("${WWE13_PLUME_DIR}/plume_d3d11.cpp" PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN")  # w13r: as the fork's global defines
    elseif(ARG_D3D12)
        set(host_shaders ${WWE13_HOST_SHADER_HEADERS_DXIL})
        target_compile_definitions(${target} PRIVATE REBLUE_D3D12=1)
        # dxc_link.cpp links a spec-constant shader at run time only when a variant is missing from
        # the pre-linked cache (dev builds): dxcompiler.dll is delay-loaded, the shipped exe starts
        # without it (guest_shaders.cpp checks before linking).
        target_include_directories(${target} PRIVATE "${WWE13_DXC_DIR}/inc")
        target_link_libraries(${target} PRIVATE d3d12 dxgi dxguid delayimp
            "${WWE13_DXC_DIR}/lib/x64/dxcompiler.lib")
        if(MSVC)
            target_link_options(${target} PRIVATE "/DELAYLOAD:dxcompiler.dll")
        else()
            target_link_options(${target} PRIVATE "-Wl,/DELAYLOAD:dxcompiler.dll")
        endif()
        target_sources(${target} PRIVATE "${WWE13_LINKED_SHADER_CACHE}")
    else()
        set(host_shaders ${WWE13_HOST_SHADER_HEADERS})
        # re:Blue's DXIL linker serves the D3D12 exe only.
        list(FILTER sources EXCLUDE REGEX "/gpu/shaders/(shader_linker|dxc_link)\\.(cpp|h)$")
        # The run-time conversion: every shader without WWE13_VULKAN_BUILTIN_SHADERS, else only the
        # ones missing from the built-in cache (guest_shaders.cpp, WWE13_BUILTIN_SPIRV).
        target_compile_definitions(${target} PRIVATE WWE13_RUNTIME_SHADERS=1)
        # w13r: miniz for the test pictures (replay.cpp WriteTargetPng; WWE2K14_NR_SHOT_EVERY) in the Vulkan exe too.
        if(NOT WWE13_VULKAN_BUILTIN_SHADERS)
            target_sources(${target} PRIVATE "${WWE13_MINIZ_DIR}/miniz.c")
            target_include_directories(${target} PRIVATE "${WWE13_MINIZ_DIR}")
        endif()
        if(WWE13_VULKAN_BUILTIN_SHADERS)
            target_compile_definitions(${target} PRIVATE WWE13_BUILTIN_SPIRV=1)
            target_sources(${target} PRIVATE "${WWE13_SHADER_CACHE}")
        endif()
        target_link_libraries(${target} PRIVATE wwe13_xenosrecomp)
        if(WIN32)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WWE13_DXCOMPILER_DLL}"
                    "$<TARGET_FILE_DIR:${target}>/dxcompiler.dll"
                VERBATIM)
        else()
            # Linux: DXC for shaders missing from the built-in cache (libdxcompiler.so dlopens libdxil.so).
            set(_dxc_lib "${WWE13_XENOS_DIR}/thirdparty/dxc-bin/lib/x64")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dxc_lib}/libdxcompiler.so"
                    "${_dxc_lib}/libdxil.so" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endif()
    target_sources(${target} PRIVATE ${sources} ${host_shaders})
    target_include_directories(${target} PRIVATE
        "${WWE13_NATIVE_ROOT}/compat"
        "${WWE13_NATIVE_ROOT}/src/reblue"
        "${WWE13_NATIVE_ROOT}/src/native"
        "${WWE13_NATIVE_GEN}"
        "${WWE13_PLUME_DIR}"
        "${WWE13_XENOS_DIR}/XenosRecomp"
    )
    target_compile_definitions(${target} PRIVATE SVR_NATIVE_RENDERER=1)
    # WWE 2K15's addresses for the D3D functions re:Blue's hooks name (D3DDevice_Clear -> sub_...).
    if(MSVC)
        set(_names_include "/FI${WWE13_NATIVE_ROOT}/compat/w13r_compat.h;/FI${WWE13_NATIVE_ROOT}/src/native/wwe2k14_d3d_names.h")
    else()
        set(_names_include "-include;${WWE13_NATIVE_ROOT}/compat/w13r_vma_rename.h;-include;${WWE13_NATIVE_ROOT}/compat/w13r_compat.h;-include;${WWE13_NATIVE_ROOT}/src/native/wwe2k14_d3d_names.h")
    endif()
    set_source_files_properties(${WWE13_NATIVE_SOURCES} PROPERTIES COMPILE_OPTIONS "${_names_include}")
    if(NOT WIN32)
        # dxcapi.h off Windows (WinAdapter.h) declares its COM interfaces with __declspec(uuid).
        set_property(SOURCE "${WWE13_NATIVE_ROOT}/src/native/wwe13_shader_cache.cpp" APPEND PROPERTY
            COMPILE_OPTIONS -fms-extensions)
    endif()
    # w13r: the generated shader caches use the SDK 0.10 type names too.
    foreach(_gen IN ITEMS "${WWE13_SHADER_CACHE}" "${WWE13_LINKED_SHADER_CACHE}" "${WWE13_DXBC_SHADER_CACHE}")
        if(_gen)
            set_source_files_properties("${_gen}" PROPERTIES COMPILE_OPTIONS "${_names_include}")
        endif()
    endforeach()
    target_link_libraries(${target} PRIVATE plume wwe13_native_thirdparty rex::o1heap)
    # Linker map: names recompiled functions in crash stacks.
    if(MSVC)
        target_link_options(${target} PRIVATE "/MAP:${CMAKE_BINARY_DIR}/${target}.map")
    elseif(WIN32)
        target_link_options(${target} PRIVATE "-Wl,/MAP:${CMAKE_BINARY_DIR}/${target}.map")
    else()
        target_link_options(${target} PRIVATE "-Wl,-Map=${CMAKE_BINARY_DIR}/${target}.map")
    endif()
endfunction()
