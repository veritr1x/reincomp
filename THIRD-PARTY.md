# Third-party software shipped beside the game executable

A release folder holds these next to `bunker-game-bootstrap.exe`. All of their
licenses allow redistribution in binary form when the license text (and, for
Apache-2.0, any NOTICE file) travels with the files. A release must therefore
include a `licenses/` folder with the texts named below.

| Component | Files | License | Where the text comes from |
| --- | --- | --- | --- |
| ANGLE (commit `890b5d8f`, with this project's presentation patch) | `libEGL.dll`, `libGLESv2.dll`, `libthird_party_zlib.dll` | BSD-3-Clause, plus the licenses of the parts built into it (astc-encoder, jsoncpp, rapidjson, SPIRV-Headers, SPIRV-Tools, Vulkan headers/loader/tools, Vulkan Memory Allocator, zlib) | `work/angle-scaling/bundle/licenses/` (13 files) |
| GCC runtime (MinGW-w64) | `libgcc_s_seh-1.dll`, `libstdc++-6.dll` | GPL-3.0 with the GCC Runtime Library Exception | the MinGW-w64 distribution's `COPYING.RUNTIME` and `COPYING3` |
| winpthreads (MinGW-w64) | `libwinpthread-1.dll` | MIT and BSD-3-Clause | the MinGW-w64 distribution's `COPYING.winpthreads.txt` |
| Firebase C++ SDK 9.6.0 | `FirebaseCppApp-9_6_0.dll` | Apache-2.0 | `LICENSE.Firebase.md`, already kept beside the DLL |
| gRPC C# native extension (Grpc.Core 2.26.0) | `grpc_csharp_ext.x64.dll` | Apache-2.0; it statically links BoringSSL, c-ares, zlib, upb and address_sorting | gRPC's `LICENSE` and `NOTICE.txt` at tag v1.26.0 |
| zlib (inflate only) | linked into the executable | zlib license | `zlib.h` |
| Remill (headers and semantics) | linked into the executable | Apache-2.0 | Remill's `LICENSE` |
| Lunar Tear server | `bunker-server.exe`, from Bunker's releases | see Bunker's `THIRD-PARTY.md` | Bunker |

## In this repository

`third_party/` holds headers only, plus one small static library:

| Folder | From | License |
| --- | --- | --- |
| `third_party/remill-include/` | Remill | Apache-2.0 |
| `third_party/zlib/` | zlib: `zlib.h`, `zconf.h` and `libz-inflate.a`, zlib's inflate code built for MinGW-w64 | zlib license |
| `third_party/angle-include/` | ANGLE: the Khronos EGL, GLES and KHR headers | Apache-2.0 and MIT (Khronos) |

## Before sharing a release folder

- Check the 13 texts collected with ANGLE against the third-party parts its
  DLLs contain.
- Add the MinGW-w64 and gRPC texts to the `licenses/` folder.
- Add the third-party notices the Firebase SDK ships; only its Apache-2.0 text
  is there.

## Not shipped

The game APK, its libraries (`libil2cpp.so`, `libunity.so`), its data and the
Firebase configuration read from the APK are the player's own files. A
prebuilt game executable contains code translated from those libraries.
