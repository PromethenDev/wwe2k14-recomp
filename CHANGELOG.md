# Changelog

## v1.6.0 - 2026-11-13

- **WWE 2K15 import (new):** in **Mods**, click **Import from WWE 2K15**: the launcher searches this PC for your own extracted
  WWE 2K15 (Xbox 360) disc (or choose its folder yourself). Pick the wrestlers you want: they are added with their models, attires, entrances,
  victories, move sets and menu pictures. For now they use a stand-in's entrance music and titantron. Tick **Also give
  wrestlers already in WWE 2K14 their 2K15 look** to update 47 wrestlers the game already has (The Rock, Seth Rollins,
  Big Show and more) to their WWE 2K15 attires. **Undo WWE 2K15 import** on the Mods page puts the original files back.
  Play WWE 2K14 once and save before importing.
- **WWE '13 arenas (new):** in **Mods > Import from WWE '13**, click **Import WWE '13 arenas**. It brings over the
  WWE '13 arenas WWE 2K14 does not have (up to 25, including Capitol Punishment, Vengeance, WrestleMania XXVIII and the
  Attitude Era arenas), with lit crowds and entrances. In Select Arena they are at the end of the **WrestleMania** page
  (scroll down past WrestleMania 29). **Undo WWE '13 arenas** on the Mods page puts the original files back. Import the
  wrestlers first: while the arenas are imported, wrestlers cannot be imported or undone.
- **Borderless display (new, Windows):** **Settings > Display > Borderless** fills the screen like Fullscreen but never
  changes your monitor's refresh rate, so switching windows on 144 Hz / 165 Hz monitors no longer flickers or goes
  black (GitHub #116).
- **Fixed: wrestlers froze when a weapon was set in a corner with the weapon wheel on** (Hell in a Cell, I Quit and
  other matches; GitHub #110).
- **Fixed: crash in the 30 Years of WrestleMania WrestleMania 23 Cena vs Michaels match** (GitHub #125): the game now
  unpacks its files in a safer way.
- **Fixed: crash when scrolling the wrestler lists in Create-an-Entrance, stables, Create-a-Story and Universe** after a
  WWE '13 import, on wrestlers that have no Universe entry yet (GitHub #132).

## v1.5.4 - 2026-11-11

- **Fixed: crash in Hell in a Cell with the weapon wheel** (GitHub #122). With the weapon wheel switched on, the match
  now gets more memory for the items it loads (taken from memory the game never uses), so it no longer runs out.
- **Fixed: crash when picking wrestlers in Create-an-Entrance, stables, Create-a-Story and Universe** after importing
  WWE '13 wrestlers (GitHub #128, #129, #130, #132).
- **Fixed: crash with some WWE '13 titantrons.**
- **Safety check for the 30 Years of WrestleMania crash in WrestleMania 23 Cena vs Michaels** (GitHub #125): when the
  game finds damaged data while unpacking a file, it now skips it instead of crashing.

## v1.5.3 - 2026-11-10

- **Fixed: WWE '13 import said "Importing needs WWE 2K14 Add-On 2" even with all DLC added** (GitHub #103, #111, #118,
  #120). There are two versions of that add-on; the import now works with either one. If your add-ons have no room
  for every wrestler you chose, the message now says how many do not fit.

## v1.5.2 - 2026-11-09

- **Bug Reports page (new):** click **Bug Reports** at the bottom of the launcher. It lists your last 10 games and how
  each one ended (crashes are shown in red), with **Save Report** and **Show Report** for each game, your saved reports,
  and a **Report on GitHub** button. Save the report for the game that went wrong, then drag the file into your GitHub
  message.
- **Crash message (new):** if the game crashes, the launcher comes back by itself, tells you the game crashed, saves a bug
  report for that game (or tells you if it could not), and takes you to the Bug Reports page. The launcher still closes when the game
  starts; a small helper without a window waits for the game in the background.
- **Fixed: crash in Ladder and TLC matches with the weapon wheel** (GitHub #110). For now the weapon wheel is off in
  Ladder and TLC matches, even with **Also in other match types** ticked.
- **Fixed (Linux): the game would not start again after closing it with Alt+F4** on PCs with little memory, until the
  PC was restarted (GitHub #107).
- **WWE '13 import:** when Add-On 2 is not found, the message now lists the add-on folders the launcher did find
  (GitHub #103). Files another program holds for a moment are tried again, and if that still fails the message says why.
- **Black wrestlers on older AMD Radeon cards (RX 560 / RX 580):** not fixed yet, but a bug report from these cards now
  shows exactly what the graphics card refuses, so we can fix it. If this happens to you, please send one from the new
  Bug Reports page (GitHub #44).

## v1.5.1 - 2026-11-08

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

## v1.5.0 - 2026-11-07

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

## v1.4.10 - 2026-11-05

- **Faster on the processor:** the game now uses much less processor time per frame. On our test PC the game's
  main thread dropped from about 16 ms to about 5 ms per frame, and a laptop on battery now holds a steady 60 fps.
  Weaker PCs should benefit most; fast PCs will not see a big change.
- **Picture fixes:** black patches during Undertaker's victory (#91); two shaders that could show random colours;
  slightly wrong reflections on many shiny materials.
- **Long sessions:** video memory no longer keeps growing over many matches in a row (#93).
- **AMD Radeon integrated graphics (for example 780M):** if Elimination Chamber or other big matches crash, please
  update your AMD graphics driver - an old 2023 driver was the cause on our test laptop.
- **Move-Set endless load fixed:** Signatures & Finishers could load forever when the game ran out of cache space; it now skips that optional copy and keeps loading.

## v1.4.9 - 2026-11-02

- **Creation Suite:** rename your created superstars and custom arenas.
- **Logos:** draw new logos or edit existing ones with the built-in paint tool (works with a touchpad, full screen option).
- **Music:** play your songs in the launcher with a position bar to jump anywhere. Change volume, trim, fades, bass
  and treble and hear each change while the song plays. Match a song, or several ticked songs, to the game's own
  music level.
- **Settings:** clearer pages with bigger buttons. **Recommended settings for my PC** now uses your strongest graphics
  card (for example the RTX instead of the built-in graphics on a laptop) and offers to switch to it.
- The game itself is unchanged from v1.4.8.

## v1.4.8 - 2026-11-02

- Fixed: matches with a created superstar (CAW) never finished loading (#72, #84). Post-match replays are off again for now, because turning them on caused this; they come back once both work together.
- Fixed: the in-launcher updater on Linux downloaded the Windows files (#82). This download contains both Windows and Linux files so Linux players on the broken updater can update normally once.

## v1.4.7 - 2026-11-01

- Fixed endless loading screens with DLC installed: at the main menu and in Move-Set > Signatures / Finishers
  (#29, #78, #83).
- Fixed a rare random crash.
- Fixed a possible freeze in cloth physics.
- Freeze reports now say what a stuck loading screen is waiting for.

## v1.4.6 - 2026-10-31

- Fixed Creation Suite export: other drives, game-edited superstars (incl. WWE-superstar heads), and large arenas /
  superstars ("file is too large"). The export window opens in Documents; failures say which step went wrong.
- Clearer message when the launcher is not next to the game program (wwe2k14.exe).
- Better bug reports: a short, readable timeline of each game session (frame rate, hitches, loading, files that
  failed to open, a crash line) and a launcher log of what was clicked and which settings changed. Account names are
  hidden in the launcher log. No measurable cost on a slow Intel UHD 630.
- Bug reports now include the PC's hardware (processor, memory, Windows version, display, power/battery, SSD or hard
  disk, graphics driver and video memory). No computer name, account name or serial numbers.

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
