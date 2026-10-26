# gw3nx: Geometry Wars 3: Dimensions on Nintendo Switch

A native homebrew port of the Android version of **Geometry Wars 3: Dimensions** for hacked
Switch consoles (Atmosphère). The game's own Android ARM code runs unmodified: a dynarmic JIT
translates it, and the port stands in for the Android pieces it expects (libc, OpenGL ES, OpenSL
audio, the APK/OBB asset system and the Java side).

**Bring your own game.** No game files are included. You need your own copy of the Android
version (APK plus its OBB data file).

## Highlights

- **Full 1.3.0 "Evolved" game with the console UI.** Adventure, Local Co-op, Hardcore, Classic,
  Ultimate, Options and Achievements, driven entirely by controller. 1.0.0 also boots.
- **60 fps** gameplay, **1080p docked** and **720p handheld**.
- **Up to 4 controllers** for Local Co-op (Joy-Con, Pro Controller, handheld). Only connected
  controllers count as players.
- **Switch button layout**: A confirms and B goes back, prompts match the labels, and ZL/ZR/L/R
  icons replace the Xbox LT/RT/LB/RB ones.
- **Full sound and music** (FMOD, unmodified).
- **Saves to the SD card**, including the game's cloud-save path.

## Added for the Switch

- **Options > Graphics**: a new page in the game's own Options screen (also reachable from the
  pause menu), built from the game's own menu widgets: **Particle detail** and **Background
  detail** sliders, a **Preset** row (Android / Console / Max, moves both sliders) and **Rumble**
  (Off / Low / Medium / High). Android shipped at roughly 40% of the PS4/Xbox One effect detail;
  the slider goes from that up to 150% of the console values.
- **HD Rumble** for smart bombs, deaths, boss hits and boss kills. The Android build had no rumble
  at all; set it (or turn it off) in Options > Graphics.
- **Achievements, offline**: all 30 achievements are tracked locally. The main menu's
  Achievements tile works again and opens the list in the game's own scrolling page (no Google
  Play sign-in), and unlocks pop up in the style of the game's own pop-ups, in its Futura font.
  Unlocks are saved in `achievements.txt`.
- **gw3nx menu** (hold **−** for 1 second): quick effect presets (Android High / Console / Max),
  an FPS counter and the achievements list.
- **Auto-pause**: pressing HOME or putting the console to sleep mid-level brings you back to the
  game's pause menu.
- **D-pad works in every menu** (on Android only the stick did).
- **Fast loading**: the 80 MB data file is read once into memory instead of in tens of thousands
  of small SD-card reads (that alone took about 70 s of the first load).
- **Original controller diagram** on the Controller page: an angular two-piece pad drawn for this
  port, with the Switch face-button layout and offset sticks.
- **HOME menu icon** and title taken from your own APK at build time.

## Removed or turned off

- **Google Play sign-in prompts** ("SIGN IN (X)") are hidden; the service the game used is gone.
- **Online features**: online leaderboards, friends' scores, Facebook and cloud sync need servers
  that no longer exist.
- **Xbox-style prompts and the Shield-style controller drawing** are replaced (see above).

## 1.0.0 vs 1.3.0 (both run on this port)

| | 1.0.0 (builds 35/41) | 1.3.0 "Evolved" (build 101) |
|---|---|---|
| Interface | Phone UI, touch-first | TV/console UI, controller-first |
| Evolved level pack | Not included | 41 extra levels |
| Main menu | Phone menus | Ring with Adventure, Local Co-op, Hardcore, Classic, Ultimate, Leaderboards, Options, Achievements |
| Data file | `main.35...obb` (1,429 files) | `main.101...obb` (1,286 files) |

1.3.0 is the recommended version: it is the one built for controllers.

## Requirements

- A Switch running Atmosphère, launched with **title takeover** (hold R while starting a game),
  since the game needs about 3 GB of memory.
- From your copy of the Android game, in `/switch/gw3/`:
  - `libgwnext.so`, `libfmodex.so`, `libfmodevent.so` (from the APK, `lib/armeabi-v7a/`)
  - `main.101.com.activision.gw3.dimensions.obb`
  - `gw3nx.nro`

## Known limitations

- The first launch takes a while (the JIT translates the game's code as it first runs).
- Docking or undocking mid-session scales the picture until the next launch (the game picks its
  resolution once, at startup).
- In co-op every controller rumbles, not just the player's.
- Achievement names follow the game's internal order matched to the public Google Play list.

## Credits

- [dynarmic](https://github.com/azahar-emu/dynarmic) (ARM JIT), libnx, devkitPro, Mesa, SDL2.
- The Linux handheld port [gw3-portmaster](https://github.com/GreekAdonis/gw3-portmaster) for
  pioneering notes on this build.
- Geometry Wars 3: Dimensions © Activision / Lucid Games. This project contains no game code or
  assets.
