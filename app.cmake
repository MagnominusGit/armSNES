# What the standalone app (Armulators' app/) is for this core: its names, Android package, look and the
# games it opens. CMake writes them into app-info.h for the C code.
set(APP_ID armsnes)
set(APP_NAME armSNES)
set(APP_WORDMARK ARMSNES)
set(APP_ACCENT Purple)
set(APP_JAVA_PACKAGE org.armsnes.app)
set(APP_JAVA_CLASS Armsnes)
# Its version, and how it's built: the CMake option that turns it on, and its target.
set(APP_VERSION 0.1a)
set(APP_BUILD_OPTION ARMSNES_APP)
set(APP_BUILD_TARGET armsnes-app)
# For the desktop: what it is, and the files it opens.
set(APP_COMMENT "Super Nintendo, Satellaview, Sufami Turbo and Super Game Boy emulator")
set(APP_MIME application/vnd.nintendo.snes.rom application/x-gameboy-rom application/x-gameboy-color-rom application/zip)
# The console's screen: the window is a multiple of it before a game says its own.
set(APP_WINDOW 256 224)
# The game list's files (archives too), and what's looked for inside an archive.
set(APP_EXTENSIONS sfc smc bs st gb gbc zip 7z)
set(APP_ARCHIVED sfc smc bs st gb gbc)
# The systems: each one's key (its folder setting is <key>_folder), name, and the folder names it's
# found by in the Games folder (its key and these, any case).
set(APP_SYSTEMS snes satellaview sufami sgb)
set(APP_SYSTEM_snes "Super Nintendo" sfc superfamicom "super famicom" "super nintendo")
set(APP_SYSTEM_satellaview "Satellaview" bsx bs-x)
set(APP_SYSTEM_sufami "Sufami Turbo" sufamiturbo "sufami turbo")
set(APP_SYSTEM_sgb "Super Game Boy" supergameboy "super game boy")
# The keyboard's default keys, as SDL names them (NONE for none), in libretro's order: B, Y, Select,
# Start, Up, Down, Left, Right, A, X, L, R, L2, R2, L3, R3. APP_BUTTON_NAMES names them in the menus,
# - for one the core doesn't use, and APP_BUTTON_ORDER lists them by libretro's numbers.
# RetroArch's keys: Z, A, X and S are B, Y, A and X, Q and W the shoulders, 1 to 4 L2 to R3.
set(APP_KEYS Z A BACKSPACE RETURN UP DOWN LEFT RIGHT X S Q W 1 2 3 4)
set(APP_BUTTON_NAMES B Y Select Start "D-Pad Up" "D-Pad Down" "D-Pad Left" "D-Pad Right" A X L R L2 R2 L3 R3)
# A, B, X, Y, the shoulders, Select, Start, the D-pad, then L2, R2, L3 and R3.
set(APP_BUTTON_ORDER 8 0 9 1 10 11 2 3 4 5 6 7 12 13 14 15)
# Help texts that name the console's buttons and what the app draws over its picture.
set(APP_HELP_SWAP "Off: A is the right button, as on a Super Nintendo controller. On: A and B swap, and X and Y.")
