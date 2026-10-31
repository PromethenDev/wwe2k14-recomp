# Changelog

## v1.4.5 - 2026-10-30

- Fixed: the game would not start on Intel "Ice Lake" laptop graphics with the Native renderer (#77).

## v1.4.4 - 2026-10-30

- Smoother matches on slow graphics chips (Intel UHD 630 at 480p: on time 42 % -> 82 %, 52.7 -> 62.3 fps).
- Fixed matches slowing down on AMD laptop graphics (Radeon 780M class, Steam Deck, Ryzen laptops): 45.7 -> 59.4 fps on a
  Radeon 780M (#76).
- Fewer first-time stutters: effects prepared before play are now used directly.
- New launcher home-screen artwork. Windows and Linux are separate downloads (Windows 212 -> 160 MB).
- New Settings options: Intel Lake compatibility shaders (off by default) and Faster loading (off by default).

## v1.4.3 - 2026-10-29

- New Settings options for slower PCs: Textures (Normal / Low) and Shadows (On / Off).
- Fewer first-time stutters: more effects are prepared before play.
- Intel graphics without 64-bit shader support (for example 10th-gen "Ice Lake" laptops) can use the Native renderer
  instead of getting a black screen (#56).
- The launcher shows a message instead of closing silently when it cannot open its window (#26).
- New picture sizes 360p and 540p.
- Fixed: the chosen game language was ignored while Settings were on Recommended.
- Saves: backup/restore is blocked while the game runs, and a restore no longer gets undone by the launcher.
- No false freeze report while shaders are prepared before play.

## v1.4.2 - 2026-10-28

- New launcher look.
- F11 switches between fullscreen and windowed (#58).
- Fixed white skin on AMD Radeon graphics (#18).
- Fixed the invisible Money in the Bank briefcase and Create a Championship title belts (#71).
- Fixed Creation Suite "unsupported" saves with a CAW that uses a WWE Superstar head.
- Fixed importing an older install into a deep folder.
- Fixed dual-graphics laptops starting at lower settings than shown.
- "Preparing shaders" box before play is back.
- Known issues: as v1.4.1; choosing a mode after the title-screen demo played can hang (press Start within ~20 s).

## v1.4.1 - 2026-10-28

- Creation Suite (launcher only; the game is unchanged from v1.4.0):
  - Fixed "unsupported save" after importing some arenas (#64); the launcher checks the save before writing anything.
  - Many more community CAWs and arenas import (for example the "Best of" save collection); fewer "package list is
    damaged" errors.
  - Fixed imported arenas showing a black ring or floor: arenas are copied completely (look settings, full name).
  - Arenas imported with an older launcher are repaired with Creation Suite > Arenas > Fix / find existing.
- Known issues: as v1.4.0 (AMD Radeon integrated graphics stalls are still being worked on).

## v1.4.0 - 2026-10-27

- Fixed the crash when backing out of the Beat the Streak "Past WrestleMania Victims" page (#46).
- Fixed the slowdown after a few matches in a row (for example 30 Years of WrestleMania) on graphics cards with less
  video memory (#69, #70): old render surfaces are released when video memory runs low.
- Fixed "device lost" crashes on AMD Radeon integrated graphics (for example Radeon 780M).
- Post-match replays play again on Windows (#7).
- "Lower CPU use" is now off by default (it could cost smoothness on some PCs).
- Creation Suite: imports work with more save files (#63, #64); export created superstars and arenas; custom logos
  can be 128 x 128 as well as 256 x 256; new Logos and Stories tabs; Discord button.
- Updated Native shaders (reflections).
- Known issues: on AMD Radeon integrated graphics (for example Radeon 780M) the game can stall for a moment or drop
  to 30 fps, most often the first time a new arena or superstar is shown - this will be fixed in the next build.
  Linux: post-match replays stay off for now. With Frame rate "60 everywhere" championship belts can shake during
  entrances (use 30/60). If the intro video plays and the game returns to the title screen, choosing a mode can hang
  on a black screen - press Start on the first title screen.

## v1.3.0 - 2026-10-26

- New launcher **Creation Suite** (Beta): one place for Superstars, Arenas and Music, in tabs.
  - Import a created superstar (CAW) or created arena from a .cas/.car file, a .zip or an unpacked folder; the game
    lists it with its name under Creations > Edit. Downloaded .rar files must be
    extracted first.
  - CAWs and arenas already in your save folder (for example copied in by hand) are set up automatically, so the game
    lists them too; older imports are repaired the same way.
  - Remove an imported creation again. A full save backup is made before every change.
- Type names with your PC keyboard in the game's name boxes (#53).
- Fixed white or yellow skin in 30 Years of WrestleMania (WrestleMania V: Hulk Hogan, Randy Savage, Miss Elizabeth;
  also Bobby Heenan and nWo Macho Man) with the Native renderer (#18): shiny / reflective surfaces now use the same
  reflection maths as the Xbox renderer.
- Fixed Hollywood Hogan's nWo t-shirt missing until he rips it off (Native renderer).
- The PC and screen no longer go to sleep while you play with a controller (#49).
- Known issues: with Frame rate "60 everywhere" championship belts can shake during entrances (use 30/60);
  if the intro video plays and the game returns to the title screen, choosing a mode can hang on a black screen -
  press Start on the first title screen.

## v1.2.1 - 2026-10-25

- Fixed rope shadows showing as solid coloured lines on the mat (Native renderer).
- New launcher **Crowd** setting (Native renderer): Full or None.
- Restored v1.0.1 fixes that were missing in v1.1.0 and v1.2.0: no double speed on G-SYNC / FreeSync / high-refresh
  monitors (#6); the game starts with non-English letters in user or folder names and keeps running when its log
  cannot be written (#21); graphics errors and the Intel driver version in the log (#8, #23); Linux counts the
  processor cores the game may use.
- The "too few processor cores" warning is logged once.
- The launcher shows the Xbox renderer's picture size.
- The v1.2.0 created-superstar save fix and Custom Music fix now take effect (the v1.2.0 launcher did not pass
  them to the game).
- Known issue: Randy Savage, Hulk Hogan and Miss Elizabeth can look white in 30 Years of WrestleMania
  (WrestleMania V) with the Native renderer (#18).

## v1.2.0 - 2026-10-24

- Fixed the game freezing when a created superstar is saved several times in a row (also signatures / finishers).
- New launcher option **Lower CPU use** (Native renderer, on by default): about two-thirds less CPU on the game's main
  thread at the same speed.
- New **Language** setting (English, Français, Deutsch, Italiano, Español).
- Xbox renderer: **Frame rate** (Locked 30 / 30-60 / 60 everywhere) and **Anti-aliasing** (FXAA) options.
- Custom Music Jukebox songs repeat properly and stop when a match loads.
- Fixed Superstar Threads faces (Batista, Big John Studd and others).
- Fixed the F3 frame counter showing about 300 fps.
- Fixed a crash at 4K picture size when memory runs out.
- Linux launcher: restores the game file's run permission and shows the real start error.
- Game and launcher icon.
- Bug reports record what the game waits on if the loading screen after choosing wrestlers never finishes.

## v1.1.0 - 2026-10-23

- New **Native renderer** (default): draws the game directly with Vulkan; much faster, especially on integrated
  graphics. Picture size 480p / 720p / 1080p / 1440p / 4K, frame rate Locked 30 / 30-60 / 60 everywhere,
  anti-aliasing on or off.
- Launcher **Renderer** choice: **Native** or **Xbox** (the previous renderer), with hover descriptions.
- Fixed stutter and crashes on laptops with AMD integrated graphics (Radeon 780M): the renderer keeps fast video
  memory within the driver's limit.
- Fewer first-time pauses: graphics effects are prepared when the game starts.
- Moving to a new folder: on its first start the launcher finds your previous installation and offers to bring
  everything over (game folder with the title update, DLC, saves, created superstars, music, keys, settings);
  later via **Game files -> Import Previous Installation**.
- Windows and Linux builds, as in v1.0.1.

## 1.0.1

Fixes
- The game no longer fails to start when Windows uses non-English letters in the user or folder name (for example
  "Masaüstü"), and it keeps running when its log file cannot be written (for example in a OneDrive folder). (#21)
- The launcher has the right size and is sharp on high-resolution screens with Windows display scaling. (#25)
- Fullscreen on high-refresh, G-SYNC or FreeSync monitors: the game checks its own speed and switches to its built-in
  timer when the monitor does not run it at 60, instead of running at double speed. If a monitor still causes
  trouble, untick **Sync frames to the monitor** in the launcher. (#6)
- The launcher no longer starts the game a second time when it is already running.
- The "too few processor cores" warning is logged once instead of filling the log.

New settings
- **Picture detail (experimental)**: draw the game at a true 1440p or 4K. (#17)
- **Stretch to fill wide screens** for 21:9 and 16:10 monitors.
- **Sync frames to the monitor** (Windows).
- The resolution setting is now called **Output size** and says what it does: it sets the window or screen size;
  the game itself draws at 1280x720 unless Picture detail is raised. (#17)

Help for crash reports
- If the game closes while starting, the launcher says so and offers **Save a Bug Report**; on Intel graphics it
  suggests updating the graphics driver. (#8, #21, #23)
- The game log now names graphics errors and shows the Intel driver version. (#8, #23)
- The launcher warns when the game is installed in a OneDrive folder.

Known limits
- Older Intel HD/UHD graphics (for example the HD 520) can still close at the title screen; these chips are too weak
  for the current renderer. (#8, #23)
- Slow motion on low-end PCs and laptops is not improved in this release. (#3, #13, #14, #20)
