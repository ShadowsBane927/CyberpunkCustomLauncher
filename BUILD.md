# Building Cyberpunk Custom Launcher & Save Editor from source

This document describes exactly how to reproduce the release build from
source. The toolchain is 100% open-source (mingw-w64, a standard GCC-based
Windows cross-compiler) and every source file in this repository is plain,
readable C.

## What's included in this repository

- `CyberpunkCustomLauncherGUI_v8_source.c`: the main application source
  (menu, save editor, save-file parsing, GDI+ UI)
- `d2d_phase1_test.c`: Direct2D device/swap-chain setup and static image
  compositing for the save editor screen
- `d2d_phase3_text.c`: DirectWrite text rendering for the save editor
- `d2d_menu_paint.c`: Direct2D rendering for the main menu screen
- `saveengine.h`: save-file (`.sav.dat`) parsing/serialization
- `gen_rc.py`: generates the Windows resource script (`resource.rc`) that
  maps embedded asset filenames to resource IDs
- `vendor/lz4/`: the LZ4 compression library (MIT licensed, unmodified,
  from https://github.com/lz4/lz4), used by `saveengine.h` to
  decompress/compress save-file chunks

## Assets

All assets needed to build the exe are included directly in `assets/`:
the main-menu and save-editor artwork (`Menu_specific/`, `Save_specific/`),
the app icon (`app_icon.ico`), and all 5 embedded fonts (see
[LICENSES.md](LICENSES.md) for font licenses: Emblema One, Limelight, and
Cinzel are SIL Open Font License; DSEG7 Classic is SIL Open Font License;
Red Menace is Creative Commons Attribution 3.0). Nothing needs to be
supplied separately - `gen_rc.py` is already configured to find everything
where it is.

## Toolchain setup (Linux/WSL/any POSIX host with apt)

```
apt-get update
apt-get install -y mingw-w64 g++-mingw-w64-x86-64
```

This provides `x86_64-w64-mingw32-gcc` and `x86_64-w64-mingw32-windres`.

## Asset layout (all included in this repository)

```
assets/
  Menu_specific/Menu specific/   <- main-menu artwork
  Save_specific/Save specific/   <- save-editor artwork
  app_icon.ico                   <- application icon
  EmblemaOne-Regular.ttf
  Limelight-Regular.ttf
  Cinzel-VF.ttf
  DSEG7Classic-Regular.ttf
  red-menace.otf
```

Every filename `gen_rc.py` expects is listed explicitly in that script -
open it and search for `rc(` calls if you need the exact expected name
for any individual asset.

## Build steps

```bash
# 1. Generate resource.rc from the asset layout above
python3 gen_rc.py

# 2. Compile the resource script
x86_64-w64-mingw32-windres resource.rc -O coff -o resource.o

# 3. Compile the LZ4 library
x86_64-w64-mingw32-gcc -c vendor/lz4/lz4.c -o lz4.o -O2

# 4. Compile each source file
x86_64-w64-mingw32-gcc -c CyberpunkCustomLauncherGUI_v8_source.c -o main.o -Wall -I vendor/lz4
x86_64-w64-mingw32-gcc -c d2d_phase1_test.c -o d2d_phase1_test.o -Wall
x86_64-w64-mingw32-gcc -c d2d_phase3_text.c -o d2d_phase3_text.o -Wall
x86_64-w64-mingw32-gcc -c d2d_menu_paint.c -o d2d_menu_paint.o -Wall

# 5. Link the final executable
x86_64-w64-mingw32-gcc main.o d2d_phase1_test.o d2d_phase3_text.o d2d_menu_paint.o lz4.o resource.o \
    -o CyberpunkCustomLauncherGUI.exe \
    -mwindows -lgdi32 -lgdiplus -lcomctl32 -lcomdlg32 -lole32 -loleaut32 -luuid \
    -ld2d1 -ldwrite -ldxguid -lwindowscodecs -luser32 -lshell32 -lmsimg32 -luxtheme -lkernel32 \
    -static-libgcc
```

This produces `CyberpunkCustomLauncherGUI.exe`, functionally identical to
the released build.

## Architecture notes

- The app is a single Win32 window that switches between two screens (main
  menu and save editor) based on user navigation.
- Both screens render via Direct2D/DirectWrite (`d2d_*.c` files). GDI/GDI+
  are still used for: native edit-box controls (the click-to-edit stat
  fields in the save editor), image *decoding* (Direct2D bitmaps are
  bridged from already-GDI+-decoded images rather than decoded twice),
  and the Level/Street Cred caption overlay (a separate always-composited
  popup window).
- Save file parsing (`saveengine.h`) handles the CLZF-chunked, LZ4-
  compressed `.sav.dat` format and locates proficiency data via the
  game's `ScriptableSystemsContainer` node structure at a fixed 24-byte
  stride.
