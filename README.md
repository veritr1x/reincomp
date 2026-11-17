# re[in]comp

NieR Re[in]carnation as a native Windows program. The game's ARM64 Android
libraries are statically recompiled to x86-64, and the Android, Unity and
OpenGL ES layers they expect are provided natively. It plays offline against
the Lunar Tear server from [Bunker](https://github.com/veritr1x/bunker).

## AI disclosure

This is a personal hobby project built with extensive AI assistance (OpenAI
Codex and Anthropic Claude) for the code, the testing and this documentation.
It is unofficial and is not affiliated with SQUARE ENIX or Applibot.

## Current state

Windows x86-64 only, and not finished.

- Works: the game starts, signs in to the local server and plays, at up to 4K,
  with the keyboard and with sound.
- Slow: about 35 to 45 fps in gameplay, and loading screens are long.
- Sound is new. It plays through the first chapter's opening; later parts of
  the game may still stop on audio code that is not recompiled yet.
  `reincomp.exe -nosound` turns it off.
- The game sometimes freezes during a loading pause. Close it and start again.
- In-app purchases: the fix is in the build but has not been tried in the shop.
- The window is fixed at 1280×720.

## How to play

You need your own copy of the game. Nothing of the game is in this repository
or in its releases, so the game executable is built on your PC from your APK.

**Your files**

- the game APK: the English 3.7.1 (152) arm64-v8a build
- the master data, `20240404193219.bin.e`
- the resource dump, `resource_dump_android.7z`

**Once: build the folder to play from.** This recompiles the game, which takes
about an hour. See [BUILDING.md](BUILDING.md) for what to install first.

```
python build/from_apk.py --apk <your APK> --toolchain <toolchain folder> --clang <clang.exe of LLVM 18> --deps <deps folder> --launcher reincomp.exe --server bunker-server.exe --out <folder>
```

**Then: play.** Put your three files in the `reincomp` folder it made and run
`reincomp.exe`. The first start prepares the APK and imports the dump (about
20 GB; it took 23 minutes here). After that it only starts the server and the game.
Closing the game stops the server.

**Saves.** `reincomp.exe -export-save backup.zip` writes a backup and
`reincomp.exe -import-save backup.zip` restores one. The format is Bunker's,
so a save moves between a phone and the PC either way.

## Layout

| Folder | Contents |
| --- | --- |
| `recompiler/` | Turns an ARM64 library into x86-64 objects (Remill, LLVM) |
| `runtime/` | What the recompiled code runs on: dispatch, guest memory, ELF loading |
| `host/` | The Android, JNI, Unity, EGL/GL, audio and libc layers for Windows, and the game's entry point |
| `game/` | What describes this game: where each recompiled function starts, three headers, and code patches. No game code |
| `build/` | The one-command build and its steps |
| `launcher/` | `reincomp.exe` |
| `third_party/` | Headers of Remill, zlib and ANGLE |

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
