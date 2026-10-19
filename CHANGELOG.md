# Changelog

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
