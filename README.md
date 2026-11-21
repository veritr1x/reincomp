# re[in]comp

NieR Re[in]carnation as a native Windows program. The build recompiles the
Global APK's ARM64 game code to x86-64 and runs it in the Windows Unity
player with the original artwork and game data. The launcher starts the
offline Lunar Tear server and Pod Programs from [Bunker](https://github.com/veritr1x/bunker).

## AI disclosure

This is a personal hobby project built with extensive AI assistance (OpenAI
Codex and Anthropic Claude) for the code, the testing and this documentation.
It is unofficial and is not affiliated with SQUARE ENIX or Applibot.

## Current state

The native Windows x86-64 player runs the original game logic on the Windows
Unity renderer. Local saves have been tested in the 3D field, storybook,
all seven main tabs, a ten-draw Summons, both Exploration games and the 3D
Viewer. The local server recorded Story and Arena victories, a three-wave
Subjugation clear, an event defeat and all three victories in Fio’s Chambers
of Dusk on Easy. The two side doors awarded 100 gems each. The central boss
awarded Ange de Fleur and unlocked the Normal chamber.
In Chapter 1, 31 measured controls in the 4K Windows deck, battle HUD, pause
menu, settings and rewards lined up with the live English Android game at
1080p reference size.
At native 1080p, the complete Summons Collection Bonus detail, Filter and
Sort dialogs each averaged less than 0.05 RGB levels of difference on the
0–255 scale against the English Android captures.
At 4K Highest, the settled Settings/Profile tab averaged 143.4 FPS. All
4,306 presentations in the final capture were below 8.3 ms, with a 7.361 ms
99th percentile and an 8.052 ms maximum. Opening the tab included an 80.722 ms frame.
On the moving 3D bridge, the player averaged 142.05 FPS over 70 seconds.
The 99th percentile was 7.381 ms, with a 64.391 ms hitch. The 2D storybook
averaged 142.53 FPS with a 7.713 ms 99th percentile and an 8.556 ms maximum.
A fresh Windows installation targets 144 FPS. At 4K Highest,
the published player averaged 124.9 FPS in three-wave Subjugation with an
18.12 ms 99th-percentile frame. Wave two averaged 113.9 FPS. The longest
combat frame was 225 ms. A prior replay of the same deck on the original
cleanup path reached 575 ms. With enough free memory, the player defers three
global asset scans until the game's normal scene cleanup at battle exit.
Returning from rewards still included a 2.05 s loading frame.

- Drag with the mouse or touch. WASD, the left controller stick and D-pad
  operate the movement stick. The right stick moves the pointer. A (Cross on
  PlayStation) presses it, and B (Circle) goes back. The player supports XInput,
  DualShock 4, DualSense and DualSense Edge through Windows.
  In Notices, Rates and Pod Programs, A/Cross taps or drags the page under
  the pointer. The left stick or D-pad scrolls it vertically. B/Circle closes
  Pod Programs and returns to the game.
- Drag a window edge to resize it. Alt+Enter switches between windowed and
  fullscreen. Use `reincomp.exe -render 3840x2160 -fullscreen` to start at 4K.
- Ctrl+B opens and closes Bunker's Pod Programs. The Bunker button in the
  game's header opens the same panel. In Arena battles, Bunker's separate
  MODS button controls No Death, One Hit and Zero Cooldown.
  New Day, Next Day, Show Original Dates and turning off the calendar restart
  the game automatically. Manual backups can be created and restored in Pod Programs.
- Audio was tested through the first chapter's opening in the earlier
  Android-renderer build. Later chapters still need coverage in the Windows player.
- The game sometimes freezes during a loading pause. Close it and start again.
- In-app purchases: the fix is in the build but has not been tried in the shop.

## How to play

You need your own copy of the game. Nothing of the game is in this repository
or in its releases, so the game executable is built on your PC from your APK.

For a Windows EXE, open [Bunker's web builder](https://veritr1x.github.io/bunker/),
choose **Windows**, and select your original Global 3.7.1 ARM64 APK and April
2024 master data. The browser downloads the precompiled runtime and assembles
the EXE locally. On first launch, choose your resource dump in Bunker.

**Your files**

- the game APK: the English 3.7.1 (152) arm64-v8a build
- the master data, `20240404193219.bin.e`
- the asset dump in 7z or ZIP format

**Build the portable EXE from source.** Run `python build/portable.py` with the
original APK, master data and matching Unity Windows player. It generates the
adapter tables, compiles the native components, translates the game and
packages `Nier Reincarnation.exe`. [Command and tool versions](BUILDING.md#build-the-portable-exe)

**Build the playable folder.** The first build imports the assets and recompiles
both native game libraries. [Build prerequisites and options](BUILDING.md)

```
python build/from_apk.py --apk "game.apk" --master "20240404193219.bin.e" --assets "resource_dump_android.7z" --out "build-output"
```

The builder applies Bunker's Global game and master-data patches and embeds the
APK's artwork and app icon. Start `build-output\reincomp\reincomp.exe` after
the build publishes the directory. Closing the game stops the local services.

**Saves.** `reincomp.exe -export-save backup.zip` writes a backup and
`reincomp.exe -import-save backup.zip` restores one. The format is Bunker's,
so a save moves between a phone and the PC either way. Sign in once on a
fresh installation to create its local player before importing. If a backup
contains several players, open Pod Programs with Ctrl+B, open the intended
player under Players and choose **Use this player**.

The scene scripts can open the Menu tab, the emulator player's 3D bridge and
the 2D storybook prologue in the built installation. They can leave the scene
open for play or measure foreground frame times at 4K.
[Scripted scene runs](launcher/README.md#scripted-scene-runs)

## Layout

| Folder | Contents |
| --- | --- |
| `recompiler/` | Turns an ARM64 library into x86-64 objects (Remill, LLVM) |
| `runtime/` | What the recompiled code runs on: dispatch, guest memory, ELF loading |
| `host/` | The Android, JNI, Unity, EGL/GL, audio and libc layers for Windows, and the game's entry point |
| `game/` | What describes this game: where each recompiled function starts, three headers, and code patches. No game code |
| `build/` | The one-command build and its steps |
| `launcher/` | `reincomp.exe` |
| `third_party/` | Headers of Remill, zlib and ANGLE, and the patches ANGLE is built with |

See also [CHANGELOG.md](CHANGELOG.md) and [THIRD-PARTY.md](THIRD-PARTY.md).

## Credits

- [Bunker](https://github.com/veritr1x/bunker), the offline launcher and server build this port plays against.
- [Lunar Tear](https://gitlab.com/walter-sparrow-group/lunar-tear) and
  [Lunar Scripts](https://gitlab.com/walter-sparrow-group/lunar-scripts) by the Walter Sparrow group, the private server.
- [Lunar Base](https://github.com/NMeliksah/lunar-base) and the
  [master data patcher](https://github.com/NavHobbyDev/lunar-tear-masterdata-patcher).
- [Remill](https://github.com/lifting-bits/remill) by Trail of Bits, which lifts ARM64 code to LLVM IR, and LLVM.
- ANGLE, the Firebase C++ SDK, gRPC, zlib and MinGW-w64.
- NieR Re[in]carnation is by SQUARE ENIX and Applibot.

MIT licensed; see [LICENSE](LICENSE).
