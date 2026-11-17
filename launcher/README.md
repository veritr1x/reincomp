# The launcher (`reincomp.exe`)

One small Go program. It prepares the player's game APK on first run, starts
Bunker's Lunar Tear server, starts the game, and stops the server when the
game closes.

## Release folder

```
reincomp/
  reincomp.exe
  game/     bunker-game-bootstrap.exe, FirebaseCppApp-9_6_0.dll,
            grpc_csharp_ext.x64.dll, angle/ (libEGL.dll and the rest)
  server/   bunker-server.exe, the file pinned in ../bunker.lock.json
  licenses/
```

The player adds three files of their own next to `reincomp.exe` and runs it:
the game APK (any name), `20240404193219.bin.e` and
`resource_dump_android.7z`. `data/` appears beside it: the prepared APK, the
two game libraries, the imported game files, saves, game storage and the two
logs.

## Importing the game files

When `data/assets` holds no imported files, the launcher runs
`bunker-server import` on the dump and the master data it finds beside itself
and shows the progress. The dump is extracted next to any earlier files and
swapped in only when complete. `-assets <folder>` uses a folder imported
elsewhere instead.

## What "prepare" does

It accepts only the APK this port was made from, by SHA-256: the English
retail build 3.7.1 (152), arm64-v8a, as APKMirror distributes it
(`9ded0797…c9209`, 263,571,029 bytes). It then writes `data/game-sound.apk`
with one one-byte setting changed in `assets/bin/Data/globalgamemanagers`
(Swappy frame pacing off) and extracts `libil2cpp.so` and `libunity.so`. With
`-nosound` it writes `data/game.apk` instead, with audio switched off as well.
Every other APK entry is copied as stored. No code changes.

## Options

- `-assets <folder>`: the imported game files the server serves (default `data/assets`).
- `-fps <n|game>`, `-render <WxH>`: frame rate target and render size.
- `-nosound`: turn the game's sound off.
- `-prepare`: prepare the game files, then stop.
- `-export-save <file.zip>`, `-import-save <file>`: back up or restore the save, then stop.

## Limits

- The master data is installed as supplied. On a phone, Pod Programs then
  adjusts it (the events calendar, language layers); that step is not part of
  this launcher.
- The game window is fixed at 1280x720.

## Build

```sh
cd launcher
CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=<version>" -o reincomp.exe .
```
