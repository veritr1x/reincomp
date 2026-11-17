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

The player adds their game APK (any name) next to `reincomp.exe` and runs it.
`data/` appears beside it: the prepared APK, the two game libraries, saves,
game storage and the two logs.

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

## Limits

- The server needs the game files already imported and the master data
  patched, as Bunker's phone app does in its own setup. `bunker-server` has no
  import command, so `-assets` must point at a folder prepared another way.
- The game window is fixed at 1280x720.

## Build

```sh
cd launcher
CGO_ENABLED=0 go build -trimpath -ldflags "-s -w -X main.version=<version>" -o reincomp.exe .
```
