# WWE 2K14 Recomp v1.7.2

An unofficial static recompilation of the **Xbox 360 version of WWE 2K14** for 64-bit Windows and Linux. The original PowerPC
game code is translated ahead of time to C++ and built into a native executable using the ReXGlue SDK; this is not an
Xbox 360 emulator.

## What's new in v1.7.2

- **Faster match loading on the new renderer:** matches load about as fast as on the Xbox renderer again. On an Intel
  UHD 630 the wait from the arena to the entrance went from about 23 to 8 seconds on Vulkan (DirectX 12: 25 to 8,
  DirectX 11: 15 to 8). The game's loading work was only getting a tiny turn each frame (GitHub #193).
- **Possible fix for a black screen on DirectX 12 with older Intel graphics** (HD 520 and similar): when the graphics
  driver refuses one kind of memory the new renderer uses, it now switches to another kind instead of drawing nothing.
  Not confirmed on an HD 520 yet (GitHub #46).
- **Bug reports** now say whether the game was closed by the player or crashed.

## What's new in v1.7.1

- **New renderer on Linux too:** the Linux version now draws through the same new renderer (Vulkan). **Settings >
  Renderer** offers **Vulkan** (recommended) or **Xbox** there.
- **Freeze reports work with the new renderer** (Windows and Linux): the game writes a freeze report again if the
  picture stops.

## What's new in v1.7.0

- **New renderer (Windows):** WWE 2K14 now draws through the WWE '13 v1.6 native renderer by iMrBurn. It is smoother
  and starts much faster: on an Intel UHD 630 a 720p match went from about 42 to about 58 fps, and the first picture
  appears in a few seconds instead of about 40. On an AMD Radeon 780M laptop the game holds its 60 / 30 fps with about
  half the stutters.
- **Renderer choice:** **Settings > Renderer** offers **Vulkan** (recommended), **DirectX 12** and **DirectX 11** (the
  new renderer) or **Xbox** (the original Xbox 360 renderer). Old settings start on Vulkan.
- **New Settings page:** one row per setting with arrows to change it, and a help box that explains the highlighted
  row. Works with mouse, keyboard and controller.
- **Graphics card choice** now also applies to the new renderer (laptops with two graphics chips).
- **In the game (new renderer):** **F1** or **Back + Start** opens its settings, **F3** shows the frame rate.
- **Fixed:** imported WWE '13 arenas together with DLC packs left the game on the title screen (GitHub #171, #175, #176).
- **Fixed:** after a WWE 2K15 import, the Mods page said "Nothing imported yet" (GitHub #178).
- **WWE 2K15 titantrons** convert with the right colours (GitHub #155).
- **Crash reports:** crashes that used to leave nothing in the game log now write the reason first (GitHub #174, #177, #179).
- **Linux** kept the previous native renderer in v1.7.0 (v1.7.1 adds the new one).

## What's new in v1.6.1

- **Imported wrestlers in the main select grid (new):** your WWE '13 and WWE 2K15 imports now appear in the main
  Superstars and Divas grids with their own faces, so you no longer have to open Downloaded Superstars. They fill the
  free places first; press **Down** past the bottom row for the next page of imports and **Up** past the top row to go
  back. Every row can be reached with Up and Down. Turn it off in **Mods > Gameplay** if you prefer the old way.
- **Faces for wrestlers you imported before:** when you press Play, the launcher adds a select-screen face for every
  imported wrestler that has none. No need to import again; the first Play after updating takes a few seconds longer.
- **Unlock everything (new):** **Mods > Gameplay > Unlock everything** opens every Unlockable and every 30 Years of
  WrestleMania era and match. Your save keeps what is unlocked, so a save backup is made first (GitHub #109).
- **Import in any order:** you can import from WWE '13 while WWE 2K15 wrestlers are imported; the launcher takes the
  WWE 2K15 wrestlers out and puts them back by itself.
- **WWE 2K15 wrestlers** bring their own entrance themes, and the ring announcer says their names.
- **Capes and long coats** of imported WWE '13 wrestlers (Ministry Undertaker, Undertaker '13) now hang and swing instead
  of floating.
- **Fixed:** imported wrestlers sometimes came out to a stock WWE 2K14 theme instead of their own.
- **Keyboard (new option):** keep keys bound to buttons working while you type names (GitHub #87).
- **WWE '13 import into a folder changed by another copy of the launcher** now repairs that folder first, then imports
  (GitHub #141).
- **Mods page** has a tidier layout: **Wrestlers**, **Import** and **Gameplay** tabs, with one card per game for importing
  and undoing.
- **Linux: WWE '13 / WWE 2K15 imports** no longer show placeholder portraits or load matches endlessly (add-on files are now loaded in the right order).
- **Moving to a new folder** now keeps your imported wrestlers instead of showing "Error Roster" (GitHub #132).
- **Created-wrestler import** should accept more saves that it used to refuse (GitHub #115, #75): anything it doesn't recognise is left untouched, and a full backup is still made first. If yours is still refused, please send a bug report.
- **Black outfits** (GitHub #105): a missing graphics helper file is now warned about.
- **Select screen** can list more than 32 add-on wrestlers per pack.
- **WWE 2K15 import:** all 74 WWE 2K15 wrestlers can be imported.
- **WWE '13 imports** get their victory motions (GitHub #142) and more free row numbers (GitHub #141).
- **Linux WWE '13 import** finds files in any letter case (GitHub #135).
- **Add-ons installed twice** (in two userdata folders) are now loaded only once.
- **Bug reports** include more information, and a stuck loading screen now saves a report.

## What's new in v1.6.0

- **WWE 2K15 import (new):** in **Mods**, click **Import from WWE 2K15**: the launcher searches this PC for your own WWE 2K15 (Xbox 360) disc image
  (.iso) or extracted disc (or press **Choose a disc image** / **Choose a folder** yourself); no need to extract the disc. Pick the wrestlers you want: they are added with their models, attires, entrances,
  victories, move sets and menu pictures. For now they use a stand-in's entrance music and titantron. Tick **Also give
  wrestlers already in WWE 2K14 their 2K15 look** to update 47 wrestlers the game already has (The Rock, Seth Rollins,
  Big Show and more) to their WWE 2K15 attires. **Undo WWE 2K15 import** on the Mods page puts the original files back.
  Play WWE 2K14 once and save before importing. If one wrestler cannot be built (for example because a mod changed the
  WWE 2K14 entrance file his entrance starts from), he is left out, the others are imported, and the launcher says why.
- **WWE '13 import reads your disc image (new):** **Import from WWE '13** and **Import WWE '13 arenas** now also
  find your WWE '13 (Xbox 360) disc image (.iso) and read it directly: no need to extract the disc. You can also pick
  the .iso yourself with **Choose a disc image**.
- **WWE '13 arenas (new, experimental):** in **Mods > Import from WWE '13**, click **Import WWE '13 arenas**. It brings over the
  WWE '13 arenas WWE 2K14 does not have (up to 25, including Capitol Punishment, Vengeance, WrestleMania XXVIII and the
  Attitude Era arenas), with lit crowds and entrances. In Select Arena they are at the end of the **WrestleMania** page
  (scroll down past WrestleMania 29). **Undo WWE '13 arenas** on the Mods page puts the original files back. Import the
  wrestlers first: while the arenas are imported, wrestlers cannot be imported or undone. You choose which arenas to
  bring over. **Known issues (experimental):** the arena's big screen stays blank during entrances (no titantron), and
  wrestlers can walk through the entrance ramp instead of on top of it. Both are being worked on.
- **Borderless display (new, Windows):** **Settings > Display > Borderless** fills the screen like Fullscreen but never
  changes your monitor's refresh rate, so switching windows on 144 Hz / 165 Hz monitors no longer flickers or goes
  black (GitHub #116).
- **Fixed: wrestlers froze when a weapon was set in a corner with the weapon wheel on** (Hell in a Cell, I Quit and
  other matches; GitHub #110).
- **Safer file unpacking** (GitHub #125): the game unpacks its files in a safer way. A later out-of-memory crash in the
  30 Years of WrestleMania WrestleMania 23 Cena vs Michaels match is still being looked into.
- **Fixed: crash when scrolling the wrestler lists in Create-an-Entrance, stables, Create-a-Story and Universe** after a
  WWE '13 import, on wrestlers that have no Universe entry yet (GitHub #132).

## What's new in v1.5.4

- **Fixed: crash in Hell in a Cell with the weapon wheel** (GitHub #122). With the weapon wheel switched on, the match
  now gets more memory for the items it loads (taken from memory the game never uses), so it no longer runs out.
- **Fixed: crash when picking wrestlers in Create-an-Entrance, stables, Create-a-Story and Universe** after importing
  WWE '13 wrestlers (GitHub #128, #129, #130, #132).
- **Fixed: crash with some WWE '13 titantrons.**
- **Safety check for the 30 Years of WrestleMania crash in WrestleMania 23 Cena vs Michaels** (GitHub #125): when the
  game finds damaged data while unpacking a file, it now skips it instead of crashing.

## What's new in v1.5.3

- **Fixed: WWE '13 import said "Importing needs WWE 2K14 Add-On 2" even with all DLC added** (GitHub #103, #111, #118,
  #120). There are two versions of that add-on; the import now works with either one. If your add-ons have no room
  for every wrestler you chose, the message now says how many do not fit.

## What's new in v1.5.2

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
- **Coming from an older version?** Extract v1.7.2 into a new folder and start the launcher: it offers to **bring
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
- **Graphics**: up-to-date graphics drivers with Vulkan, DirectX 12 or DirectX 11. With the new renderer, integrated
  graphics such as AMD Radeon 780M and Intel UHD 630 are supported. The **Xbox** renderer needs a dedicated graphics card.
- About 10 GB of free space for the game files.

## Install and play

1. Download `WWE2K14-Recomp-v1.7.2-x64.zip` (the combined Windows and Linux package) and extract it to a normal folder such as Documents or Desktop (not
   `C:\Program Files`). For a manual Linux-only download, use `Linux-WWE2K14-Recomp-v1.7.2-x64.zip`.
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
├── wwe2k14-vulkan.exe, wwe2k14-d3d12.exe, wwe2k14-d3d11.exe   # the game with the new renderer
├── wwe2k14.exe                 # the game with the Xbox renderer (the launcher starts the one you chose)
├── dxcompiler.dll, dxil.dll, pipelines_seed.w12p               # used by the new renderer
├── pipelines-seed.keys         # first-start graphics list (Linux: Linux-x86_64/pipelines-seed.keys)
├── wwe2k14.toml                # keyboard keys (change them on the launcher's Controls page)
├── avcodec-58.dll, avutil-56.dll
├── shaders/                    # shaders of the previous native renderer (kept for compatibility)
├── shaders32/                  # the same for GPUs without 64-bit shader support (Intel Ice Lake)
├── crowd-cards/                # extra crowd shaders (Linux: Linux-x86_64/crowd-cards)
├── shadertools/                # converts rare missing shaders at run time (Linux: Linux-x86_64/shadertools)
├── Linux-x86_64/               # Linux launcher + game (wwe2k14-vulkan = new renderer, wwe2k14 = Xbox renderer)
├── README.md, CHANGELOG.md, DISCLAIMER.md, CONTRIBUTORS.md, LICENSE, SHA256SUMS
├── licenses/, third-party-licenses/, ffmpeg-source-*.zip
├── WWE 2K14/                   # your game folder (default.xex + default.xexp + data)
├── userdata/                   # saves, DLC and settings (created on first run)
└── logs/                       # game logs (created on first run)
```

## Launcher settings

The launcher shows a recommended setup for your graphics card. You can also choose:

- **Renderer** - **Vulkan** (default, recommended), **DirectX 12** or **DirectX 11** (the new renderer), or **Xbox**
  (the original Xbox 360 renderer). Try DirectX 12 or 11 if Vulkan has problems on your PC.
- With the new renderer: **Picture size** (480p to 4K - how sharp the 3D picture is drawn) and **Frame rate** (Locked
  30, 30 / 60, 60 everywhere). More options are in the game: **F1** (or **Back + Start**) opens its settings, **F3**
  shows the frame rate. On Linux the new renderer is Vulkan only.
- With **Xbox**: **Resolution** 720p, 1080p or 1440p (the game draws at 1280x720 like the Xbox 360 and is scaled),
  **Frame rate** and **Anti-aliasing** (FXAA).
- **Language** - the game's language (English, Français, Deutsch, Italiano, Español).
- **Display** - fullscreen or windowed (press **F11** in the game to switch at any time); **Graphics card** - for PCs
  with two GPUs (laptops: pick the one you want to play on).
- **Stretch to fill wide screens** (Xbox renderer) - fill a 21:9 or 16:10 screen instead of showing black bars.
- **Creation Suite** - import created superstars and arenas (Beta), and drop your own songs in for the game's
  custom music.
- **Mods** - three tabs: **Wrestlers** (your imported wrestlers), **Import** (WWE '13 and WWE 2K15) and **Gameplay**
  (imported wrestlers in the main select grid, Unlock everything, the Hell in a Cell weapon wheel).
  Importing many wrestlers takes a few minutes and needs free disk space; the launcher checks the space first. Each
  titantron movie is converted on your PC. If one cannot be converted, that wrestler uses a WWE 2K14 titantron instead
  and the launcher tells you. **Undo** on each game's card restores your original files.
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
- **New renderer**: no crowd, shadow or anti-aliasing options yet; the launcher's Stretch and Sync options apply to the Xbox renderer only.
- **Previous native renderer** (until v1.7.0 on Linux): Bobby Heenan's outfit can show a red tint in WrestleMania mode; Randy Savage, Hulk Hogan
  and Miss Elizabeth can look white in 30 Years of WrestleMania (WrestleMania V); John Cena's title belt can shake
  with **60 everywhere** (use **30 / 60**);
  rarely, the loading screen after choosing wrestlers does not finish (restart the game). If something blocks you,
  switch to the **Xbox** renderer.
- **The Back button does not toggle the pre-match options; use the D-pad (arrow keys) instead.** In the
  Create-a-Superstar editor, holding Back (**Tab**) shows a controls legend. Save a created superstar from the
  **OTHER** tab's **ACCEPT** row (see "On-screen prompts and saving a created superstar" above).
- **WWE '13 arenas (experimental)**: the arena's big screen stays blank during entrances (no titantron), and wrestlers
  can walk through the entrance ramp instead of on top of it.
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

Click **Bug Reports** at the bottom of the launcher. The page lists your last 10 games and how each one ended
(crashes in red). Click **Save Report** on the game that went wrong: it saves a `wwe2k14-bug-report-game-<date>.zip`
next to the launcher with that game's log, the launcher's logs and settings and basic PC information (graphics card,
system); no game files or saves. **Show Report** finds the file. Click **Report on GitHub**, describe what happened and
drag the file into your message. If the game crashed, the launcher comes back by itself, saves that game's report (or says if it
could not) and opens this page for you.

If the game freezes, wait a few seconds, close it, and save the report for that game the same way: the game writes a
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
- **New renderer** (`wwe2k14-vulkan.exe`, `wwe2k14-d3d12.exe`, `wwe2k14-d3d11.exe` and `Linux-x86_64/wwe2k14-vulkan`):
  the WWE '13 Recomp v1.6 native renderer by **iMrBurn**, released jointly with PromethenDev under the **GNU General
  Public License v3**. These programs are therefore distributed under GPL-3.0; their source code is the `v1.7.2` tag
  of this repository. Parts:
  re:Blue (BSD 3-Clause), plume (MIT), the Direct3D 11 backend from Kaiko's WWE SmackDown vs. Raw 2011 PC port (used with
  permission), XenosRecomp (MIT). See `third-party-licenses/new-renderer/`.
- **Shader compiler**: `dxcompiler.dll` and `dxil.dll` are Microsoft's DirectX Shader Compiler v1.8.2407, unchanged.
  `dxil.dll` is under Microsoft's licence terms (`third-party-licenses/dxc/LICENSE-MS.txt`); using it means you accept them.
  Linux uses the same release's `libdxcompiler.so` and `libdxil.so`.
- **Previous native renderer** (inside `wwe2k14.exe` / `wwe2k14`, not offered by the launcher any more): draws through [plume](https://github.com/renderbag/plume) (MIT
  license); its shaders are converted with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp). Memory is managed
  with the Vulkan Memory Allocator (MIT).
- **Video**: [FFmpeg](https://ffmpeg.org/) libraries under the LGPL 2.1 or later; the corresponding source is included
  (`ffmpeg-source-*.zip`).
- **Import from WWE '13**: no WWE '13 or WWE 2K14 game files are included; the import works only with files you already
  own. The movie converter (`wwe2k14-bink-convert`) is a separate program under the GNU Affero General Public License
  version 3, built on NihAV; its source is attached to each release (`wwe2k14-bink-convert-source-*.tar.gz`). The LZX
  decoder library (`wwe2k14_lzx`) is from libmspack under the LGPL 2.1; its source is attached too
  (`libmspack-lzx-source-*.tar.gz`). See `licenses/importer-NOTICE.txt`.
- Other components and their licenses are listed in `licenses/` and `third-party-licenses/`.
- **License**: this project's own code is under the BSD 3-Clause [LICENSE](LICENSE); the new-renderer programs
  as a whole are under GPL-3.0 (see above). Neither covers WWE 2K14 or any game content, and third-party components keep
  their own licenses.
- Contributors: see [CONTRIBUTORS.md](CONTRIBUTORS.md).
