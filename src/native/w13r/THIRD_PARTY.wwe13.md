> **In WWE 2K14 Recomp:** this file is kept as it came from the WWE '13 Recomp v1.6 fork and describes that project.
> In WWE 2K14 the renderer builds as `wwe2k14-vulkan.exe`, `wwe2k14-d3d12.exe` and `wwe2k14-d3d11.exe`; unlike WWE '13,
> the WWE 2K14 release ships Microsoft's `dxcompiler.dll` and `dxil.dll` beside them (see
> `third-party-licenses/dxc/` in the release). The licences named below are in this folder (`LICENSE.GPL-3.0`) and in
> the WWE '13 Recomp repository.

# Third-party work and credits

- **WWE '13 Recomp** by **PromethenDev** and contributors
  ([PromethenDev/wwe13-recomp](https://github.com/PromethenDev/wwe13-recomp)), BSD 3-Clause (notice kept in
  `LICENSE.BSD-3-Clause-PromethenDev`). This fork builds on their recompilation, launcher, SDK patches and
  research; the project as a whole is GPL-3.0 (`LICENSE`).
- **WWE '13 fork, SDK 0.10 port and native renderers** (Vulkan, Direct3D 12, Direct3D 11) by **iMrBurn**
  (GPL-3.0), released jointly with PromethenDev from v1.6. The native renderers are experimental.
- **Native renderer** (from the WWE '12 PC project, iMrBurn/wwe12-pc, GPL-3.0): re:Blue (zolaware, BSD-3)
  and plume (zolaware, MIT), adapted from nefariousjosiah's WWE SmackDown vs. Raw 2008-2010 recomp ports
  (GPL-3.0, derived from the UnleashedRecomp line); XenosRecomp (hedge-dev) for shader conversion.
- **Direct3D 11 renderer** (`wwe13_d3d11.exe`, `port/patches/plume-d3d11.patch`, `port/tools/convert_shaders_d3d11.py`,
  `port/tools/host_shader_d3d11.py`, `port/src/reblue/gpu/d3d11_slots.cpp`): plume's Direct3D 11 backend and the
  fxc shader conversion come from **Kaiko** (KaikoClanworth1)'s WWE SmackDown vs. Raw 2011 PC port, adapted for
  WWE '12 PC with their written permission (2026-10-26), and taken from WWE '12 PC here. The Direct3D 12 build-time
  shader pre-linking (`port/src/prelink`) also comes from WWE '12 PC. Microsoft's shader compilers (DXC, fxc)
  run only when building; no Microsoft DLL ships with `wwe13_d3d12.exe` or `wwe13_d3d11.exe`.
- **ReXGlue SDK**, via the [rexglue-sdk-yukes](https://github.com/HollywoodAkeem/rexglue-sdk-yukes) fork,
  based in part on **Xenia** (BSD). The WWE '13 changes to it are in `rexglue-patches/`.
- **Icon** (`res/wwe13.ico`, `res/wwe13.png`): made from the WWE logo of the game's Xbox 360 title
  thumbnail. The WWE logo is a trademark of WWE; it is used by this unofficial fan project, which is not
  affiliated with or endorsed by WWE, THQ or Yuke's.
- The **EXIT** main menu entry follows the WWE '12 PC port (iMrBurn/wwe12-pc) and the WWE 2K14 recomp.

No game data (executables, title updates, archives, audio, video, textures, DLC or saves) is part of this
repository or of its release archives.
