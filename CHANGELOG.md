# Changelog

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
