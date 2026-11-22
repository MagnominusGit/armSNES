# armSNES

A Super Nintendo and Super Famicom libretro core, with its own standalone app, for ARM and x86
handhelds. It is a port of [ares](https://github.com/ares-emulator/ares) to C.

It has three modes, set with the Mode option. Exact gives the same picture and sound as ares's most
accurate renderer, frame for frame and sample for sample. Balanced keeps nearly all of that at much
less cost. Fast, the default, is quicker again, with its own line renderer on a thread of its own; it
may differ from Exact only in timing too small to see or hear. A saved state from any mode loads in
the others, and states are the same on ARM and x86, so run-ahead and netplay work in every mode.

## Legal

- **Your own games.** armSNES comes with no games or firmware and doesn't download any. Play only
  games you own, dumped from your own cartridges or bought legally.
- **Nothing is shared in netplay.** Each player needs their own copy of the game and of any firmware
  it needs.
- **Not Nintendo.** armSNES is an independent project, not affiliated with or endorsed by Nintendo.
  Super Nintendo, Super Famicom, Satellaview, Sufami Turbo and Super Game Boy are trademarks of
  Nintendo.

## What it runs

- Cartridges on plain boards (LoROM, HiROM and their extended forms, with or without save RAM), and
  on boards with an NEC DSP (DSP-1 to DSP-4, ST010, ST011), an SA-1, a Super FX, an S-DD1, an OBC1,
  a Cx4, an SPC7110 or an ST018, and with the Epson or Sharp real-time clock. Games with a DSP, a Cx4
  or the ST018 need its firmware (`dsp1.rom`, `dsp1b.rom`, `st010.rom`, `cx4.rom`, `st018.rom` and
  so on, or the `.program.rom` and `.data.rom` halves) in the system folder, unless the dump carries
  it.
- BS-X memory packs (`.bs`, with `BS-X.bin`), in the BS-X cartridge with its receiver, and on their
  own boards.
- Sufami Turbo cartridges (`.st`, with `STBIOS.bin`).
- Game Boy and Game Boy Color games (`.gb`, `.gbc`) in the Super Game Boy, with `SGB1.sfc` and
  `sgb1.boot.rom`. The Game Boy is Gambatte, as in armGBA.
- MSU-1 games (`game.msu` and the tracks `game-1.pcm` on, beside the game).
- The event and pirate cartridges whose chips ares or MAME know.
- In RetroArch, its subsystems put a second cartridge in a Sufami Turbo's slot B, a memory pack in a
  slotted cartridge, or a chosen Game Boy game in a Super Game Boy cartridge.
- IPS, UPS and BPS patches beside the game, which RetroArch and the app apply.

## Controls

| Button | What it does |
| --- | --- |
| A, B, X, Y, L, R, Select, Start, D-pad | The controller's own |
| R2 | Fast-forward while held, outside netplay |
| L2 | Fast-forward too; fine aim while a light gun or the mouse aims with the right stick |
| L3, R3 | L and R with turbo, at the Turbo L and R Period |
| Right stick | A, B, X and Y with turbo, pushed toward where each sits on the pad: up X, right A, down B, left Y (Right Stick Turbo) |
| Left stick | The D-pad too, unless Left Stick as D-Pad is off |

With a light gun or the mouse plugged in, the right stick aims instead, and R3 centers the crosshair (with
Light Gun Motion, how the device is held becomes the middle).

Plug in other controllers through the frontend's controller port types, or, in a frontend without
them (such as the app), the Port 1 and Port 2 Device options: the SNES Mouse or the Super Multitap in
either port, the NTT Data Keypad (its keys from the keyboard, in RetroArch) or the Twin Tap, and the
Super Scope or one or two Justifiers in the second. The guns aim with a light gun, the mouse or the
right stick, with a touch on the screen aiming and firing wherever it lands, and Light Gun Motion
adds the device's gyroscope or accelerometer. With the right stick, R is the trigger, L the Super
Scope's cursor button, Select its turbo and Start its pause. The mouse moves with the mouse, the
right stick (R and L its buttons) or a drag on the touch screen, where a tap clicks.

## Options

The menu shows only the options that do something for the game loaded: the chip clocks only for
games with that chip, the Super Game Boy's only for Game Boy games, the light gun options only with a
gun or the mouse plugged in, Fast's own options only in Fast, and the layer and channel switches only
with Layer Selection or Channel Selection on.

| Category | Option | Values (default first) |
| --- | --- | --- |
| System | Mode | Fast, Balanced, Exact |
| | Fast Mode Threads | All, Picture Only, Off |
| | Frame Skip (Fast) | Off, Auto, 1 to 3 |
| | Region (restarts the game) | Auto (from the header), NTSC, PAL |
| | Power-On RAM | Cleared, Patterned |
| | CPU Clock, Super FX Clock, SA-1 Clock | 100%, 25% to 400% |
| Video | Aspect Ratio | Console pixels (8:7), 4:3, Square pixels |
| | Overscan Lines | Hidden, Shown |
| | Crop Top and Bottom, Crop Sides | 0, 4, 8, 12, 16 pixels |
| | Super Game Boy Border | Shown, Hidden |
| | Deep Black Boost | Off, On |
| | Hi-Res Blending | Off, On, Merge |
| | NTSC/PAL Filter | Off, Composite, S-Video |
| | Scanlines (with the filter) | Off, Light, Medium, Dark |
| | HD Mode 7 (Fast and Balanced, without the filter) | Off, 2x to 8x |
| | HD Mode 7 Perspective Correction | On, Off |
| | Enable HD Scales (5x to 8x, from the next game started) | Off, On |
| | Widescreen (Fast and Balanced) | Off, 16:10, 16:9, 2:1, 21:9 |
| | Widescreen Layers | Mode 7 only, All |
| | Widescreen Sprites | Hidden, Shown |
| | Sprite Limit (Fast) | 32 per line, No limit (games still see the console's limit) |
| | Layer Selection | Off, On (shows the switches below; Off shows everything) |
| | Background 1 to 4, Sprites, Color Math | Shown, Hidden |
| Audio | Interpolation | Gaussian, Cubic, Linear, None |
| | Game Boy Volume, MSU-1 Volume, Voice 1 to 8 Volume | 100%, 0% to 90% in tenths |
| | Channel Selection | Off, On (shows the switches below; Off plays everything) |
| | Game Boy, MSU-1, Voice 1 to 8 | On, Off (a channel keeps its volume for when it's back on) |
| Input | Turbo Buttons | Off, A, B, X, Y, A and B, X and Y, all four |
| | Turbo Button Period (A, B, X, Y) | 4 frames, 2 to 60 frames |
| | Turbo L and R Period (L3, R3) | 4 frames, 2 to 60 frames |
| | Right Stick Turbo | On, Off |
| | Allow Opposing Directions | Off, On |
| | Left Stick as D-Pad | On, Off |
| | Port 1 Device, Port 2 Device (where the frontend has no controller menu) | SNES Controller, SNES Mouse, Super Multitap, NTT Data Keypad, Twin Tap, None; Super Scope, Justifier, Two Justifiers in port 2 |
| | Rumble (homebrew) | Off, On |
| Light Guns and Mouse | Light Gun Aim | Right stick (moves), Right stick (points), Light gun, Mouse |
| | Light Gun Crosshair | On, Off |
| | Aim Sensitivity | 100%, 25% to 300% |
| | Aim Stick Invert | Off, Horizontal, Vertical, Both |
| | Light Gun Motion | Off, Gyroscope, Accelerometer |
| | Light Gun Motion Range | Whole screen, Fine |
| | Light Gun Motion Sensitivity | 100%, 25% to 300% |
| | Light Gun Motion Invert | Off, Horizontal, Vertical, Both |
| | Light Gun Motion Sensor Rotation | 0°, 90°, 180°, 270° |

Widescreen adds columns on each side, drawn from the game's own layers past the console's 256: by
default only on Mode 7 lines, the others keeping black sides, or on every line for games patched for
widescreen. A background one screen wide that isn't scrolled, usually the status bar, stays in the
middle. HD Mode 7 draws Mode 7 lines at two to eight times the resolution; Balanced draws them with
Fast's renderer, from each line's registers as it starts. Room for up to 4x is set aside when a game
starts, so the scale can change while it runs; Enable HD Scales sets aside room for 8x instead, from the
next game started. Perspective Correction smooths a tilted Mode
7 plane's slopes and horizon: where the matrix changes evenly from line to line, the change goes on
through each line's HD rows.

The NTSC/PAL Filter is armNES's: the picture as a TV decoded the console's composite or S-Video
signal, three pixels to each of the console's, with its color fringes, blended dithering and blurred
hi-res. Each line is given twice, the second darkened by the Scanlines option.

Layer Selection and Channel Selection, at the end of Video and Audio, hide parts of the picture or
switch off sound channels, to see or hear one on its own: for ripping graphics or music, or a closer
look. Only what's shown and heard changes: the game plays on the same, so run-ahead and save states
aren't affected. A voice switched off keeps its echo, as the game's memory holds it.

## Building

    cmake -S . -B build
    cmake --build build --target armsnes_libretro

This makes `armsnes_libretro.so`; `libretro/armsnes_libretro.info` goes in RetroArch's info
directory. It builds for x86-64, 64-bit ARM and 32-bit ARM (ARMv7).

`snesrun` runs a game without a frontend (`build/snesrun game.sfc 600`), and `tools/ref/build.sh`
builds `aresrun`, the same runner on unmodified ares, to compare against.

## Standalone app

`armsnes` runs games in a window without RetroArch. It is Armulators' app (the `armulators`
submodule), which armGBA, armNES and armN64 share; armSNES gives it its names and systems
(`app.cmake`) and its achievement hook (`app/`):

    git submodule update --init
    cmake -S . -B build -DARMSNES_APP=ON
    cmake --build build --target armsnes-app

`armulators/package/appimage/build.sh`, then `package.sh` beside it, make AppImages for x86-64 and
64-bit ARM Linux in a Debian 11 container (Docker), with SDL2 built in.
`armulators/package/android/build.sh` makes the Android APK.

### Running and controls

Run `armsnes game.sfc` (or a `.smc`, `.bs`, `.st`, `.gb`, `.gbc`, `.zip` or `.7z` file), or drop a
game on the window. Esc, a controller's Hotkey + X or a right-click opens the menu, where all of
these can be changed.

| Keyboard | Does |
| --- | --- |
| Arrow keys | D-pad |
| X, Z, S, A | A, B, X, Y |
| Q, W | L, R |
| Enter, Backspace | Start, Select |
| 1, 2, 3, 4 | L2, R2, L3, R3 |
| Tab (held) | Fast-forward |
| R | Rewind |
| P | Pause |
| F2 | Restart |
| F5, F7 | Save, load a state |
| F6, F8 | Change the state slot |
| F3 | Show the frame rate |
| F9 | Start or stop recording |
| F11 | Full screen |
| F12 | Screenshot |

Controllers work too, one per player, up to four, with ROCKNIX's hotkeys held with the Hotkey (Home,
or Select on a controller without one): Start twice quits (the second held a second), A pauses, R
and L save and load a state, R2 turns fast-forward on or off, L2 rewinds, X opens the menu, Y shows
the frame rate and B takes a screenshot. The app has no mouse device or keyboard keys for the core:
the SNES Mouse and the guns move with the right stick or a touch, and the NTT Data Keypad needs
RetroArch. The subsystems are RetroArch's too.

### Menu

The menu has four tabs, which L and R move between: General (recent games, Load Game, the app's
settings and Quit), Game (state slots with pictures, cheats, Restart, screenshots, recording and Close
Game), Core (the core's options, as above) and Extras (netplay and RetroAchievements). The game list
shows a Super Nintendo, Satellaview, Sufami Turbo or Super Game Boy folder in the Games folder as that
system.

### Files

The app's settings are in `~/.config/armsnes/frontend.cfg`, the core's options in
`~/.config/armsnes/armsnes.cfg` and the recent games in `~/.config/armsnes/recent.txt`. Firmware
goes in `~/.config/armsnes/system`, and saves, states, screenshots and cheats in
`~/.local/share/armsnes`. On ROCKNIX, the app follows ROCKNIX's other standalone emulators: firmware
comes from `roms/bios`, saves sit beside the games, and states and screenshots go in
`roms/savestates` and `roms/screenshots`. Saves and Folders in the app's settings changes each of
these.

The plan and how far it has got are in [docs/ROADMAP.md](docs/ROADMAP.md).

## License

GPL-2.0-or-later; see [LICENSE](LICENSE). The code ported from ares keeps ares's notice, in
[LICENSES/ISC-ares.txt](LICENSES/ISC-ares.txt). `gambatte/` is Gambatte-Speedrun's core under
GPL-2.0 (its [COPYING](gambatte/COPYING)), so the built core is GPL-2.0. The app's own licenses are
in `armulators/`.
