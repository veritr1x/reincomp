# Building

## The game executable

```sh
python build/build.py --pack <pack folder> --out <output folder>
```

This compiles the host layer (the sources in `build/host_sources.txt`, the
game's entry point and the ELF loader), links it with the recompiled game
objects of a **pack**, and runs the post-link helper inliner. A clean build
takes about a minute on a 14-core machine; later builds recompile only the
files whose includes changed.

You need MinGW-w64 GCC 15 (`g++` on `PATH`, or `--cxx <path>`) and Python 3.

## The pack

A pack is the game-derived half of the build:

- `objects/`: the recompiled game objects, in link order
- `generated/`: the headers made with them (`game_traces.h` and three more)
- `remill-include/`, `zlib-include/`, `angle-include/`, `libz-inflate.a`
- `pack.json`: the list `build.py` reads

`python build/make_pack.py <link response> <build folder> <pack folder>`
collects a pack from an existing build. A pack is made from the game's own
code, so it is never committed here.

When the game stops on code that was never lifted ("unresolved indirect
target"), lift it and add it to the pack:

```sh
python3 recompiler/tools/lift_new_traces.py --lib <library> --bias <base> \n  --traces-h <pack>/generated/game_traces.h --seeds seeds.json --out <folder> \n  --lifter <remill-lift> --semantics <aarch64.bc> --poll <poll-instrument>
python build/add_traces.py <pack> <folder>/result.json
```

`recompiler/tools/find_code_pointers.py` and `find_resume_points.py` produce
seed lists for whole regions ahead of time.

**Making a pack from an APK is not one command.** `recompiler/compile.py`
plans and lifts a library, and `recompiler/tools/relift_shards.sh` relifts
shards with the instrumenter, but no script takes an APK to the full object
set and its generated headers.

## The recompiler toolchain

`recompiler/remill/fetch.sh` clones the Remill fork into `work/remill-src`;
`recompiler/remill/dev.sh` builds and tests it in a container. The lifter and
its semantics must be built and kept as a matching pair. The instrumenter is
built with `python recompiler/build_poll_instrument.py`. The recompiler runs
on Linux or WSL with LLVM/Clang 18 and the Python packages in
`recompiler/requirements.txt`.

## The launcher

```sh
cd launcher
CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=<version>" -o reincomp.exe .
```

## The release folder

```sh
python build/make_release.py --exe <game exe> --launcher <reincomp.exe> \
  --deps <dependency folder> --server <bunker-server.exe> --out <folder>
```

The dependency folder holds `angle/` (libEGL.dll and the rest),
`FirebaseCppApp-9_6_0.dll`, `grpc_csharp_ext.x64.dll` and `licenses/`
([THIRD-PARTY.md](THIRD-PARTY.md)). The server is the file pinned in
`bunker.lock.json`, from Bunker's releases.

`work/` is ignored: keep toolchains, packs and builds there.
