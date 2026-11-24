# Deathsmiles for PlayStation Vita

An unofficial PlayStation Vita port of **Deathsmiles** for iPhone and iPod touch (CAVE, version 1.0.9).

The port runs the original iOS game code on the Vita through a custom loader that recreates the parts of iOS the game needs. It runs at a locked 60 FPS on a stock Vita, without overclocking or plugins. You get physical button controls with three control layouts, custom side bezels and a full LiveArea manual.

> This project contains **no game files**. You need your own copy of the iOS game.

---

## Features

- **Full game:** Arcade mode and Smartphone mode, all stages, bosses, endings and credits. Tested with a one-credit clear on real hardware.
- **60 FPS** at the Vita's default clock speeds (333 MHz CPU / 166 MHz GPU).
- **Physical controls** with three layouts, switchable at any time with **SELECT + L**:
  - **Type iOS:** the original iPhone layout, with Auto Shot on.
  - **Type 360:** based on the Xbox 360 version.
  - **Type Custom:** an alternative layout with the shot buttons on the left side.
- **Touch screen still works:** the original on-screen buttons hide as soon as you use the physical buttons, which take priority.
- **Side bezels:** one artwork per character, plus a menu bezel. They replace the original iPhone frame, and dialogue scenes switch to the menu bezel automatically so text and portraits are never covered.
- **LiveArea manual:** 27 pages covering controls, characters, scoring, stages, ranks, items and more.
- **Seamless music** in cutscenes and credits.
- **Faster loading:** the CPU runs at 444 MHz only while the game loads (an official Vita clock speed), and the next data file is read ahead while the current one is decoded.
- **Screenshots:** **L + R + SELECT** saves a PNG to `ux0:data/deathsmiles/shots/`.

## Controls

| Button | Type iOS | Type 360 | Type Custom |
|---|---|---|---|
| D-pad / left stick | Move | Move | Move |
| × | Change direction | Shot / Laser (left) | Shot / Laser (right) |
| ○ | Bomb | Shot / Laser (right) | Bomb |
| □ | Shot / Laser | Lock Shot / Power Up (hold) | Shot / Laser (left) |
| △ | Lock Shot / Power Up (hold) | Bomb | Lock Shot / Power Up (hold) |
| L | — | Turbo shot (left) | Turbo shot (left) |
| R | Lock Shot / Power Up (hold) | Turbo shot (right) | Turbo shot (right) |
| START | Pause | Pause | Pause |
| SELECT + L | Change control type | Change control type | Change control type |

- In Type 360 and Type Custom, Auto Shot is off. Tap a shot button to fire, or hold it to fire the laser. Firing left or right also turns your character to face that side.
- In menus, × and ○ confirm and cancel.
- Your control type is saved in `ux0:data/deathsmiles/controls.txt`.

## Requirements

- A PlayStation Vita or PlayStation TV with HENkaku / h-encore / Ensō. Tested on PCH-2000 and PCH-1000, both on firmware 3.60 with Ensō.
- **kubridge** installed (`ur0:tai/kubridge.skprx` listed under `*KERNEL` in `ur0:tai/config.txt`).
- **libshacccg.suprx** extracted to `ur0:data/` (required by vitaGL).
- Your own copy of **Deathsmiles 1.0.9 for iOS** (`.ipa`). The game executable inside it must be decrypted. Apps downloaded from the App Store are locked to the Apple account that bought them (FairPlay encryption), and that locked code cannot run on the Vita. The port checks this on first launch and tells you if your copy is encrypted or a different version.

## Installation

No PC tools are needed: the port prepares the game files by itself on the first launch.

1. **Extract the IPA.** It is a ZIP file: rename it to `.zip` if needed and extract it. Inside, open `Payload/deathsmiles.app/`.
2. **Copy the game data.** Copy the whole contents of `deathsmiles.app/` to `ux0:data/deathsmiles/app/` on your Vita (for example with VitaShell over USB or FTP).
3. **Install the VPK.** Install `Deathsmiles.vpk` with VitaShell.
4. **Launch** Deathsmiles from the LiveArea. On the first launch the port converts the game executable into `game.bin`. This takes a few seconds and only happens once.

The final layout on your memory card should be:

```
ux0:data/deathsmiles/
├── app/              (contents of deathsmiles.app, copied by you)
├── game.bin          (created on the first launch)
├── Documents/        (save data, created automatically)
├── controls.txt      (created automatically)
└── log.txt           (created automatically)
```

If something goes wrong, the error is shown on screen and saved in `ux0:data/deathsmiles/log.txt`.

## Custom bezels (optional)

The bezels are included in the VPK. To use your own artwork, place a PNG with the same name in `ux0:data/deathsmiles/`; it takes priority over the built-in one.

- `bezel_windia.png`, `bezel_casper.png`, `bezel_follett.png`, `bezel_rosa.png`, `bezel_tiara.png`: shown during gameplay with each character. Size: 960×544, with a transparent centre and 118 px of artwork on each side.
- `bezel_menu.png`: shown in menus and dialogue scenes. Size: 960×544, with 72 px of artwork on each side.
- `bezel.png`: fallback for any character without its own bezel.

## How the port works

Deathsmiles for iOS is an ARMv7 program written in Objective-C, using OpenGL ES 1.1, OpenAL and UIKit. The Vita has the same CPU architecture (32-bit ARMv7), so the original machine code runs natively. The port rebuilds the environment around it.

- **Loader.** On the first launch, the port extracts the ARMv7 part of the iOS executable and converts it into a simple image (`game.bin`) with its relocations and the list of system functions it imports. The loader then maps the image into executable memory through kubridge, applies the relocations and connects every import to a Vita implementation.
- **Objective-C runtime.** A small, thread-safe Objective-C runtime reads the game's own classes, methods and selectors from the executable and handles message sending, retain/release and autorelease pools.
- **iOS frameworks.** The parts of Foundation, UIKit, CoreGraphics, QuartzCore, AudioToolbox, StoreKit and GameKit that the game actually uses are reimplemented in C:
  - views and labels;
  - text rendering with TrueType fonts;
  - bitmap contexts and PNG images;
  - timers and the run loop;
  - file access;
  - notifications.

  Text and image drawing results are cached, so menus and the shop run at 60 FPS.
- **Graphics.** The game's OpenGL ES 1.1 calls go through **vitaGL**. The 480×320 iPhone screen is scaled to 816×544 and centred on the Vita's 960×544 display, and the side bezels are composed around it.
- **Audio.** OpenAL is reimplemented as a 48 kHz software mixer on its own CPU core, costing well under 1 ms per frame. The game's music sequencer chains music segments, and the mixer starts each new segment on the exact sample where the previous one ends, so the music plays without gaps.
- **Input.** The Vita buttons are translated into the game's own MFi game-controller support, and the touch screen is translated into iOS touch events.
- **iOS calling conventions.** Two differences from the Vita had to be handled:
  - iOS reserves CPU register r9, so the port itself is compiled without using it.
  - iOS passes 64-bit values to variadic functions with different alignment, so formatted text, including the save-file checksum, is decoded the iOS way.
- **Save data.** Saves are written atomically, with a backup copy, to `ux0:data/deathsmiles/Documents/`.
- **In-app purchases.** The original App Store purchases (the paid DLC items) can no longer be bought, so the port handles them locally: they show as free, and **Restore** in the game's options unlocks them. Everything bought with the game's own gold still works exactly as in the original: you earn gold by playing and spend it in the shop.

## Known limitations

- Stage transitions can pause for about 1–1.5 seconds. Most of this time is the game's own loader decoding its data; the port already shortens it with the 444 MHz loading boost and read-ahead.
- Game Center is not available. The game's own in-game achievements still work.
- Untested on PlayStation TV.

## Credits

- **Deathsmiles** © CAVE CO., LTD. All rights reserved. This is an unofficial, non-commercial fan project, not affiliated with or endorsed by CAVE.
- **LiveArea manual:** largely based on the **"Deathsmiles Strategy Guide" by Rising Star Games**, adapted and condensed for this port. That guide in turn credits Will Freeman, Jon Rodgers (CAVE), Harry Cole and EOJ of the CAVE-STG forum.
- **VitaSDK** and the Vita homebrew community.
- **vitaGL** by Rinnegatamante.
- **kubridge** by TheFloW.
- **stb_image, stb_truetype and stb_image_write** by Sean Barrett (public domain / MIT).
- **Fonts:** Liberation Sans Bold (SIL Open Font License) and IPAGothic (IPA Font License); license files are included in the VPK.
- **touchHLE**, used as a reference to compare the port's behaviour with the original game.
- Port by nahucirujano.
