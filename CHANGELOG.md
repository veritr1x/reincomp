# Changelog

What changed, newest first, by date and area. A release's heading comes
above the dated sections it holds.

## 0.2.6 (2026-11-23)

- **Check for updates** now checks reincomp releases. **About** shows the Windows player version and the bundled Bunker version separately. The updater verifies the rebuilt player against the checksum in the tagged reincomp release.
- The right stick and DualSense touchpad move a visible cursor that fades when idle. Press the touchpad, Cross or Xbox A to tap at the cursor. Moving the mouse returns control to the system pointer.
- Bunker's Windows display settings include borderless fullscreen and restore the previous window size when returning to windowed mode.
- Windows builds include Bunker 1.12.21.

## 0.2.5 (2026-11-23)

- Fixed a loading stall at 40% when Windows temporarily locks Bunker's catalog. The asset converter now keeps running if Pod Programs fails to start. A Windows test with an empty converter cache reached 100% and resumed the saved story scene.
- Reduced the time to convert the first 800 prologue bundles from 2.57 to 2.03 seconds by preserving the original data in text-only bundles.
- Fixed a Proton startup failure when Unity took an address range needed by the game runtime. The portable EXE can stay selected as the Steam shortcut after deployment.
- Restored sound in cutscenes that have an audio track. Movie playback respects the launcher's mute setting.
- Kept picturebook story text inside the illustrated area on ultrawide screens.

To build the fixed Windows player with your own game files, follow the [portable build instructions](https://github.com/veritr1x/reincomp/blob/v0.2.5/BUILDING.md#build-the-portable-exe). The release downloads are the standalone launcher and the Windows recompiler toolchain.

## 0.2.4 (2026-11-23)

- Chapter 1 weapon memories now play their opening movie and continue into dialogue and movement. The story also advances when a video decoder is unavailable.
- The Windows launcher shows native setup controls when WebView2 is unavailable. First-run asset preparation works on older Wine versions.
- During play, **Ctrl+B → Options → Stop Lunar Tear** closes the game and server, then returns to the launcher. You can import a Bunker backup ZIP or an emulator's `game.db` and deploy again.
- Windows builds use Bunker 1.12.19.

## 0.2.3 (2026-11-22)

- Deck Builder now displays character portraits and equipment images in the Windows player.
- The Archive has a visible Close button in narrow and wide windows. Use it to return to the launcher.
- Windows builds now use Bunker 1.12.18.

## 0.2.2 (2026-11-22)

- The Windows game opens at its saved window size and fits the game view on the first frame. Deploy also handles installation folders with spaces.
- WASD moves the character in 2D and 3D scenes. The right stick moves the cursor, and Xbox A or PlayStation Cross taps at its position. Space, Xbox Y, or PlayStation Triangle activates Skip when the scene offers it.
- Ctrl+B, Xbox View (Select/Back), and PlayStation Options open the Bunker launcher. Press again to return to the game. Pod Programs keeps its Close button visible across desktop window sizes.
- Automatic Windows updates now finish when Edge hands the build to a child process. The updater also closes its private browser processes after the build.

## 0.2.1 (2026-11-21)

- The portable Windows player can update and restart from Bunker’s launcher.
  Enable automatic installation in Options, or use **Check for updates**.
  The updater reuses the installed APK and master data, verifies the rebuilt
  EXE, and keeps the existing saves, resources and settings.
- Fixed taps and clicks after resizing the game window. The embedded Unity
  player now recognizes its parent window’s focus and keeps each tap in the
  resized window’s coordinate system.
- Added space above **Close** in the game files dialog.

## 0.2.0 (2026-11-21)

- Fixed a Windows summon freeze between the second and third rewards of a ten-pull.

- Build the portable Windows player from the original Global APK and April
  2024 master data with `python build/portable.py`. The build regenerates the
  adapter tables from the APK and Unity 2019.4.29f1 player and symbols.
- Bunker's Windows launcher offers the four Android resource choices. Copy
  the extracted folder, use it in place, pick a 7z or ZIP, or paste an archive
  link. The launcher verifies the complete Android revision before Deploy.
- The Windows implementation includes Bunker's in-game pages, display
  settings, FSR 3.1, touch gestures, keyboard movement and controller input.

## 0.1.1 (2026-11-18)

Ctrl+B in the game opens the Bunker (Pod Programs) in a pane over the game.
The build downloads what serves its pages along with the rest, so a folder
built from your APK has it without further steps. It needs the Microsoft Edge
WebView2 Runtime, which Windows 11 has. Little of the Bunker is tested on a
PC yet.

## 2026-11-18 (after 0.1.0)

### The Bunker in the game
- New: Ctrl+B in the game window opens the Bunker's pages (Pod Programs) in
  a pane over the game, and closes it again; the pages' own Close closes it
  too. The game keeps running underneath. The pane is WebView2
  (`host/bunker_view.cpp`), loaded through `WebView2Loader.dll`; without the
  WebView2 Runtime, Ctrl+B does nothing and the game log says why.
- New: with a `pods/` folder beside it, the launcher starts the server with
  its control endpoint and Bunker's Pod Programs beside it, and tells the
  game where the pages are (`data/bunker-view.json`). The game does not wait
  for them.
- New: `build/make_pods.py` makes that folder: Python 3.12, the packages
  Pod Programs needs, Bunker's pages at the pinned release, and
  `WebView2Loader.dll`, with their license texts. `make_release.py` takes
  it with `--pods`.
- Checked in the game at the title screen: the pane opens and closes with
  Ctrl+B from the game and from inside the pane, the Pod Programs home page
  shows, and a backup made through the pages was written. Not checked:
  clicking through the pages in the pane, the pages' Close, and a save edit
  while playing.

- The build downloads that folder too, as the pods zip of the 0.1.1 release
  pinned in `release.lock.json`; `--pods` takes your own instead. The zip was
  checked against the released 1.12.13 server: its pages are served, and Deck
  Builder refuses a save nobody has played with a message.
- The pinned server is Bunker 1.12.13, whose Deck Builder no longer restarts
  the server over and over on a save nobody has played yet.

## 0.1.0 (2026-11-18)

The first release. It holds the launcher, `reincomp.exe`, and the recompiler's
toolchain for Windows (the lifter, its semantics and the instrumenter), and
nothing made from the game: the game executable is built on your PC from your
own APK with `python build/from_apk.py`. See README.md for what you need and
BUILDING.md for the steps. Windows x86-64 only; the game plays with sound at
about 35 to 45 fps, and can freeze during a loading pause.

## 2026-11-18

### Build from your own APK
- The build needs only the APK and clang: `python build/from_apk.py --apk
  <APK> --clang <clang.exe> --out <folder>`. It downloads the toolchain, the
  launcher and the files that ship beside the game from this project's
  release, and the server from Bunker's, each checked against a pinned hash
  (`release.lock.json`, `bunker.lock.json`). `--no-download` turns that off.
- New on the 0.1.0 release: the deps zip, with ANGLE, the Firebase and gRPC
  DLLs and the license texts of everything in them (gRPC and what it links,
  the MinGW-w64 and GCC runtimes, Firebase's third-party notices).
- New: `third_party/angle/` holds the three patches ANGLE is built with, the
  pins of its sources and the build's configuration.
- New: `python build/from_apk.py` goes from the game APK to a folder to play
  from, in one command. It recompiles the game's two libraries on your PC,
  builds the executable and assembles the folder.
- New: the pack of recompiled code is regenerated, not obtained. `game/` holds
  what describes the game without holding any of its code: where each
  recompiled function starts (about 608,000 addresses), three headers and the
  code patches. `recompiler/lift_all.py` lifts a whole library from them.
- New: the recompiler runs natively on Windows. No Linux or WSL is needed.
  `recompiler/windows/build_toolchain.py` builds the lifter (AArch64 only),
  its semantics and the instrumenter with LLVM 18 and MSVC.
- A pack is now only objects. The trace table is written from the address
  lists at build time, and the Remill, zlib and ANGLE headers are in
  `third_party/`.
- `build.py --compile-only` compiles the host layer with no pack.
- The toolchain builds with a Visual Studio newer than LLVM 18 (2022's current
  compiler tools), which otherwise refuse clang 18 in their headers.
- The pinned Remill revision is the one the working build was lifted with
  (3fd14f3, plus the indexed-fmul patch).

### Game
- In-app purchases: the four code patches the phone build applies to the
  purchase flow are applied before the code is recompiled
  (`game/patches.json`). Not tried in the shop yet.

### Launcher
- New: the first start imports the resource dump and the master data when
  they are next to `reincomp.exe`, with progress. It uses Bunker's own import
  (`bunker-server import`).
- New: `-export-save` and `-import-save` back up and restore the save, in
  Bunker's backup format: a save moves between a phone and the PC either way.
- The pinned server is Bunker 1.12.12, so a released server now works without
  `--unpinned-server`. 1.12.11 was the first with the import and save
  commands, but refused a save made by another version; 1.12.12 snapshots the
  save and updates it.

### CI
- New: every push compiles the host layer, builds the launcher and parses the
  Python tools. A tagged release builds the Windows toolchain and the launcher
  and attaches them.

### Sound
- New: the game has sound. Unity's audio (FMOD) plays through a native version
  of its Android audio device class, to the Windows sound device, at 44.1 kHz
  stereo. With no playback device the game runs on silently.
- The launcher plays sound by default; `-nosound` turns it off, as before.
- The JNI layer supports direct byte buffers, and threads the host starts can
  attach to the Java VM.
- An OpenSL ES provider (engine, output mix, buffer-queue players) is included
  for builds of the game that choose that output; this game does not use it.
- Added the libc and Android pieces the audio code asks for: `setpriority`,
  `strerror_r`, `__system_property_get` and Unity's phone-call listener.

### Recompiler
- New: `recompiler/tools/find_code_pointers.py` lists code reached only through
  pointers or indirect branches, and `find_resume_points.py` lists where
  execution resumes after an instruction the runtime emulates. Both write seeds
  for `lift_new_traces.py`.

### Build
- New: `build/add_traces.py` adds newly lifted functions to a pack, and
  `build/remove_traces.py` takes them out again.
- `build.py` rebuilds a file when its compiler command changes, not only when
  an included file does. Before, switching packs could leave stale objects.

## 2026-11-17

### Repository
- First commit of re[in]comp as its own repository: the recompiler, the
  runtime, the Windows host layer, the build and the launcher.

- The license names this project's own copyright holder. The first commit carried
  Lunar Tear's line by mistake; no Lunar Tear code is in this repository.

### Build
- New: `python build/build.py --pack <pack> --out <folder>` builds the game
  executable in one command. It compiles the host layer, links it with a pack
  of recompiled game objects and post-processes the result. Later builds
  recompile only the files that changed.
- New: `build/make_pack.py` collects a pack from an existing build, and
  `build/make_release.py` assembles the folder players unzip.

### Launcher
- New: `reincomp.exe` prepares the player's game APK on first start, starts
  the Lunar Tear server, starts the game, and stops the server when the game
  closes.
- The prepared APK is the same bytes on every machine, and the game accepts it.

### Game
- The game runs against a local Lunar Tear server, at up to 4K, with keyboard
  input.
- Known: about 40 fps in gameplay, long loading screens, a fixed 1280×720
  window.
