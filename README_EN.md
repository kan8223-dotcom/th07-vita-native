# Touhou 7 (PCB) native PS Vita port (th07-vita-native)

An unofficial port of Touhou Youyoumu ~ Perfect Cherry Blossom to the PS Vita.
It runs the same game engine lineage as the PSP port ([th07-psp-native](https://github.com/kan8223-dotcom/th07-psp-native)) natively on the Vita CPU and draws with the Vita GPU (GXM) directly.

**No original game data is included.** You need your own copy of the Windows game (version 1.00b).

## Features

- No extra runtime: GXM is used directly (no OpenGL ES layer); the shaders are precompiled and packaged.
- Copy the game folder and play: no renaming or conversion.
- Noto Sans JP (SIL OFL 1.1) is bundled; MS Gothic is optional.
- Optional MIDI: with Windows' gm.dls the MIDI mode plays with the Microsoft GS Wavetable Synth instruments (96 voices).
- 640x480 stretched to 960x544; the Window screen mode in the options shows 4:3.

## Install

1. Install `th07-vita-native.vpk` with VitaShell (HENkaku / Ensō).
2. Copy the contents of your game folder to `ux0:data/th07/`:
   `th07.dat` and `thbgm.dat` (required), and `th07.exe` or `東方妖々夢.exe`
   (optional: on first start its icon is written to the LiveArea icon).
3. Start the game. Missing files are listed on screen (START exits).

Optional: copy `C:\Windows\Fonts\msgothic.ttc` to `ux0:data/th07/` for the original font.
Optional: copy `C:\Windows\System32\drivers\gm.dls` to `ux0:data/th07/` and choose MIDI in the options.
The first start after that builds `midi-th07.tdsb` (the instruments the songs use) with a progress screen.
Without gm.dls, the MIDI setting plays the WAV soundtrack.
These files and anything made from them are for personal use only; do not redistribute them.

## Controls (same as the PSP port)

D-pad / left stick: move. Cross: shot / OK. Circle: bomb / cancel. Square / L / R: focus. Triangle: skip dialogue. START: pause / menu.

## Performance

PS Vita at CPU 333 MHz / GPU 222 MHz (set by the game), Normal, Reimu A, stages 1-6 in one run, MIDI music (2026-10-18).
Frames over 20 ms / frames in the stage (loading between stages excluded; the first frames of each stage included):
1: 11 / 10,327, 2: 6 / 14,974, 3: 7 / 16,270, 4: 74 / 27,550, 5: 35 / 29,566, 6: 322 / 28,701.

## Other Vita port

[Crystal1612/th07_vita](https://github.com/Crystal1612/th07_vita) uses SDL2 with OpenGL ES (PIB) and needs the PSM Runtime (CrystalPSM), PIB-Configuration-Tool and msgothic.ttc.
This port needs no additional runtime and bundles a font.

## Build

Requires [VitaSDK](https://vitasdk.org/) with its standard packages (SDL2, SDL2_image, SDL2_ttf, libvita2d, FreeType, libpng, libwebp, bzip2, zlib).

```sh
export VITASDK=/path/to/vitasdk
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build
```

## Credits and licenses

- The port: CC0 1.0 ([LICENSE](LICENSE)), following [some100/th07](https://github.com/some100/th07) (portable branch) and th07-psp-native.
  Lineage: some100/th07 → kan8223-dotcom/th07-psp-native → this port.
- `third_party/touhou-modern-vita` and `third_party/touhou_dls_common`: MIT.
- Noto Sans JP: SIL Open Font License 1.1 ([licenses/NotoSansJP/](licenses/NotoSansJP/)).
- Saekaze (formerly Swiizyu) — native Switch ports ([saekaze/touhou-switch](https://github.com/saekaze/touhou-switch)); the spark that started this series of ports.
- Statically linked libraries: [THIRD_PARTY.md](THIRD_PARTY.md). Credits: [CREDITS.md](CREDITS.md).

This software uses the FreeType Project font engine.

Touhou Youyoumu is © Team Shanghai Alice. This port is unofficial and not affiliated with Team Shanghai Alice.
