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
- Slow: about 40 fps in gameplay, and loading screens are long.
- Sound is new: music and effects play through the first chapter's opening;
  later parts of the game may still stop on audio code that is not recompiled
  yet. `reincomp.exe -nosound` turns sound off.
- The window is fixed at 1280×720.
- You must already have the game's files imported for the server; there is no
  importer on Windows yet.

## How to play

You need your own copy of the game. Nothing of the game is in this repository.

1. Get a release folder: build it ([BUILDING.md](BUILDING.md)), or get one
   from someone who has.
2. Put your game APK in that folder, next to `reincomp.exe`. It must be the
   English 3.7.1 (152) arm64-v8a build.
3. Run `reincomp.exe`. The first start prepares the game files, which takes
   about a minute. If the server's game files are not in `data\assets`, run
   `reincomp.exe -assets <folder>`.

Closing the game stops the server. Saves and logs are in the `data` folder.

## Layout

| Folder | Contents |
| --- | --- |
| `recompiler/` | Turns an ARM64 library into x86-64 objects (Remill, LLVM) |
| `runtime/` | What the recompiled code runs on: dispatch, guest memory, ELF loading |
| `host/` | The Android, JNI, Unity, EGL/GL and libc layers for Windows, and the game's entry point |
| `build/` | The one-command build and the release assembly |
| `launcher/` | `reincomp.exe` |

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
