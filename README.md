# WWE 2K14 Recomp v1.5.1

An unofficial static recompilation of the **Xbox 360 version of WWE 2K14** for 64-bit Windows and Linux. The original PowerPC
game code is translated ahead of time to C++ and built into a native executable using the ReXGlue SDK; this is not an
Xbox 360 emulator.

## What's new in v1.5.1

- **In-game settings menu (new):** press **F1** on the keyboard, or **Back + Start** together on a controller, during
  the game. Change crowd, shadows, Lower CPU use, sync and stretch without going back to the launcher. Your choices are
  saved for next time.
- **Controller vibration switch (new):** a **Controller vibration** tick box on the **Controls** page. Untick it to turn
  rumble off completely. (GitHub #102)
- **Weapon wheel in more matches:** under the Hell in a Cell weapon wheel box on the **Mods** page, tick **Also in other
  match types** to pull objects from under the ring in more exhibition matches. Off by default.
- **Fixed: WWE '13 import could not find WWE 2K14 Add-On 2** when its folder had a different name. The launcher now
  recognises it by its contents. (GitHub #103)
- **Fixed: WWE '13 import stopped with "An LSD registry table has an invalid row count"** on some copies of WWE '13
  add-on files. Those files are accepted now, and if a file really is damaged the message names it.
- **Fixed:** the Creation Suite said a save was "unsupported" when a created superstar used certain WWE Superstars'
  heads (for example Daniel Bryan's).

## What's new in v1.5.0

- **Import from WWE '13 (new):** a button on the new **Mods** page brings wrestlers from your own WWE '13 into WWE 2K14:
  their models, attires, entrances, moves, names, theme music, the ring announcer saying their names, and their own
  titantron videos, converted on your PC. All 106 supported wrestlers, base game and add-ons, or just the ones you tick.
  - The launcher finds your WWE '13 files and add-ons by itself, or you choose the folder at any time.
  - Themes start at WWE 2K14's own music volume. Each wrestler's theme volume can be changed under **Edit**, with a
    button to go back to the game's volume.
  - It checks free disk space first, shows its progress, and can be cancelled. **Undo WWE '13 import** puts back every
    original file exactly. You can import more wrestlers later without starting over.
  - You need your own WWE '13 and your own WWE 2K14 DLC packs. No game files are included in this download.
- **Mods page (new):** imported wrestlers with search, Men and Divas lists, remove and add back several at once, each
  tagged with the game it came from, and a **Pop out list** button for a full-size view.
- **Weapon wheel in Hell in a Cell:** pull any object from under the ring, as in Extreme Rules: chairs, ladders, tables
  and more. The computer uses it too. Off by default; tick it on the Mods page. Exhibition matches only.
- **Full shadows in big matches:** matches with 3 or more wrestlers, including Ladder and TLC, get the same soft rope and
  wrestler shadows on the ring as one-on-one matches. On by default, except on AMD integrated graphics, where it stays
  your choice for now (Settings).
- **Faster on weak graphics:** lighter shaders and less work per frame on built-in graphics such as Intel UHD 630 and
  AMD Radeon 780M. On our Intel UHD 630 test PC: about 55 fps against about 51 for v1.4.10, and steadier (the slowest 5%
  of frames 27 ms against 30 ms). The test matches vary, so treat that as a small gain.
- **Smoother over a whole match:** the game uses video memory more carefully and keeps frame pacing steadier.
- **Two optional add-ons that lower the picture for speed,** both off by default, in Settings: **Performance mode**
  (anti-aliasing off and softer texture filtering) and **Texture filtering: Standard** (softer than the default Sharp).
- **Saves page:** backups show **Normal** or **Modded**, so you know which saves were played with imported wrestlers.
- **Fixed:** black strips on the steel cage and on Hell in a Cell. Launcher settings that did not stay saved. After an
  update, new shaders are prepared again so the first matches don't stutter.

## What's new in v1.4.10

- **Faster on the processor:** the game now uses much less processor time per frame. On our test PC the game's
  main thread dropped from about 16 ms to about 5 ms per frame, and a laptop on battery now holds a steady 60 fps.
  Weaker PCs should benefit most; fast PCs will not see a big change.
- **Picture fixes:** black patches during Undertaker's victory (#91); two shaders that could show random colours;
  slightly wrong reflections on many shiny materials.
- **Long sessions:** video memory no longer keeps growing over many matches in a row (#93).
- **AMD Radeon integrated graphics (for example 780M):** if Elimination Chamber or other big matches crash, please
  update your AMD graphics driver - an old 2023 driver was the cause on our test laptop.
- **Move-Set endless load fixed:** Signatures & Finishers could load forever when the game ran out of cache space; it now skips that optional copy and keeps loading.

## What's new in v1.4.9

- **Creation Suite:** rename your created superstars and custom arenas.
- **Logos:** draw new logos or edit existing ones with the built-in paint tool (works with a touchpad, full screen option).
- **Music:** play your songs in the launcher with a position bar to jump anywhere. Change volume, trim, fades, bass
  and treble and hear each change while the song plays. Match a song, or several ticked songs, to the game's own
  music level.
- **Settings:** clearer pages with bigger buttons. **Recommended settings for my PC** now uses your strongest graphics
  card (for example the RTX instead of the built-in graphics on a laptop) and offers to switch to it.
- The game itself is unchanged from v1.4.8.

## What's new in v1.4.8

- Fixed: matches with a created superstar (CAW) never finished loading (#72, #84). Post-match replays are off
  again for now, because turning them on caused this; they come back once both work together.
- Fixed: the in-launcher updater on Linux downloaded the Windows files (#82). This download contains both Windows and
  Linux files so Linux players on the broken updater can update normally once.

## What's new in v1.4.7

- **Fixed endless loading screens with DLC installed**: at the main menu and in Move-Set > Signatures / Finishers
  (#29, #78, #83).
- Fixed a rare random crash and a possible freeze in cloth physics.
- Freeze reports now say what a stuck loading screen is waiting for.

## What's new in v1.4.6

- **Creation Suite export fixed:** exporting works when the game is on a different drive from where you save the file
  (for example the game on D: and your Desktop on C:), for superstars the game has edited (including ones with a WWE
  superstar's head), and for large arenas and superstars with their own images - there is no size limit any more (the "file is too large" error).
- The export window opens in Documents, and a failed export now says which step went wrong.
- **Better bug reports:** a short, readable timeline of each game session and a launcher log of clicks and setting
  changes, both included in **Save a Bug Report** (account names hidden).

## What's new in v1.4.5

- **Fixed: the game would not start on Intel "Ice Lake" laptop graphics** (for example Core i3-1005G1 / i5-1035G1
  with Intel UHD or Iris Plus) with the Native renderer in v1.4.4 (#77). One compatibility shader was missing.

## What's new in v1.4.4

- **Smoother matches on a slow graphics chip.** Tested on an Intel UHD 630 at 480p: frames on time went from 42 % to
  82 %, and speed from 52.7 to 62.3 fps. Crowds are now drawn with far fewer steps, and less memory is copied each frame.
- **Fixed: matches slowing down on AMD laptop graphics** (Radeon 780M class, Steam Deck, Ryzen laptops). On a Radeon
  780M laptop, matches now run at 59.4 fps with 1.9 % of frames late (was 45.7 fps, 30.5 % late).
- **Fewer first-time stutters:** effects prepared before play are now used directly.
- New launcher home-screen artwork. Windows and Linux are now separate downloads.
- New Settings option **Intel Lake compatibility shaders** (off by default). Turn it on only if the Native renderer
  crashes or looks wrong on an Intel graphics chip.
- New Settings option **Faster loading** (off by default; works with Locked 30).
- Known issues: as v1.4.3. If the title-screen demo has played, choosing a mode can hang - press Start within about
  20 seconds. On some laptops **Lower CPU use** makes entrances less smooth - leave it off unless your PC runs hot or loud.

### From v1.4.3
- New Settings options for slower PCs: **Textures** (Normal / Low) and **Shadows** (On / Off).
- New picture sizes **360p** and **540p**.
- Intel graphics without 64-bit shader support (for example 10th-gen "Ice Lake" laptops) can now use the Native
  renderer instead of getting a black screen (#56).
- Fewer stutters the first time you play: more effects are prepared before play.
- The launcher shows a message instead of closing silently when it cannot open its window (#26).
- Fixed: the chosen game language was ignored while Settings were on Recommended.
- Saves: backup/restore is blocked while the game is running, and a restore is no longer undone by the launcher.
- Known issues: as v1.4.2. On some laptops **Lower CPU use** makes entrances less smooth - leave it off unless your PC
  runs hot or loud.

### From v1.4.2

- New launcher look.
- F11 switches between fullscreen and windowed (#58).
- Fixed white skin on AMD Radeon graphics (Hogan, Savage, Elizabeth and others) (#18).
- Fixed the invisible Money in the Bank briefcase and title belts in Create a Championship (#71).
- Fixed Creation Suite saying "unsupported" for saves with a CAW that uses a WWE Superstar head.
- Fixed bringing saves over from an older install when extracted into a deep folder.
- Fixed laptops with two graphics chips starting at lower settings than the Settings page shows.
- "Preparing shaders" box before play is back.
- Known issues: as v1.4.1. If the title-screen demo has played, choosing a mode can hang - press Start within about
  20 seconds.

### From v1.4.1

- **Creation Suite fixes** (launcher only; the game itself is the same as v1.4.0):
  - Importing some arenas no longer leaves the save "unsupported" (#64). The launcher now checks the save before it
    writes anything.
  - Many more community CAWs and arenas import, including most of the "Best of" save collection (before, many were
    refused as "package list is damaged").
  - Imported arenas no longer show a black ring or floor: each arena is now copied completely, including its look
    settings and full name.
  - Arenas imported with an older launcher can be repaired: open **Creation Suite > Arenas** and press
    **Fix / find existing** once.
- Known issues: on **AMD Radeon integrated graphics** (for example Radeon 780M) the game can still stall for a moment
  or drop to 30 fps, most often the first time a new arena or superstar is shown; we are still working on it. Linux:
  post-match replays stay off for now. With **Frame rate: 60 everywhere** championship belts can shake during
  entrances (use 30 / 60); if the intro video plays and the game goes back to the title screen, choosing a mode can
  hang on a black screen - press **Start** on the first title screen.

### From v1.4.0

- **Beat the Streak crash fixed**: backing out of the "Past WrestleMania Victims" page no longer closes the game (#46).
- **No more slowdown after a few matches in a row** (for example in 30 Years of WrestleMania) on graphics cards with
  less video memory (#69, #70).
- **AMD Radeon integrated graphics** (for example Radeon 780M): no more "device lost" crashes.
- **Post-match replays** play again on Windows (#7).
- **Lower CPU use** is now **off by default** (it could cost smoothness on some PCs); you can still turn it on.
- **Creation Suite**: imports now work with more save files (#63, #64); **export** created superstars and arenas;
  new **Logos** tab (your own pictures as
  custom logos, 256 x 256 or 128 x 128) and **Stories** tab; **Discord** button in the launcher.

### From v1.3.0

- **Creation Suite** (CAW / arena import), **PC keyboard typing** in name boxes (#53), WrestleMania V skin fix (#18),
  Hollywood Hogan's nWo t-shirt, PC no longer sleeps while playing with a controller (#49).

### From v1.2.1

- **Rope shadows fixed** (Native renderer), **Crowd** setting (Full / None), G-SYNC / FreeSync double speed and
  non-English folder name fixes restored from v1.0.1, better graphics errors in the log, Xbox renderer picture size
  shown in the launcher.

### From v1.2.0

- **Created superstars save fix**: saving a created superstar several times in a row (and the signatures / finishers
  screens) no longer freezes the game.
- **Lower CPU use** (Native renderer), **Language** setting, Xbox renderer **Frame rate** and
  **Anti-aliasing** options, Custom Music fixes, Superstar Threads faces, F3 counter fix, 4K memory crash fix, Linux
  start fixes and the game/launcher icon.
- **Coming from an older version?** Extract v1.5.1 into a new folder and start the launcher: it offers to **bring
  everything over** (game folder, title update, DLC, saves, created superstars, music, keys and settings). Updating
  with the launcher's **Update** button keeps everything in place.

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
- **Import from WWE '13** is optional too. It needs your own WWE '13 files and your own WWE 2K14 DLC packs; it tells you
  plainly if something is missing.

## What you need

- **Windows 10 or 11 (64-bit)**, or **Linux** (glibc 2.38 or newer, e.g. Ubuntu 24.04, with GTK3; on Wayland the
  game window runs through XWayland, installed by default on most distributions).
- **CPU**: a 64-bit processor with AVX2 (most PCs from 2013 onward: Intel Haswell / AMD Ryzen or newer).
- **Graphics**: up-to-date Vulkan drivers. With the **Native** renderer, integrated graphics such as AMD Radeon 780M
  and Intel UHD 630 are supported (start with 480p). The **Xbox** renderer needs a dedicated graphics card.
- About 10 GB of free space for the game files.

## Install and play

1. Download `WWE2K14-Recomp-v1.5.1.zip` (the combined Windows and Linux package) and extract it to a normal folder such as Documents or Desktop (not
   `C:\Program Files`). For a manual Linux-only download, use `Linux-WWE2K14-Recomp-v1.5.1.zip`.
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
├── pipelines-seed.keys         # first-start graphics list (Linux: Linux-x86_64/pipelines-seed.keys)
├── wwe2k14.toml                # keyboard keys (change them on the launcher's Controls page)
├── avcodec-58.dll, avutil-56.dll
├── shaders/                    # Native renderer shaders (shared by Windows and Linux)
├── shaders32/                  # Native renderer shaders for GPUs without 64-bit shader support (Intel Ice Lake)
├── crowd-cards/                # extra crowd shaders (Linux: Linux-x86_64/crowd-cards)
├── shadertools/                # converts rare missing shaders at run time (Linux: Linux-x86_64/shadertools)
├── Linux-x86_64/               # Linux launcher + game
├── README.md, CHANGELOG.md, DISCLAIMER.md, CONTRIBUTORS.md, LICENSE, SHA256SUMS
├── licenses/, third-party-licenses/, ffmpeg-source-*.zip
├── WWE 2K14/                   # your game folder (default.xex + default.xexp + data)
├── userdata/                   # saves, DLC and settings (created on first run)
└── logs/                       # game logs (created on first run)
```

## Launcher settings

The launcher shows a recommended setup for your graphics card. You can also choose:

- **Renderer** - **Native** (default, recommended) or **Xbox** (the previous renderer).
- With **Native**: **Picture size** (480p, 720p, 1080p, 1440p, 4K - how sharp the 3D picture is drawn), **Frame
  rate** (Locked 30, 30 / 60, 60 everywhere), **Anti-aliasing** and **Lower CPU use**.
- With **Xbox**: **Resolution** 720p, 1080p or 1440p (the game draws at 1280x720 like the Xbox 360 and is scaled),
  **Frame rate** and **Anti-aliasing** (FXAA).
- **Language** - the game's language (English, Français, Deutsch, Italiano, Español).
- **Display** - fullscreen or windowed (press **F11** in the game to switch at any time); **Graphics card** - for PCs
  with two GPUs (laptops: pick the one you want to play on).
- **Stretch to fill wide screens** - fill a 21:9 or 16:10 screen instead of showing black bars.
- **Creation Suite** - import created superstars and arenas (Beta), and drop your own songs in for the game's
  custom music.
- **Mods** - **Gameplay** changes such as the Hell in a Cell weapon wheel (off by default), and **Import from WWE '13**.
  Importing many wrestlers takes a few minutes and needs free disk space; the launcher checks the space first. Each
  titantron movie is converted on your PC. If one cannot be converted, that wrestler uses a WWE 2K14 titantron instead
  and the launcher tells you. **Undo WWE '13 import** restores your original files.
- **Saves** - **Back Up Now** and **Restore** your saves with one click.

## Updates

When the launcher starts, it asks this repository's **Releases** page whether a newer version exists. If there is one,
the **Play** page shows it with **What's new**, **Update**, **Not now** and **Skip this version**. **Update**
downloads the new release, checks that the download is intact, replaces only the program files and restarts the
launcher. Your game files, saves, DLC, settings, keyboard keys and music are never touched; the previous version's
program files are kept in `backups/update-<version>/`. Nothing about you or your PC is sent. Turn the check off with
**Check for updates** on the **Settings** page.

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

- **AMD Radeon integrated graphics** (for example Radeon 780M): short stalls and drops to 30 fps, most often the
  first time a new arena or superstar is shown. On our one test laptop (Radeon 780M), a heavy match plugged in on its
  Silent power mode can also fall to about 30 fps when the chip's memory slows down; on battery it held 60. The speed
  work in this version reduced that in our test but did not remove it.
- **Post-match replays** are off for now on Windows and Linux (with them on, matches with a created superstar never
  finished loading).
- **Native renderer**: Bobby Heenan's outfit can show a red tint in WrestleMania mode; Randy Savage, Hulk Hogan
  and Miss Elizabeth can look white in 30 Years of WrestleMania (WrestleMania V); John Cena's title belt can shake
  with **60 everywhere** (use **30 / 60**);
  rarely, the loading screen after choosing wrestlers does not finish (restart the game). If something blocks you,
  switch to the **Xbox** renderer.
- **The Back button does not toggle the pre-match options; use the D-pad (arrow keys) instead.** In the
  Create-a-Superstar editor, holding Back (**Tab**) shows a controls legend. Save a created superstar from the
  **OTHER** tab's **ACCEPT** row (see "On-screen prompts and saving a created superstar" above).
- **Import from WWE '13**: do not close the launcher or turn off the PC while it is copying files. If that happens, the
  import cannot repair itself; your original files stay in a backup beside your game folders.
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

## Legal

- This is an unofficial fan project. It isn't affiliated with or endorsed by WWE, 2K, Take-Two Interactive, Yuke's or
  Microsoft. *WWE 2K14* and all related names, characters and game material belong to their respective owners.
- **No game material is included**: no `default.xex`, title update, game data, audio, video, DLC or saves. You must
  own the game and supply your own files. See [DISCLAIMER.md](DISCLAIMER.md).
- **Recompiler and runtime**: the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (this project builds on the
  [rexglue-sdk-yukes](https://github.com/HollywoodAkeem/rexglue-sdk-yukes) fork), which is derived from
  [Xenia](https://github.com/xenia-project/xenia). See their licenses in `third-party-licenses/`.
- **Native renderer**: draws through [plume](https://github.com/renderbag/plume) (MIT license); its shaders are converted
  with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp). Memory is managed with the Vulkan Memory Allocator (MIT).
- **Video**: [FFmpeg](https://ffmpeg.org/) libraries under the LGPL 2.1 or later; the corresponding source is included
  (`ffmpeg-source-*.zip`).
- **Import from WWE '13**: no WWE '13 or WWE 2K14 game files are included; the import works only with files you already
  own. The movie converter (`wwe2k14-bink-convert`) is a separate program under the GNU Affero General Public License
  version 3, built on NihAV; its source is attached to each release (`wwe2k14-bink-convert-source-*.tar.gz`). The LZX
  decoder library (`wwe2k14_lzx`) is from libmspack under the LGPL 2.1; its source is attached too
  (`libmspack-lzx-source-*.tar.gz`). See `licenses/importer-NOTICE.txt`.
- Other components and their licenses are listed in `licenses/` and `third-party-licenses/`.
- **License**: this project's own code is under the BSD 3-Clause [LICENSE](LICENSE). It does not cover WWE 2K14 or any game
  content, and third-party components keep their own licenses.
- Contributors: see [CONTRIBUTORS.md](CONTRIBUTORS.md).
