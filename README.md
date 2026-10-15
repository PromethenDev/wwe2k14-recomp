# WWE 2K14 Recomp

An unofficial static recompilation of the **Xbox 360 version of WWE 2K14** for 64-bit Windows and Linux. The original
PowerPC game code is translated ahead of time to C++ and built into a native executable using the ReXGlue SDK; this
is not an Xbox 360 emulator.

## Before you download

You must own a legally obtained Xbox 360 copy of WWE 2K14 and supply the game files yourself. This project does
**not** include the retail `default.xex`, title-update file, game assets, audio, video, DLC, or saves. The executable
is a compiled recompilation derived from the game; it is not a substitute for supplying the required game data. See
[DISCLAIMER.md](DISCLAIMER.md).

## Required game files

- **Base game** files from your own copy (a game folder, or your own disc image `.iso`).
- **Title update 1.0.3** (`default.xexp` and its data files). It is required: the recompilation was built from the
  updated game, so the game refuses to start without it and tells you why.
- **DLC packs** are optional.

## What you need

- **Windows 10 or 11 (64-bit)**, or **Linux** (glibc 2.38 or newer, e.g. Ubuntu 24.04, with GTK3; on Wayland the
  game window runs through XWayland, installed by default on most distributions).
- **CPU**: a 64-bit processor with AVX2 (most PCs from 2013 onward: Intel Haswell / AMD Ryzen or newer).
- **Graphics**: a graphics card with up-to-date Vulkan drivers. A dedicated graphics card is recommended:
  integrated graphics are not supported well yet (on Intel UHD 630 entrances and matches can show a black picture,
  and AMD Radeon 780M laptops run slowly).
- About 10 GB of free space for the game files.

## Install and play

1. Download `WWE2K14-Recomp-v1.0.0-x64.zip` and extract it to a normal folder such as Documents or Desktop (not
   `C:\Program Files`).
2. Start **`WWE 2K14 Launcher.exe`** (Linux: `Linux-x86_64/wwe2k14-launcher`).
3. In **Game files**, do one of the following:
   - put your game folder in the `WWE 2K14` folder beside the launcher, or
   - press **Find Automatically** to search this PC, or
   - press **Choose a Folder**, or **Use a Disc Image** to extract your own `.iso` once.
4. Add the title update (and DLC packs, if you have them) with **Add TU or DLC**. The launcher accepts the package
   files you have and installs them where the game expects them.
5. Press **Play**. The very first start takes a few extra seconds while the game prepares its graphics; after that it
   starts normally.

The launcher checks every file and shows **Game files ready** when everything is correct.

```text
WWE2K14-Recomp-x64/
├── WWE 2K14 Launcher.exe       # start here (Windows)
├── wwe2k14.exe                 # the game (started by the launcher)
├── wwe2k14.toml                # keyboard keys (change them on the launcher's Controls page)
├── avcodec-58.dll, avutil-56.dll
├── Linux-x86_64/               # Linux launcher + game
├── README.md, DISCLAIMER.md, LICENSE, SHA256SUMS
├── licenses/, third-party-licenses/, ffmpeg-source-*.zip
├── WWE 2K14/                   # your game folder (default.xex + default.xexp + data)
├── userdata/                   # saves, DLC and settings (created on first run)
└── logs/                       # game logs (created on first run)
```

## Launcher settings

The launcher shows a recommended setup for your graphics card. You can also choose:

- **Resolution** – 720p is the original Xbox 360 sharpness; 1080p and 1440p are sharper and need a stronger graphics
  card.
- **Display** – fullscreen or windowed (press **F11** in the game to switch at any time); **Graphics card** – for PCs
  with two GPUs.
- **Start straight away next time** – skip the launcher on later starts. Hold **Shift** while it starts to come back
  to the settings.
- **Music** – drop your own songs in to use them in the game's custom music.
- **Saves** – **Back Up Now** and **Restore** your saves with one click.

WWE 2K14 does not have working in-game frame-rate or anti-aliasing overrides in this recompilation, so the launcher
no longer offers those options; the picture runs at the game's own pace.

## Updates

When the launcher starts, it asks this repository's **Releases** page whether a newer version exists. If there is one,
the **Play** page shows it with **What's new**, **Update**, **Not now** and **Skip this version**. **Update**
downloads the new release, checks that the download is intact, replaces only the program files and restarts the
launcher. Your game files, saves, DLC, settings, keyboard keys and music are never touched; the previous version's
program files are kept in `backups/update-<version>/`. Nothing about you or your PC is sent. Turn the check off with
**Check for updates when the launcher starts** on the **Settings** page.

## Controls

An Xbox-compatible controller is recommended. The keyboard also works as a controller: the default keys follow modern
WWE 2K games on PC - **WASD** move, **J** strike (X), **L** grapple (A; also confirms in menus), **K** Irish whip /
pin (B; back in menus), **Space** signature / finisher (Y), **1** reversal (RT), **Left Shift** run (LT), **U** pick
up / in-out of the ring (LB), **Left Ctrl** limb targeting (RB), arrow keys taunts (D-pad), **Esc** pause. See and
change every key on the launcher's **Controls** page. When the game's on-screen keyboard is open (for example, naming
a story), you can type with your PC keyboard and paste with **Ctrl+V**. Names are upper case, like on the console.

## Saves and DLC

The game keeps saves, DLC and settings in `userdata/` beside the launcher. It is local to this installation; use
**Back Up Now** on the launcher's **Saves** page (or copy the folder) before moving or replacing a release.

## On-screen prompts and saving a created superstar

- **ATTENTION dialogs start on NO.** When the game asks a yes/no question - for example the "create save data?" prompt
  on the first start, or the custom-data notice - the **NO** button is selected first. Press **Left** (controller:
  D-pad left) to move to **YES**, then **L** (controller **A**) to confirm.
- **Saving a created superstar (Create a Superstar):** in the editor, switch to the **OTHER** tab (**RB**, keyboard
  **F**), move the highlight down to the last row **ACCEPT** and press **L** (controller **A**). Answer the ATTENTION
  prompt about unattended attire with **Left** + **L** (YES). The editor's Back overlay (hold **Tab**) is only a
  controls legend, not the save menu.

## Known issues

- **The Back button does not toggle the pre-match options; use the D-pad (arrow keys) instead.** In the
  Create-a-Superstar editor, holding Back (**Tab**) shows a controls legend. Save a created superstar from the
  **OTHER** tab's **ACCEPT** row (see "On-screen prompts and saving a created superstar" above).
- Some shadow and lighting differences remain compared to the console.
- Frame rates can dip in active play on some systems (the game's timing follows the frame rate).
- Extract the release to a normal folder, not `C:\Program Files`: the launcher keeps your saves and settings next to
  itself (it warns you if the folder is read-only).
- Only the exact title update 1.0.3 is supported; other regions, game revisions or title updates are refused.
- In menus, the modern keyboard keys confirm with **L** and go back with **K** (they follow the controller's A and B
  buttons). The Back button is the exception noted above.

## Reporting bugs

Press **Save a Bug Report (logs + settings)** at the bottom of the launcher. It saves a `wwe2k14-bug-report-<date>.zip`
next to the launcher with the latest game logs, your launcher settings and basic PC information (graphics card,
system); no game files or saves. Attach it to a new issue on this repository's **Issues** page and describe what
happened. If the game crashed, its log ends with a crash report that shows exactly where.

If the game freezes, wait a few seconds and then use **Save a Bug Report** and send it: the game writes a
`wwe2k14-freeze-<date>.txt` into its `logs` folder (and the report zip always includes it) that records every guest
thread and what it was waiting on at the moment it stopped presenting.

## Building from source

This is a developer build, not a one-command player install. Full guide: [docs/BUILDING.md](docs/BUILDING.md).

1. Run `tools/setup.sh`: it clones the public ReXGlue fork at a pinned commit into `../tools/rexglue`, applies this
   project's SDK fixes from `rexglue-patches/` and builds the SDK.
2. Copy your own `default.xex` and `default.xexp` (title update 1.0.3) into `game/`, then run `tools/codegen.sh`.
3. Build the game (`tools/build-linux.sh`, `tools/build-windows.sh`) and the launcher, then package with
   `tools/package-release.sh`.

`game/`, `generated/` and all retail files stay local: they are never committed or redistributed.

## Credits

- **ReXGlue SDK** — Xbox 360 PowerPC-to-x64 recompilation runtime and tooling; this project builds on the
  [rexglue-sdk-yukes](https://github.com/HollywoodAkeem/rexglue-sdk-yukes) fork. ReXGlue is based in part on work from
  **Xenia**, the Xbox 360 emulator project.
- **Xenia contributors** — foundational Xbox 360 system and GPU research/code acknowledged by the ReXGlue SDK.
- WWE 2K14 and all related game material are the property of their respective rights holders. This project is
  unofficial and is not endorsed by them.

Licensed under the BSD 3-Clause License ([LICENSE](LICENSE)); see also [DISCLAIMER.md](DISCLAIMER.md).
