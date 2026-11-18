# Third-party software shipped beside the game executable

A folder to play from holds these next to `bunker-game-bootstrap.exe`. All of
their licenses allow redistribution in binary form when the license text (and,
for Apache-2.0, any NOTICE file) travels with the files. The deps zip of a
release therefore carries a `licenses/` folder with the texts named below.

| Component | Files | License | Where the text comes from |
| --- | --- | --- | --- |
| ANGLE (commit `890b5d8f`, with the patches in `third_party/angle/`) | `libEGL.dll`, `libGLESv2.dll`, `libthird_party_zlib.dll` | BSD-3-Clause, plus the licenses of the parts built into it (astc-encoder, jsoncpp, rapidjson, SPIRV-Headers, SPIRV-Tools, Vulkan headers/loader/tools, Vulkan Memory Allocator, zlib) | `licenses/angle/` (13 files, from the pinned sources) |
| GCC runtime (MinGW-w64) | `libgcc_s_seh-1.dll`, `libstdc++-6.dll` | GPL-3.0 with the GCC Runtime Library Exception | `licenses/mingw-w64/gcc-COPYING.RUNTIME` and `gcc-COPYING3`, from GCC 15.2.0 |
| winpthreads (MinGW-w64) | `libwinpthread-1.dll` | MIT and BSD-3-Clause | `licenses/mingw-w64/winpthreads-COPYING` and `COPYING.MinGW-w64-runtime.txt`, from MinGW-w64 13.0.0 |
| Firebase C++ SDK 9.6.0 | `FirebaseCppApp-9_6_0.dll` | Apache-2.0 | `licenses/firebase/LICENSE` and `NOTICES`, the SDK's own third-party notices, at tag v9.6.0 |
| gRPC C# native extension (Grpc.Core 2.26.0) | `grpc_csharp_ext.x64.dll` | Apache-2.0; it statically links BoringSSL, c-ares, zlib, upb and address_sorting | `licenses/grpc/`: gRPC's `LICENSE` and `NOTICE.txt` at tag v1.26.0, and the texts of those five at the commits that tag uses |
| zlib (inflate only) | linked into the executable | zlib license | `zlib.h` |
| Remill (headers and semantics) | linked into the executable | Apache-2.0 | Remill's `LICENSE` |
| Lunar Tear server | `bunker-server.exe`, from Bunker's releases | see Bunker's `THIRD-PARTY.md` | Bunker |

## In this repository

`third_party/` holds headers, one small static library, and patches:

| Folder | From | License |
| --- | --- | --- |
| `third_party/remill-include/` | Remill | Apache-2.0 |
| `third_party/zlib/` | zlib: `zlib.h`, `zconf.h` and `libz-inflate.a`, zlib's inflate code built for MinGW-w64 | zlib license |
| `third_party/angle-include/` | ANGLE: the Khronos EGL, GLES and KHR headers | Apache-2.0 and MIT (Khronos) |
| `third_party/angle/` | This project's patches to ANGLE, with the pins of the sources it is built from | BSD-3-Clause, as ANGLE |

## Still to check

- The 13 texts collected with ANGLE have not been checked one by one against
  the third-party parts its DLLs contain.
- The Firebase and gRPC DLLs need the Microsoft Visual C++ runtime, which is
  not shipped: it comes with Windows updates or Microsoft's redistributable.

## Not shipped

The game APK, its libraries (`libil2cpp.so`, `libunity.so`), its data and the
Firebase configuration read from the APK are the player's own files. A
prebuilt game executable contains code translated from those libraries.
