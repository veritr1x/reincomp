# Building

The released player builds on Windows. The optional web builder uses
Emscripten to build the compiler that runs in the player's browser. The Linux
section covers the experimental native player and the tested Proton path.

## Build the portable EXE

On Windows x64, install Python 3.12+, Go 1.25.8, Node.js, Microsoft Edge, Git,
MinGW-w64 GCC 15.2 and the complete LLVM 18.1.8 Windows distribution. Use
Unity 2019.4.29f1's x64 non-development IL2CPP player and its matching public
symbols. The build verifies the Unity files, the Global 3.7.1 ARM64 APK and
the original Global master data before compiling.

```powershell
python build/portable.py --apk "game.apk" --master "20240404193219.bin.e" --unity-player "D:\Unity\win64_nondevelopment_il2cpp" --unity-symbols "D:\Unity\UnityPlayer_Win64_il2cpp_x64.pdb" --llvm "D:\LLVM-18.1.8\bin" --out "D:\reincomp-build"
```

The output is `D:\reincomp-build\Nier Reincarnation.exe`. Choose a drive with
space for both the build and the resources you intend to install. Use
`--cxx`, `--go`, `--node` or `--edge` for tools outside their usual locations.
An internet connection is needed for the pinned public Bunker sources,
compiler SDK, vendor dependencies and notices. The build derives the adapter
tables and spring-animation group locally, compiles the native host and
translates the APK in an isolated browser profile. Compiling the ARM64 game
from scratch can take hours. The faster public web builder uses its released
precompiled game runtime.

## Linux

For the Windows build running under Steam Proton, see the
[Proton gameplay checks](docs/proton.md).

The experimental native x86-64 player uses Unity 2019.4.29f1's
`linux64_withgfx_nondevelopment_il2cpp` module, its matching
`UnityPlayer_s.debug`, and a prepared install's `.bunker-player` directory.
Build on x86-64 Linux with Python 3.11+, a C++17 compiler, CMake, GNU
binutils and the recompiler dependencies listed below.

```sh
install="/path/to/.bunker-player"
unity="/path/to/linux64_withgfx_nondevelopment_il2cpp"
python3 recompiler/linux/build_toolchain.py --out build-output/linux-tools
python3 build/linux_player/build_game_assembly.py --lib "$install/data/native/libil2cpp.so" \
    --unity "$unity" --toolchain build-output/linux-tools --out build-output/linux-assembly
python3 build/linux_player/stage.py --install "$install" --unity "$unity" \
    --game-assembly build-output/linux-assembly/GameAssembly.so --out build-output/linux-player
build-output/linux-player/Reincarnation.x86_64 -batchmode -nographics -logFile /dev/stderr
```

The builder compiles the original VM bootstrap to x86-64 ELF. The staging
command converts the startup files to Unity's Linux64 target and installs
the ARM data image beside the managed metadata. Choose a new output directory.
The native player loads the assembly, runs the original static constructors,
registers Unity's internal calls and starts managed runtime initialization.
Native 2D and 3D gameplay are still unverified.

To include the offline server, pass `--server /path/to/bunker-server-1.12.23-linux-x86_64`
to the staging command. It verifies the release checksum. The server can
check its game, asset and account endpoints against a private save on Linux.

```sh
build-output/linux-player/bunker-server -data "/path/to/test-save" \
    -assets "/path/to/imported-assets" -port-offset 32100 -self-test -stdin </dev/null
```

To exercise the native recompiler on four functions from your APK, run the
following from the repository root on x86-64 Linux. Install Git, Clang and
LLVM 18 development files, gflags and glog development headers, and a C++17
compiler. On Ubuntu 24.04 the development packages are `clang-18 llvm-18-dev
libgflags-dev libgoogle-glog-dev g++`. The toolchain build fetches the same
pinned Remill revision and applies the same instruction patches as the Windows
build. Pass `--remill /path/to/local/clone` to reuse a checkout. The test links
the translated objects into a Linux shared library and checks their results
and cooperative polling at runtime.

```sh
python3 recompiler/linux/build_toolchain.py --out build-output/linux-tools
bash recompiler/tests/linux-il2cpp.sh "/path/to/.bunker-player/data/native/libil2cpp.so" \
    build-output/linux-tools build-output/linux-tools/poll-instrument build-output/linux-il2cpp \
    "/path/to/linux64_withgfx_nondevelopment_il2cpp"
```

The optional final argument enables a native engine ABI test using GNU
binutils and the module's matching `UnityPlayer_s.debug`. It recovers the
Linux internal-call addresses, calls Unity's native transform constructor,
and composes the resulting matrices using the original game's translated
`libunity.so` routine. To exercise the exported C functions and internal-call
registration of the actual staged shared library, run:

```sh
python3 recompiler/tests/linux-game-assembly.py build-output/linux-player build-output/linux-il2cpp/engine-calls.json
```

## What to install

| What | For |
| --- | --- |
| Python 3.12 | the build scripts |
| LLVM 18.1.8 for Windows (`clang+llvm-18.1.8-x86_64-pc-windows-msvc`, from LLVM's releases) | compiling the recompiled code; it must be version 18 |
| MinGW-w64 GCC 15 (`g++` on `PATH`) | compiling and linking the game executable |
| Microsoft Visual C++ Redistributable 2015-2022 | the Firebase and gRPC DLLs beside the game need it; most PCs have it |
| Go 1.25.8 or compatible newer version | the launcher, Windows Bunker server and Archive converters |
| Unity 2019.4.29f1 with Windows IL2CPP build support | the x64 non-development Windows player |
| Git and protoc | assembling Bunker's server module and generating its protocol sources for Archive |
| Visual Studio C++ Build Tools, Windows SDK, CMake and Ninja | building the current recompiler from source |

Run the build in a Visual Studio developer prompt. It compiles and caches the
recompiler with this checkout's instruction semantics and instrumentation.
Use `--toolchain` to reuse tools built from the same source.

## From the APK to a folder to play from

```
python build/from_apk.py --apk "game.apk" --master "20240404193219.bin.e" --assets "resource_dump_android.7z" --out "build-output"
```

Use the original Global 3.7.1 build 152 ARM64 APK, the original Global master
data and a 7z or ZIP resource dump. Pass `--unity-player` or `--clang` to
override their locations. The build stages its work under `build-output/work`
and publishes `build-output/reincomp/reincomp.exe` when it succeeds.

1. Verify the APK and master data. Apply the Global patches from the pinned
   Bunker release to the game library, metadata and master data. Import the
   dump with Bunker's server and prepare the synchronized notice pack. Missing
   character, weapon and companion portraits are filled from preserved notice
   art or the matching sprites in the supplied game bundles.
2. **Recompile both libraries** (`recompiler/lift_all.py`). Each library is
   lifted from its list of trace heads in `game/`, in shards of 128, with the
   code patches of `game/patches.json` applied first. About 45 minutes on a
   14-core machine: 4,749 shards.
3. **Collect the pack** (`build/make_pack.py`): the objects, in link order.
4. Compile the embedded Windows host and Unity adapters. Convert the original
   mobile textures for the desktop renderer.
5. Build the launcher, Windows Bunker server and Archive converters. Package
   Pod Programs and apply the original app icon to the player and launcher.

**The Bunker in the game.** For Ctrl+B to show the Bunker's pages, the folder
needs a `pods` folder. The build downloads it, as the pods zip of the 0.1.1
release. To make your own and pass it with `--pods <folder>`:

```
python build/make_pods.py --python <python-3.12-embed-amd64.zip> --bunker <Bunker checkout> --loader <WebView2Loader.dll> --out <folder>
```

The zip is python.org's Windows embeddable package. The checkout is Bunker at
the release in `bunker.lock.json`, after its `scripts/prepare.py`. The DLL is
the x64 one in Microsoft's Microsoft.Web.WebView2 NuGet package. The Python
packages come from PyPI.

`--toolchain`, `--deps`, `--pods` and `--server` accept local release
components. For an offline build also supply `--bunker` with prepared server
sources, `--notices` with the synchronized notice pack, and the UnityPy 1.25.4
dependencies in the supplied Pods package or a local pip wheelhouse.

The deps are what ships beside the game: `angle/` (ANGLE's `libEGL.dll`,
`libGLESv2.dll` and the DLLs they need), `FirebaseCppApp-9_6_0.dll`,
`grpc_csharp_ext.x64.dll` and `licenses/` ([THIRD-PARTY.md](THIRD-PARTY.md)).
They are a zip on this project's 0.1.0 release. ANGLE is built with this
project's patches, which are in `third_party/angle/` with the pins of its
sources; the other two DLLs come from the Firebase C++ SDK 9.6.0 and the
Grpc.Core 2.26.0 package. The importer is Bunker's Windows executable pinned in
`bunker.lock.json`. The playable folder builds the same pinned server with
its release version embedded. Its lazy table reader keeps the 7.8 MB encrypted
master in memory so that Pod Programs can replace the file while the server
is paused. With `--unpinned-server`, the supplied executable is used for both
import and gameplay.

A pack is made from the game's own code. Keep it, and anything built from it,
out of this repository.

### Bunker's Windows web builder

The Windows builder accepts the same original Global APK and master data.
In full compilation mode the browser translates the APK's two ARM64 libraries to Windows x64 objects,
applies the Bunker master-data patch, links the runtime and produces a single
`Nier Reincarnation.exe`. Keep the builder tab open. Completed object groups
are cached in IndexedDB. Reopening the same builder in the same browser
profile and selecting the same inputs resumes compilation. Linking also
needs temporary disk space in the browser's origin-private file system.

To assemble a site, prepare the Emscripten LLVM 18 tools with
`recompiler/web/build-tools.sh` and the MinGW-targeting GNU linker with
`recompiler/web/build-gnu-linker.sh`. Generate the patch plan with
`game_plan.py`, export the native host libraries and linker arguments with
`prepare_link.py`, and package the stock Unity player, Bunker services and
launcher with `prepare_player.py`. These scripts are in `recompiler/web`.

```powershell
python recompiler/web/assemble_kit.py --tools <wasm-tools> --gnu <binutils-work>/wasm/ld --semantics <toolchain>/aarch64.bc --plan <patch-plan> --link <host-libraries> --player <portable-player> --bunker <bunker-checkout> --pyodide <pyodide-0.28.3-with-wheels> --out <empty-kit-directory>
python <bunker-checkout>/web/build_site.py --companion-apk <launcher-apk> --framework <LunarTear.framework> --python <ios-python-bundle> --notices <notice-pack.zip> --windows-kit <kit-directory> --output <site-directory>
```

For local Bunker builds, place the kit in `.build/windows-kit` in the Bunker
checkout. The site builder detects it, checks its Bunker version against the
mobile launcher and verifies each file's SHA-256 before enabling Windows.
For a Bunker release, pass `--archive bunker-windows-browser-<version>.zip`
to `assemble_kit.py`. The Pages workflow accepts that asset from the matching
release tag after verifying the archive's digest.

Serve the static site over HTTPS or localhost for the browser storage and
cryptography APIs. The APK, master data, translated objects and resource dump
stay on the player's device. The downloaded EXE accepts the resource dump
through the Bunker launcher on first run.

#### Precompiled runtime for the public Windows builder

The published Bunker builder reuses the translated runtime from a Windows EXE that
has reached gameplay. Players still select the original APK and master data.
The browser verifies both files, applies Bunker's master-data patch, reads the
APK icons and metadata, checks each runtime download and assembles the EXE.

```powershell
python recompiler/web/prepare_runtime_pack.py --exe "<tested-player.exe>" --exe-sha256 <verified-exe-sha256> --kit <matching-kit-directory>/kit.json --out <new-private-runtime-directory>
```

Use the compiler kit for that EXE. Add
`--windows-runtime <private-runtime-directory>` to the `build_site.py`
command above and choose a separate output directory. Each pack is pinned to
its kit. Regenerate it from a newly tested EXE when changing the game,
translator or native runtime.

For a public deployment, the root of `veritr1x/reincomp-private-archive`
holds the runtime manifest and its numbered parts. Bunker's
`web/windows-release.json` pins the archive commit, manifest SHA-256, and
compiler ZIP SHA-256 for the mobile release. Attach the compiler ZIP to that
Bunker release. The Pages workflow uses the read-only
`WINDOWS_RUNTIME_DEPLOY_KEY` to fetch the selected commit, verifies the
downloaded files, and publishes the translated parts alongside the web
builder. Visitors can download those parts. The APK, master data, save and
resource dump stay with the player.

To time a cold Windows build in Edge, run the following command with a new
run directory under an existing parent. It checks empty browser storage,
disables the HTTP cache, downloads through the Build button, and compares the
file on disk with the supplied digest. Set `BUILD_TEST_MBPS` to cap the total
site transfer rate in megabits per second.

```powershell
node recompiler/web/tests/cold-build.cjs <private-site-directory> <original.apk> <master.bin.e> <new-run-directory> <verified-exe-sha256>
```

### Reusing a profiled Windows pack

With the retained LLVM 18 polled IR for the Unity object containing
`ShaderLab::shaderprops::GetFloat` and `GetVector`, you can rebuild those
lookups and reuse the other objects. The tool checks both lifted functions
against the tested global 3.7.1 build before generating the new pack.

```
python recompiler/tools/optimize_shader_lookups.py --pack <source-pack> --shard-object <shader-object.o> --shard-ir <shader-object.poll.ll> --llvm-bin <LLVM-18-bin> --cxx <g++.exe> --out <optimized-pack>
python build/build.py --pack <optimized-pack> --out <build-folder>
```

The 3D field uses portal traversal and cell finalization during culling. To compile their repeated tail calls into two native loops, pass the polled Unity traversal shard and the object it produced. The output pack can use the shader-optimized pack above as its input.

```
python recompiler/tools/optimize_portal_traversal.py --pack <source-pack> --shard-object <traversal-object.o> --shard-ir <traversal-object.poll.ll> --llvm-bin <LLVM-18-bin> --out <optimized-pack>
python build/build.py --pack <optimized-pack> --out <build-folder>
```

During draw submission, the 3D field prepares shader parameters through three small functions. To inline those calls, use the portal-optimized pack as input and supply the polled Unity parameter shard.

For releases that already use `inline_lifted_helpers.py --inline-stores`, add `--inline-stores` to the shader optimizer command below. This lets LLVM optimize register use around the same 8-, 32-, and 64-bit guest stores before linking. These stores retain the release fast path's existing exclusive-reservation behavior.

```
python recompiler/tools/optimize_shader_parameters.py --pack <portal-optimized-pack> --shard-object <parameter-object.o> --shard-ir <parameter-object.poll.ll> --llvm-bin <LLVM-18-bin> --out <draw-optimized-pack>
python build/build.py --pack <draw-optimized-pack> --out <build-folder>
```

The character spring update can compile together with its collision and vector math routines. Use the patched IL2CPP image and the complete IL2CPP trace-head list from the same pack. Keep the generated directory in place during linking.

```
python recompiler/tools/build_spring_group.py --image <patched-il2cpp-image> --heads <il2cpp-heads> --lifter <remill-lift.exe> --semantics <semantics-folder> --instrumenter <poll-instrumenter.exe> --clang <LLVM-18-clang.exe> --output <spring-folder>
python build/build.py --pack <source-pack> --spring-group <spring-folder> --out <build-folder>
```

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

### Play or measure a saved scene with the native Windows player

To measure the Settings and Profile tab in your own build, choose Best in
the game and run the following from the repository root with a progressed save.

```powershell
.\build\replay_checkpoint.ps1 menu -Install .\build-output\reincomp -Fps 144
```

The report uses PresentMon and the player's frame log to identify focused 4K
frames. It prints the opening and settled frame times separately.
An installed-player replay defaults to 144 FPS. Pass `-Fps 165` to profile
with less headroom between presentations.
See [the scene commands](launcher/README.md#scripted-scene-runs) for save
imports, a moving 3D bridge run and the 2D storybook.

Run either command from the repository root with the prepared Windows player,
asset cache and emulator checkpoints under `.fps-bench`. Both scenes use
3840×2160, the game's highest graphics preset and a 165 FPS render target.
The player opens in a window. Alt+Enter toggles fullscreen.
The menu checkpoint accepts `-MenuSave <game.db>` for a progressed local save.
Use `.\build\replay_checkpoint.ps1 menu -Play -OpenScreen Summon -Render 3840x2160`
to open Summons. Other destinations are Organization, Enhance, Quest, Map,
Library, Menu, Mama and All. A checkpoint uses a private copy of the save.
The prepared bridge can pace completed frames on the graphics thread and
keep the main thread on a measured logical processor. The runner reads the
matching Windows computer name, `field_main_cpu` and `field_present_rate`
from `.fps-bench/native-unity-player/checkpoint-client/frame-pacing.json`.
Those settings take effect in the loaded 3D field. The storybook keeps its
165 FPS target.
Add `-AutoWalk` to the 3D command to activate the game's Auto button once
the field has loaded. The timed walk reports windows where the camera moved
separately from the whole bridge run.

```powershell
.\build\replay_checkpoint.ps1 3d -Play
.\build\replay_checkpoint.ps1 2d -Play
```

Close the game before switching scenes. Add `-RestoreCheckpoint` to start from
a fresh copy of the emulator save. Use `-NativePlayer` in place of `-Play`
to capture a timed foreground run. The script records individual rendered frames and reports the mean, p95,
p99 and count of intervals at or above 8.3 ms. When PresentMon produces a
trace, it also reports Windows presentation intervals.
Add `-RenderTimingOnly` to measure the render thread without starting PresentMon.
Add `-ThreadCpu` to a timed run for a ten-second breakdown of the main
thread's managed callbacks. The report shows elapsed time for each callback
with time in nested managed callbacks subtracted. In the loaded 3D field it
also timestamps callbacks taking at least 3 ms in a separate `.callbacks.tsv`
file. The bridge run saves timings of the individual story continuations
in `.story.tsv`. Both use the Windows performance counter clock so the
timings can be matched to the presentation trace.
Set `REINCOMP_PROFILE_FRAME_CPU=1` for a timed replay to record the main
thread's cycle counts at input dispatch and at the start and end of rendering.
The report compares those counts for main-thread frames above and below the
8.3 ms budget and records the duration of each stop-the-world pause in
`.gc.tsv`. The presentation trace measures when frames reached Windows. For
the bridge it also reports the first five seconds after the loading screen
starts closing, so the entry hitch is visible alongside the gameplay timings.
The native player compiles IL2CPP's liveness walkers together. Bitmap
descriptors visit their reference slots in order, and the marker appends
directly when its existing lists have space. The player uses the sizes from
completed traversals to reserve scratch space before the next world stop.
The learned reserve is capped at 10 MiB. IL2CPP handles further growth and
delivers the callbacks. Set `REINCOMP_REFERENCE_LIVENESS=1` to benchmark
the original walkers and allocation hints. The following check compares 512
descriptors and the ordered batches for 6,001 live objects, including
duplicate references and a cycle. Run it with the game closed.

```powershell
$env:REINCOMP_CHECK_LIVENESS = '1'
try {
    Start-Process .\build-output\reincomp\reincomp.exe -Wait
    Get-Content .\build-output\reincomp\game\liveness-check.txt
}
finally { Remove-Item Env:REINCOMP_CHECK_LIVENESS }
```

The field checkpoint prepares IL2CPP type metadata for three quest progress
row types during loading. The game then applies the real story update when it
arrives. Set `REINCOMP_PROFILE_STORY_WORK=1` for individual timings and a
record of which types were prepared. Set `REINCOMP_SAMPLE_STORY_STACKS=1`
to also sample the main-thread stacks around the bridge crossing.
In the loaded field, Octo starts its next queued asset on a later
frame once admitting requests has taken at least 0.75 ms. A request already
started can take longer. The timing run records these decisions in `.loads.tsv`
when `REINCOMP_PROFILE_STORY_WORK` is set.
The native build also skips construction of an unused diagnostic string in
`FollowPathController.StartFollowPath`. The game calculates the route, checks
its corners and updates the actor through the original navigation code.
The story timing trace includes the elapsed time of each path start.

To rebuild the native DLLs for the portable player, use the
`build/portable.py` command above with a new output directory. To replay a
checkpoint in that build, pass its EXE and a Bunker save from the desired
scene. For example, this command loads a saved 3D field in fullscreen.

```powershell
.\build\replay_checkpoint.ps1 3d -Install 'D:\reincomp-build\Nier Reincarnation.exe' -SaveArchive 'D:\saves\bridge.zip' -Fullscreen -Play
```

Use `2d` and a storybook save for a 2D scene. Importing a checkpoint replaces
the test installation's active save through Bunker's save importer.

For an existing converted Octo cache, run the prepared
`warm_desktop_bundles.py --index-only` with its recorded Python interpreter.
This verifies the local catalog files once. The player checks their file
identity, length, modification time and mask before reusing the matching
Windows bundle. Updated or unindexed files go through normal stream conversion.

## Checking release artwork

Use the build's Python environment with UnityPy and Pillow to compare the
textures shipped in the APK and the sprites listed in a Bunker artwork
manifest. Supply additional `--art-manifest` arguments for the character,
weapon and inventory catalogs. `--verify-only` checks uncached sprites
in memory and leaves the install untouched. Set `BUNKER_SOURCE_ROOT` if
your Bunker checkout is outside the default sibling directory.

```powershell
python build/native_player/prepare-screen-art.py build-output/reincomp --verify-only --packaged --art-manifest path/to/native-summon-ui/manifest.json
```

## Checking a change without the game

`python build/build.py --compile-only --out <folder>` compiles the whole host
layer and stops. It needs no pack; CI runs it on every push.

`work/` is ignored: keep toolchains, packs and builds there.
