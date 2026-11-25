# re[in]comp

NieR Re[in]carnation for Windows and Linux. The game's ARM64 code is
recompiled to x86-64 and runs in the desktop Unity player, against the offline
server from [Bunker](https://github.com/veritr1x/bunker).

Unofficial hobby project, built with extensive AI assistance (OpenAI Codex and
Anthropic Claude). Not affiliated with SQUARE ENIX or Applibot.

## How to play

Supply your own copy of the game. Select these two files in the browser:

- the English 3.7.1 (152) arm64-v8a APK
- the master data, `20240404193219.bin.e`

Keep the asset dump (7z or ZIP) for first launch.

Open [Bunker's web builder](https://veritr1x.github.io/bunker/), choose
**Windows**, select your APK and master data, and run the downloaded EXE.
Its launcher imports the assets and starts the game.
If WebView2 is unavailable, use **Choose resource archive** or **Copy extracted
folder** in the setup window. Select **Deploy** when the game files are ready.

For Proton on Linux or Steam Deck, add the Windows download `Nier Reincarnation.exe` to Steam
as a non-Steam game and select Proton under **Properties → Compatibility**.
Keep the shortcut pointed at that same EXE after selecting **Deploy**.
See the [Proton gameplay checks](docs/proton.md) for the tested setup and how
to verify keyboard movement in a field or storybook scene.

To build from source instead, see [BUILDING.md](BUILDING.md).

### Play with RecompLauncher

1. Download `reincomp-<version>-windows-x64.zip` or `reincomp-<version>-linux-x64.tar.gz`
   from the [reincomp release](https://github.com/veritr1x/reincomp/releases/latest).
   Extract it into a writable folder. Linux requires the libraries listed in
   [Native Linux setup](docs/linux-web-builder.md).
2. In RecompLauncher, enable developer tools under **Settings → Developers**.
   Choose **Add a build** and select the extracted folder containing `recomplauncher.json`.
3. In **Easy Setup**, select the Global 3.7.1 ARM64 APK, `20240404193219.bin.e`,
   and your Android resource dump (7z or ZIP). Once setup finishes, select **Play**.

Setup runs locally. RecompLauncher can back up and restore the Bunker save.
Its game settings include frame rate and sound on Windows and window mode on Linux.
On Linux, choose **On launch → Open Bunker** to use the graphical setup window.

### Play the TAR on Linux

On x86-64 Linux, choose **Linux** in [Bunker's web builder](https://veritr1x.github.io/bunker/)
and select the Global 3.7.1 ARM64 APK and `20240404193219.bin.e`. The TAR includes
the game files prepared from those inputs.

Save `NieR-Reincarnation-Linux-x86_64.tar` and your resource dump
`resource_dump_android.7z` in your Downloads folder. Open a terminal and run
these commands for the first launch.

```sh
cd "$HOME/Downloads"
tar -xf NieR-Reincarnation-Linux-x86_64.tar
cd "NieR Reincarnation"
./Play.sh
```

The first launch downloads and verifies the runtime. On a Linux desktop,
Bunker opens in a window. Click **Choose** to select the resource archive or
an imported folder containing `assets/revisions`, then **Deploy**.
To launch directly from a terminal, use `./Play.sh --dump "/path/to/archive"`
or `./Play.sh --assets "/path/to/library"`.

To play again, run these two commands.

```sh
cd "$HOME/Downloads/NieR Reincarnation"
./Play.sh
```

For offline setup and system requirements, see [Native Linux setup](docs/linux-web-builder.md).

### Server ports

The launcher moves the local game, asset and account ports together. Select
an asset port from 8080 to 65535. For example, 11945 uses game port 11868 and
account port 6865. Saved changes apply the next time Lunar Tear starts.

- **Windows and Proton** — Open **Bunker → ⋮ → Server ports**, enter the asset
  port and select **Save**. During play, Ctrl+B opens Bunker. In the native
  setup window used without WebView2, enter the asset port and click **Save ports**.
  To return to port 38080, click **Restore defaults** and save. The launcher
  stores the choice in `data/launcher.json` inside the player installation.
  If a conflict prevents startup, run `"Nier Reincarnation.exe" -launcher`
  to open the settings before starting the server.
- **Native Linux** — Open **Bunker → ⋮ → Server ports**. From the terminal,
  run `./Play.sh --server-ports` or `./Play.sh --server-ports 11945`.
  The choice is saved in `.reincomp/server-ports.json` beside `Play.sh`.
  `./Play.sh --server-ports default` restores asset port 8080.
- **RecompLauncher** — Open the game's settings and choose **Server ports →
  Asset port**. This uses the same saved value as the platform launcher.
- **Bunker for Android and iOS** — In the web builder, choose the mobile
  platform, then **03 Build → Experimental :: Port offset**. Enter the asset
  port minus 8080 (3865 for asset port 11945). The generated APK or IPA uses
  that value. Changing ports on an installed mobile copy requires rebuilding
  the package. The Apple-silicon Mac build uses the same IPA.

## Controls

- Mouse or touch: tap and drag.
- Move with WASD, the arrow keys, the left stick or the D-pad. Hold Left Shift to walk, or toggle walking with Tab or a left-stick click (L3). A gentle stick tilt also gives you fine movement.
- Press R, Xbox X or PlayStation Square to toggle field auto-run when the Auto button is available.
- Aim with the right stick or swipe the DualSense touchpad with one finger. The controller ring fades after one second and hides as soon as you use the mouse.
- Press A, Cross or the DualSense touchpad to tap at the controller ring’s position. Esc, Mouse Back, B or Circle goes back. Space, Y or Triangle skips.
- Alt+Enter toggles fullscreen. Ctrl+B, Xbox View (Select/Back), or PlayStation Options opens the Bunker launcher. Press it again to return to the game.

In reincomp, open **Bunker → Display → Window mode** and choose **Windowed** or **Borderless fullscreen**. Select **Save** to apply it now and use it on your next launch.

## Saves

```
reincomp.exe -export-save backup.zip
reincomp.exe -import-save backup.zip
```

Backups use Bunker's format, so a save moves between phone and PC. Sign in
once on a fresh install before importing.

## Known issues

- The game sometimes freezes during loading. Restart it.
- Audio past the first chapter and in-app purchases are untested.

## Credits

[Bunker](https://github.com/veritr1x/bunker),
[Lunar Tear](https://gitlab.com/walter-sparrow-group/lunar-tear) and
[Lunar Scripts](https://gitlab.com/walter-sparrow-group/lunar-scripts),
[Lunar Base](https://github.com/NMeliksah/lunar-base), the
[master data patcher](https://github.com/NavHobbyDev/lunar-tear-masterdata-patcher),
[Remill](https://github.com/lifting-bits/remill), LLVM, ANGLE and others listed
in [THIRD-PARTY.md](THIRD-PARTY.md). NieR Re[in]carnation is by SQUARE ENIX
and Applibot.

MIT licensed; see [LICENSE](LICENSE). Changes are in [CHANGELOG.md](CHANGELOG.md).
