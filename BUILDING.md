# Building

Everything here runs on Windows. No Linux or WSL is needed.

## What to install

| What | For |
| --- | --- |
| Python 3.12 | the build scripts |
| LLVM 18.1.8 for Windows (`clang+llvm-18.1.8-x86_64-pc-windows-msvc`, from LLVM's releases) | compiling the recompiled code; it must be version 18 |
| MinGW-w64 GCC 15 (`g++` on `PATH`) | compiling and linking the game executable |
| The toolchain: `remill-lift.exe`, `aarch64.bc`, `poll-instrument.exe` | recompiling the game's libraries. From a release's toolchain zip, or built as below |
| Go 1.25 | only to build the launcher yourself |

## From the APK to a folder to play from

```
python build/from_apk.py --apk <your APK> --toolchain <toolchain folder> --clang <LLVM 18>/bin/clang.exe --deps <deps folder> --launcher reincomp.exe --server bunker-server.exe --out <folder>
```

It runs these steps, each of which can also be run by hand. An interrupted run
continues where it stopped.

1. **Check the APK** and take `libunity.so` and `libil2cpp.so` out of it.
2. **Recompile both libraries** (`recompiler/lift_all.py`). Each library is
   lifted from its list of trace heads in `game/`, in shards of 128, with the
   code patches of `game/patches.json` applied first. About 45 minutes on a
   14-core machine: 4,749 shards.
3. **Collect the pack** (`build/make_pack.py`): the objects, in link order.
4. **Build the executable** (`build/build.py --pack <pack> --out <folder>`):
   compiles the host layer, links it with the pack, and runs the post-link
   helper inliner. About a minute; later builds recompile only what changed.
5. **Assemble the folder** (`build/make_release.py`).

`--deps`, `--launcher` and `--server` are optional: without them it stops at
the game executable.

The deps folder holds what ships beside the game: `angle/` (ANGLE's
`libEGL.dll`, `libGLESv2.dll` and the DLLs they need),
`FirebaseCppApp-9_6_0.dll`, `grpc_csharp_ext.x64.dll` and `licenses/`
([THIRD-PARTY.md](THIRD-PARTY.md)). The server is Bunker's `bunker-server`
for Windows, from Bunker's releases. The launcher's import and save commands
need a server newer than Bunker 1.12.10; until Bunker's next release, build
it from Bunker's `desktop-pods` branch and pass `--unpinned-server`. The deps folder is not published
anywhere yet: ANGLE has to be built with this project's presentation patch,
and the two DLLs come from the Firebase C++ SDK 9.6.0 and the Grpc.Core 2.26.0
package.

A pack is made from the game's own code. Keep it, and anything built from it,
out of this repository.

## The toolchain

```
python recompiler/windows/build_toolchain.py --llvm <LLVM 18 folder> --work <work folder> --out <toolchain folder>
```

Run it from a Visual Studio developer prompt (MSVC and the Windows SDK), with
`cmake`, `ninja` and `git` on `PATH`. It fetches the Remill fork at its pinned
commit, gflags and glog, and builds in about three minutes:

- `remill-lift.exe`, the lifter, for AArch64 only
- `aarch64.bc`, the lifter's instruction semantics
- `poll-instrument.exe`, which prepares lifted code for the runtime

The release workflow builds the same three files.

## When the game stops on code that was never recompiled

The log says `unresolved indirect target at 0x...`. Lift that address and add
it:

```
python recompiler/tools/lift_new_traces.py --lib <library> --bias <base> --traces-h <build folder>/generated/game_traces.h --seeds seeds.json --out <folder> --lifter <remill-lift> --semantics <aarch64.bc> --poll <poll-instrument>
python build/add_traces.py <pack> <folder>/result.json
```

`add_traces.py` also adds the new heads to `game/`, so the next full build
includes them. `recompiler/tools/find_code_pointers.py` and
`find_resume_points.py` produce seed lists for whole regions ahead of time.

## The launcher

```
cd launcher
go build -trimpath -ldflags "-s -w -X main.version=<version>" -o reincomp.exe .
```

## Checking a change without the game

`python build/build.py --compile-only --out <folder>` compiles the whole host
layer and stops. It needs no pack; CI runs it on every push.

`work/` is ignored: keep toolchains, packs and builds there.
