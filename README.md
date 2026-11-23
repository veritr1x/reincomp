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
./Play.sh --dump "$HOME/Downloads/resource_dump_android.7z"
```

The first launch downloads and verifies the runtime, imports the resources,
and starts the game. If you already have an imported library, use
`./Play.sh --assets "/path/to/library"` for the last command. Choose the
folder containing `assets/revisions`.

To play again, run these two commands.

```sh
cd "$HOME/Downloads/NieR Reincarnation"
./Play.sh
```

For offline setup and system requirements, see [Native Linux setup](docs/linux-web-builder.md).

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
