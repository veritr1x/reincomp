# re[in]comp

NieR Re[in]carnation as a native Windows program. The game's ARM64 code is
recompiled to x86-64 and runs in the Windows Unity player, against the offline
server from [Bunker](https://github.com/veritr1x/bunker).

Unofficial hobby project, built with extensive AI assistance (OpenAI Codex and
Anthropic Claude). Not affiliated with SQUARE ENIX or Applibot.

## How to play

You need your own copy of the game. None of it is in this repository or its
releases. The EXE is built on your PC from these files:

- the English 3.7.1 (152) arm64-v8a APK
- the master data, `20240404193219.bin.e`
- the asset dump (7z or ZIP)

Open [Bunker's web builder](https://veritr1x.github.io/bunker/), choose
**Windows**, select your APK and master data, and run the downloaded EXE.
Its launcher imports the assets and starts the game.
If WebView2 is unavailable, use **Choose resource archive** or **Copy extracted
folder** in the setup window. Select **Deploy** when the game files are ready.

On Linux or Steam Deck, add the downloaded `Nier Reincarnation.exe` to Steam
as a non-Steam game and select Proton under **Properties → Compatibility**.
Keep the shortcut pointed at that same EXE after selecting **Deploy**.

To build from source instead, see [BUILDING.md](BUILDING.md).

## Controls

- Mouse or touch: tap and drag.
- Move with WASD, the left stick or the D-pad. Aim with the right stick or swipe the DualSense touchpad with one finger. The controller ring fades after one second and hides as soon as you use the mouse.
- Press A, Cross or the DualSense touchpad to tap at the controller ring’s position. B or Circle goes back. Space, Y or Triangle skips.
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
