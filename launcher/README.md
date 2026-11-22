# The launcher (`reincomp.exe`)

The Go launcher starts the packaged Windows player, Bunker's Lunar Tear
server and Pod Programs. Closing the game stops the local services.

## Release folder

```
reincomp/
  reincomp.exe
  game/     WindowsPlayer.exe, UnityPlayer.dll, GameAssembly.dll,
            GuestRuntime.dll, EngineCalls.dll, WindowsPlayer_Data/ and runtime dependencies
  server/   bunker-server.exe, the file pinned in ../bunker.lock.json
  pods/     embedded Python, Bunker's pages and WebView2Loader.dll
  data/     APK-derived game files, original master data, imported assets and saves
  licenses/
```

Supply the APK, master data and asset archive to `build/from_apk.py`. Run
the resulting `reincomp.exe`. Use `-render 3840x2160 -fullscreen` to start
fullscreen at 4K. Alt+Enter toggles the mode while playing.

## Importing the game files

The launcher looks for an installed revision and master data beside the player.
Use `-assets <folder>` to point it at an existing Bunker installation elsewhere.
When resources are missing, Windows opens Bunker's System, Play and Programs
screen. Select **Choose** under **Game files** to copy an extracted Global
Android folder, use it in place, open a `.7z` or `.zip`, or enter an HTTP or
HTTPS link to the archive. Using a local folder in place saves disk space.
Keep that folder on its drive while playing. The launcher can also pick up
a resource archive placed beside the EXE. Once the files pass verification,
**Deploy** opens the game in the same window. Use **Change** to select new
resources from a launcher that already has a verified installation.

A download from Bunker's Windows web builder is named `Nier Reincarnation.exe`.
Place it in a writable folder. On first launch it verifies and expands its
contents into `.bunker-player` there and prepares the game assets from the APK.
The resource archive can sit beside the downloaded EXE.

With resources installed and auto-deploy on, the EXE starts the game directly.
With auto-deploy off, it opens the launcher and waits for Deploy. Use
`"Nier Reincarnation.exe" -launcher` to open it explicitly. The Pod Programs
row opens the shared dashboard with Display, Summon Rates and the save editors.
The Archive has its own row on the launcher.
Options includes save import and export, master-data import, auto-deploy,
theme, server status and the log. During play, press **Ctrl+B → Options → Stop
Lunar Tear**. Confirm to close the game and return to the launcher with the
server stopped. Under **Options → Import save backup**, select your Bunker
backup ZIP or the emulator's `game.db`. Select **Deploy** to resume with the
imported save. The previous save is kept in `.bunker-player/data/server.before-import`.

In the portable Windows player, **Options → Check for updates** offers
**Update and restart** when a newer Windows release is available. Enable
**Install updates automatically** to check and install before gameplay.
The updater rebuilds the release with the original APK and master data already
in the installation, verifies the executable against Bunker's published SHA-256,
then restarts it in place. Imported resources, saves and settings stay in their
existing folders. An offline startup can still enter the game.

## The Bunker in the game (Ctrl+B)

With a `pods/` folder, the launcher starts the server with its control
endpoint (`-control`, a random token) and starts Bunker's Pod Programs beside
it with the Python in `pods/python`. The game does not wait for it. When the
pages are served, the launcher writes their address and token to
`data/bunker-view.json`. Ctrl+B in the Windows player opens the Bunker launcher
over the running game. Xbox View (Select/Back) and PlayStation Options also
toggle the launcher. Choose Pod Programs or Archive there. Press the shortcut
again or select **Back to the game** to resume. Pod Programs keeps its own files in
`data/pods`, its log is `data/pods.log`, and the pane's browser data is in
`data/webview`. It needs the Microsoft Edge WebView2 Runtime, which Windows
11 has.

## APK preparation in older launcher-only installations

It accepts only the APK this port was made from, by SHA-256: the English
retail build 3.7.1 (152), arm64-v8a, as APKMirror distributes it
(`9ded0797…c9209`, 263,571,029 bytes). It then writes `data/game-sound.apk`
with one one-byte setting changed in `assets/bin/Data/globalgamemanagers`
(Swappy frame pacing off) and extracts `libil2cpp.so` and `libunity.so`. With
`-nosound` it writes `data/game.apk` instead, with audio switched off as well.
Every other APK entry is copied as stored. No code changes.

## Options

- `-assets <folder>`: the imported game files the server serves (default `data/assets`).
- `-fps <n|game>` and `-render <WxH>` override the saved Bunker Display frame rate and resolution. A fresh Windows installation starts with a 144 FPS target in a 1600×900 window. Display applies texture size, shadows, reflections, bloom and idle drawing on the next launch. With idle drawing enabled, Bunker reduces drawing to about 30 FPS after two seconds without touch outside battles. Turn it off to keep the selected frame rate in still menus.
- `-fullscreen` starts the Windows player in borderless fullscreen. Alt+Enter toggles it during play. In windowed mode, drag the border to resize.
- `-prepare`: prepare the game files, then stop.
- `-export-save <file.zip>`, `-import-save <file>`: back up or restore the save, then stop.

The single-command build applies the Bunker master-data patches and keeps the
supplied original in `data/original-master.bin.e` for the Pod Programs editors.

## Build

```sh
cd launcher
CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=<version>" -o reincomp.exe .
```

## Scripted scene runs

Run these commands from the repository root with a built installation.
Register the local player once, choose **Best** in the game’s graphics settings
and use a save that has reached the desired scene. For a save containing
multiple players, Ctrl+B → Pod Programs → Players lets you choose the character.

```powershell
.\build\replay_checkpoint.ps1 menu -Install .\build-output\reincomp
.\build\replay_checkpoint.ps1 menu -Install .\build-output\reincomp -Play -OpenScreen Summon
.\build\replay_checkpoint.ps1 menu -Install .\build-output\reincomp -Play -OpenScreen SummonRates
.\build\replay_checkpoint.ps1 menu -Install .\build-output\reincomp -Play -OpenScreen SummonNotice
.\build\replay_checkpoint.ps1 menu -Install .\build-output\reincomp -Play -OpenScreen All
.\build\replay_checkpoint.ps1 3d -Install .\build-output\reincomp -SaveArchive .\field.zip -AutoWalk
.\build\replay_checkpoint.ps1 2d -Install .\build-output\reincomp -SaveArchive .\storybook.zip -Play
```

`SummonRates` opens the selected banner’s Rates page. `SummonNotice` opens the
banner’s information button. For Mama’s Exciting Festival, the notice links
to the Summons Details page with the cost, featured items and item pool.
Both replays wait for the embedded page to finish loading.
The timed 3D replay expects the bridge (`tw002001`). For a save in Fio’s
Chambers of Dusk, add `-FieldScene bg007001` to measure the loaded chamber.
The report checks the active scene, the 3D actor, graphics settings and focus.
`All` visits Summons, Loadout, Enhance, Quests, Map, Library and Menu in order.
To capture each tab, set `REINCOMP_SCREEN_CAPTURE_FILE` to an absolute PNG path
in an existing directory before launching `All`. For example, a path ending in
`reference.png` produces `reference-Gacha.png` through `reference-Menu.png`.
Clear the environment variable after the capture.
With matching English 16:9 Android captures named `android-Gacha.png` through
`android-Menu.png` at 1920×1080, run
`python build/native_player/compare_menu_headers.py captures/android captures/reference`.
It checks the Summons gem controls, Bunker and Notices buttons, and the counter
divider on each other tab. It reports horizontal offsets in 1080p reference pixels.
Use a Python environment with Pillow and NumPy. Each control must have a
zero-pixel offset and an edge correlation of at least 0.8 for the check to pass.

`-SaveArchive` uses the launcher's Bunker save importer. Omit it to continue
the installed player’s current save. To reproduce device-local UI settings
and notification history, add `-PlayerPrefs path\to\emulator-playerprefs.xml`
with the game's Unity Android PlayerPrefs XML. Each replay uses a temporary
copy, including the emulator's language and saved registration. When the backup
has multiple characters, let the replay sign in once. Use **Ctrl+B → Pod Programs → Players →
Use this player** to select the character for that registration, then restart
the replay with the same XML. On a fresh install, confirm the game's resource
download when it first opens the chapter. Bunker serves those files from the
imported asset dump and the game caches them for later runs.
Select **Best** in the source preferences for a Highest graphics comparison.
`-Play` leaves the game open. Timed runs
record presentations from the game process at 3840×2160. Keep the game focused.
The menu report identifies the Settings and Profile tab and measures the opening
five seconds and settled play separately. Results go in `data/benchmarks`.
Set `REINCOMP_PRESENTMON` to the PresentMon executable if it is outside the
usual installation directory or PATH.

For the prepared developer fixtures under `.fps-bench`, omit `-Install` and use
`-RestoreCheckpoint` to reload the appropriate fixture. Add `-ThreadCpu` for
managed callback timings, or `-CompareMenuPointer` to compare the original and
optimized pointer handling on the Settings and Profile tab.

### Earlier emulator-host installations

From the repository root, select a scene in a prepared installation built with this host.

```powershell
.\build\reach_scene.ps1 -Install C:\Games\reincomp -Scene title
.\build\reach_scene.ps1 -Install C:\Games\reincomp -Scene field -Restart
.\build\reach_scene.ps1 -Install C:\Games\storybook -Scene storybook -PreviousInstall C:\Games\reincomp -Restart
```

To measure the scene currently on screen, run `.\build\measure_fps.ps1 -Install C:\Games\reincomp`. It records six full five-second windows of FPS and frame times while you play. Each window reports the number of intervals that took at least 8.30 ms (`at_or_above_8_30ms`) and the number longer than 1/120 second (`over_120hz_budget`), each divided by the number measured. The counts use the original intervals before rounding. Set `-SampleWindows` to change the duration.

Add `-ThreadCpu` to a measurement or checkpoint replay to record approximate UnityMain CPU milliseconds per presented frame. Each window also reports the percentage of elapsed time that Windows scheduled that thread. The game records FPS at successful window swaps.

The title route waits for Tap to Start. The field and storybook routes tap it and resume the current save. Use a matching 3D field or 2D picture-book checkpoint. After loading, the script checks the active actor's picture-book flag. Complete any first-time prompts that appear. Each run saves a PNG and three five-second windows of presentation FPS and frame times under the installation's data folder. With `-Restart`, use `-PreviousInstall` to switch between separate saved games that use the same local server ports.

Use `-GuestCpuCount 8` to benchmark Unity with eight guest CPUs. This setting applies to the launched process and controls the topology used to size Unity's worker pool. The host normally reports up to 16 logical processors.

Field and storybook runs can select the game's Low or Highest graphics level with `-GraphicsPreset low` or `-GraphicsPreset highest`. The storybook route waits 45 seconds for the prologue's opening quotation to clear before measuring the side-scrolling scene. Omit the preset to benchmark the saved graphics setting.

For an isolated copy of an emulator save, pass the same `-Profile <name>` on each run. This selects that benchmark's Android preferences and device ID. The installation's `data/server` directory holds quest progress. After the Windows client has registered once, use the launcher's `-import-save` command to transfer the emulator player to that client.
