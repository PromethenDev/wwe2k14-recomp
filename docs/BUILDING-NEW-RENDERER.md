# Building the new renderer (v1.7.1)

From v1.7.0 the Windows release has three extra game programs: `wwe2k14-vulkan.exe`, `wwe2k14-d3d12.exe` and
`wwe2k14-d3d11.exe`; from v1.7.1 the Linux release has `Linux-x86_64/wwe2k14-vulkan`. They are WWE 2K14 drawn through the WWE '13 Recomp v1.6 native renderer by iMrBurn
(`src/native/w13r`, GPL-3.0, see `src/native/w13r/LICENSE.GPL-3.0` and `src/native/w13r/THIRD_PARTY.wwe13.md`).
Because they contain that code, these programs as a whole are distributed under GPL-3.0; this repository
(tag `v1.7.1`) is their source. `wwe2k14.exe` (Xbox renderer) and the launcher keep the BSD 3-Clause licence.

Not included, as for every build of this project: the game, the code recompiled from your `default.xex`
(`generated/`, made by `tools/codegen.sh`), and the game's shaders the Direct3D builds pre-convert (see step 3).

## 1. SDK

The same as the main build (`docs/BUILDING.md`): `tools/setup.sh` clones rexglue-sdk-yukes at
`987593c0fc6612850a2e1b247b2b03aa5281e663` and applies `rexglue-patches/*.patch` (149 patches for v1.7.1).

## 2. Renderer dependencies (plume, XenosRecomp)

The renderer builds against patched copies of plume and reblue-XenosRecomp, exactly as WWE '13 Recomp v1.6.0
prepares them (`native-deps/prepare_native_deps.ps1`, copied from that project's tag `v1.6.0`, commit
`72b907c03472d0516c03c199128af1853ee87754`):

| Folder | Upstream | Commit | Patches (in this order, from `native-deps/patches/`) |
|---|---|---|---|
| `plume` | https://github.com/renderbag/plume | `e0c8871b930dc1a6544bf457f2348c456b9d7e13` | plume-reblue, plume-pipeline-cache, plume-d3d12, plume-d3d11, plume-fullscreen-exclusive, plume-vk-dxgi-present, plume-vk-dxgi-present-active |
| `reblue-XenosRecomp` | https://github.com/hedge-dev/XenosRecomp | `339af41df2c23dbe3256c1c377716b81a0e0fe6b` | xenosrecomp-reblue-wwe12 |

On Linux (the build host this release used), from the folder that holds this repository:

```bash
mkdir native-deps && cd native-deps
git clone https://github.com/renderbag/plume plume
git -C plume checkout e0c8871b930dc1a6544bf457f2348c456b9d7e13 && git -C plume submodule update --init --recursive
for p in plume-reblue plume-pipeline-cache plume-d3d12 plume-d3d11 plume-fullscreen-exclusive \
         plume-vk-dxgi-present plume-vk-dxgi-present-active; do
  git -C plume apply --ignore-whitespace "../../wwe2k14-recomp/native-deps/patches/$p.patch"
done
git clone https://github.com/hedge-dev/XenosRecomp reblue-XenosRecomp
git -C reblue-XenosRecomp checkout 339af41df2c23dbe3256c1c377716b81a0e0fe6b
git -C reblue-XenosRecomp submodule update --init --recursive
git -C reblue-XenosRecomp apply --ignore-whitespace ../../wwe2k14-recomp/native-deps/patches/xenosrecomp-reblue-wwe12.patch
```

(`--ignore-whitespace` is needed because the patches were made on Windows with CRLF line endings.)

Put both in a `native-deps` folder beside this repository (the default of `WWE13_NATIVE_DEPS` in
`cmake/w13r_renderer.cmake`) and build XenosRecomp there (`build/XenosRecomp/XenosRecomp.exe` on Windows,
`build-linux/XenosRecomp/XenosRecomp` on Linux).

## 3. Game shaders for the Direct3D builds

The Direct3D 12 and 11 programs carry pre-converted copies of the game's shaders. They come from your own game: run
the Vulkan build once with `WWE13_DUMP_SHADERS=<folder>` set and play through menus, an entrance and a match, then
point `WWE13_XSC_DIR` at that folder (default `out/w13r/xsc`).

## 4. Build

Windows programs are cross-built from Linux like the main build (`win-amd64-xwin` toolchain). Configure once per
API, with the old native renderer off:

```bash
cmake -S . -B out/build/win-amd64-w13r -G "Ninja Multi-Config" \
  -DCMAKE_TOOLCHAIN_FILE=../tools/rexglue/cmake/toolchains/win-amd64-xwin.cmake \
  -DCMAKE_PREFIX_PATH=<the Windows SDK install that tools/build-windows.sh makes> \
  -DWWE2K14_HAS_NR=OFF -DWWE2K14_W13R=ON -DWWE2K14_W13R_API=vulkan -DWWE2K14_TEST_TOOLS=OFF
cmake --build out/build/win-amd64-w13r --config RelWithDebInfo --target wwe2k14
# Direct3D 12 / 11: same options with -DWWE2K14_W13R_API=d3d12 (or d3d11) in their own build folder;
# -DWWE2K14_W13R_GEN_FROM=out/build/win-amd64-w13r reuses the Vulkan build's compiled game code.
```

Rename each `wwe2k14.exe` to `wwe2k14-vulkan.exe`, `wwe2k14-d3d12.exe` or `wwe2k14-d3d11.exe` and place it next to
the launcher, with `dxcompiler.dll` and `dxil.dll` from the DirectX Shader Compiler v1.8.2407 release
(https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2407) and, optionally, a pipeline seed
`pipelines_seed.w12p` (`tools/w13r/merge-pso-seed.py`).

## Launcher

The launcher's source is in `launcher2/`. Its Settings page starts `wwe2k14-<api>.exe` for the Vulkan, DirectX 12
and DirectX 11 choices and `wwe2k14.exe` for the Xbox choice (`launcher2/core/launch.cpp`, `GameExecutableFor`).

## Linux (v1.7.1)

The Linux program `Linux-x86_64/wwe2k14-vulkan` is the same renderer built natively on Linux (Vulkan only), with the
Linux SDK from `docs/BUILDING.md`:

```bash
cmake -S . -B out/build/linux-amd64-w13r -G "Ninja Multi-Config" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=<the Linux SDK install that tools/setup.sh makes> \
  -DWWE2K14_HAS_NR=OFF -DWWE2K14_W13R=ON -DWWE2K14_W13R_API=vulkan -DWWE2K14_TEST_TOOLS=OFF
cmake --build out/build/linux-amd64-w13r --config RelWithDebInfo --target wwe2k14
```

Rename the result to `wwe2k14-vulkan`; the build copies `libdxcompiler.so` and `libdxil.so` (from reblue-XenosRecomp's
`thirdparty/dxc-bin/lib/x64`) beside it. The SDK's Linux window is a GTK window on X11, so plume is built without its
SDL integration and draws into the window's X11 drawing area (`src/native/w13r/src/reblue/platform/native_window.cpp`).
