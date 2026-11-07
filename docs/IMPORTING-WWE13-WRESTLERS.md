# Importing WWE '13 wrestlers into WWE 2K14

WWE 2K14 Recomp v1.5.0 can bring wrestlers from your own copy of WWE '13 into WWE 2K14. It does the work on your PC,
from your own files: their models and attires, entrances, moves, names, theme music, the ring announcer saying their
names, and their own titantron videos.

No game files are included in the download, and none are shared by the import. You need your own copies of both games.

## What you need

- **WWE 2K14 Recomp v1.5.0 or newer**, set up and working (you can start a match).
- **Your WWE 2K14 DLC packs**, added in the launcher on the **Game files** page (**Add TU or DLC**). The import adds the
  wrestlers into the game's add-on content, so it won't run without them.
- **Your own WWE '13 (Xbox 360)**, as an **extracted game folder**: the folder that has `default.xex` and a `pac` folder
  in it. A disc image (ISO) on its own is not enough; extract it first.
- **Optional: your WWE '13 DLC packs** (Pack 2 and Pack 3), copied from your own console, either as the original package
  files or extracted. Without them you can still import every wrestler from the base game.
- **Free disk space**: about **5 GB** for a full import. Part of it is only used while importing.
- **A game folder that isn't buried too deep.** Windows has a limit on how long a file path can be. If your game folder
  is many folders deep, the launcher will ask you to move it somewhere shorter, such as `C:\Games`.

## Importing

1. **Close the game.** The import won't run while WWE 2K14 is open.
2. In the launcher, open **Mods** and press **Import from WWE '13**.
3. The launcher searches your PC for your WWE '13 files and add-ons. This usually takes a few seconds.
   - If you know where they are, press **Choose a folder** at any time and pick the folder that holds them.
   - **Stop search** stops it straight away.
4. The window lists every wrestler it can import. Everyone it found is ticked. Use **Select all** and **Clear**, or tick
   just the ones you want. Anyone it can't import is listed under **Unavailable wrestlers**, with the reason.
5. Press **Import**. The window closes and the progress bar shows on the Mods page.
   - The longest step is converting the titantron videos: "Converting titantrons: 12 of 106".
   - A full import of all 106 takes about **5 to 10 minutes** on a typical PC, less on a fast one. Importing more
     later is quicker.
   - **Don't close the launcher or turn off the PC while it's installing files.** Cancel is safe and leaves your game
     as it was.
6. When it's done, press **Play**. The wrestlers are in **Downloaded Superstars** on the select screen.

## After importing

- **Mods page:** search, show Men or Divas, remove wrestlers from the game and add them back. **Pop out list** opens a
  full-size view of the list.
- **Theme volume:** themes start at WWE 2K14's own music volume. To change one, tick that wrestler, press **Edit** and
  move the slider. **0** is the game's volume, and **Back to WWE 2K14's volume** resets it. Wrestlers who share a song
  share its volume.
- **Importing more:** run the import again and tick the extra wrestlers. Everyone you imported before stays.
- **Undo WWE '13 import** (next to the Import button) removes all imported wrestlers and puts back every original file
  exactly as it was.
- Changes apply the next time you press **Play**.
- **Saves:** on the **Saves** page, backups made while imported wrestlers were in your game are tagged **Modded**.

## If something goes wrong

- **"Importing needs your WWE 2K14 DLC packs"**: add your DLC on the **Game files** page, then import again. (In v1.5.0
  this message names an "Add-ons page" by mistake. It means the Game files page.)
- **Base game not found:** press **Choose a folder** and pick your extracted WWE '13 folder, the one with `default.xex`.
- **Fewer wrestlers than you expected:** the ones from WWE '13's DLC need your WWE '13 DLC packs. Choose the folder that
  holds them too.
- **"Not enough free disk space"**: the message says how much it needs and where. Free some space and try again.
  Nothing has been changed.
- **A titantron didn't convert:** that wrestler gets a WWE 2K14 video instead, and the import tells you who.
- **Slow import:** antivirus software checking each new file can slow it down a lot, and so can a laptop on battery or
  in a quiet or power-saving mode.
- **Already installed wrestlers another way** (an older import kit): undo that first. The launcher won't import on top
  of it.

Please report problems on the project's GitHub Issues or Discord, with a bug report from the launcher
(**Save a Bug Report** at the bottom of the launcher).

## Supported wrestlers (106)

**Divas (14):** AJ Lee '13, Alicia Fox, Beth Phoenix, Brie Bella '13, Eve, Kelly Kelly, Kharma, Layla '13,
Lita '00 ('13), Lita '13, Natalya '13, Nikki Bella '13, Stephanie McMahon '13, Trish Stratus.

**Superstars (92):** Alberto Del Rio '13, Animal, Antonio Cesaro '13, Big Boss Man, Big Show '13, Billy Gunn, Booker T,
Bradshaw '13, Bret Hart '13, Brian Pillman, British Bulldog, Brock Lesnar '13, Brodus Clay '13, Cactus Jack '13,
Chris Jericho '13, Chris Jericho '99 '13, Christian '13, Christian '99 ('13), CM Punk '13, Cody Rhodes '13,
Damien Sandow '13, Daniel Bryan '13, David Otunga '13, Diamond Dallas Page, Dolph Ziggler '13, Drew McIntyre '13,
Dude Love '13, Eddie Guerrero '13, Edge '13, Edge '99 ('13), Epico, Faarooq, Gangrel, Goldust, Grand Master Sexay,
H. Hearst-Helmsley '13, Hawk, Heath Slater '13, Hunico, Jack Swagger '13, JBL '13, Jey Uso, Jimmy Uso,
Jinder Mahal '13, John Cena '04 ('13), John Cena '13, John Laurinaitis, Justin Gabriel '13, Kane '13,
Kane '97-99 ('13), Ken Shamrock, Kevin Nash '13, Kofi Kingston '13, Mankind '13, Mark Henry '13, Mark Henry '98 ('13),
Mike Tyson, Min. Undertaker '13, Mr. McMahon '13, Paul Wight '13, Primo, R-Truth '13, Randy Orton '13,
Rey Mysterio '13, Rikishi, Road Dogg, Ryback '13, Santino Marella '13, Scotty 2 Hotty, Shane McMahon,
Shawn Michaels '13, Sheamus '13, Sin Cara '13, Stone Cold '13, Ted DiBiase, Tensai '13, Terry Funk, The Godfather,
The Great Khali '13, The Miz '13, The Rock '13, The Rock '98-99 ('13), Triple H '13, Triple H '99 ('13),
Undertaker '13, Undertaker '97-98 '13, Vader, Val Venis, Wade Barrett '13, X-Pac, Yoshi Tatsu, Zack Ryder '13.

## Good to know

- Everything happens on your PC, from your files. Nothing is downloaded and nothing is uploaded.
- The video converter used for titantrons is a separate open-source program (GNU AGPL v3). Its source code, and the
  source of the decoder library the import uses (LGPL 2.1), are attached to the release.
- WWE 2K14, WWE '13 and all wrestler names, likenesses and game material belong to their respective owners. This is an
  unofficial fan project, not affiliated with or endorsed by WWE, 2K, Take-Two Interactive, Yuke's or Microsoft.
