# Windows web builder

Choose **Windows** on [Bunker's web builder](https://veritr1x.github.io/bunker/).
Select the original Global 3.7.1 ARM64 APK and `20240404193219.bin.e`, then
click **Build**. The browser verifies both files and assembles
`Nier Reincarnation.exe` locally. Open the downloaded EXE and select the
resource dump in Bunker's launcher.

## Publishing a tested runtime

The version-matched public compiler and player kit is a ZIP asset on the
Bunker release. The translated runtime comes from an EXE verified in gameplay
with that kit. To extract its compressed parts and installation checksums, run
the following command with Python 3.11 or later.

```powershell
python recompiler/web/prepare_runtime_pack.py --exe "<verified-player.exe>" --exe-sha256 <sha256> --kit <kit-directory>/kit.json --out <new-runtime-directory>
```

Place only the generated `runtime.json` and `runtime.partNNN` files at the
root of `veritr1x/reincomp-private-archive`. Record its commit, the manifest
SHA-256 and the compiler ZIP SHA-256 in Bunker's
[`web/windows-release.json`](https://github.com/veritr1x/bunker/blob/main/web/windows-release.json).
The Pages workflow reads the selected revision with a read-only deploy key.
It verifies the pack against the matching kit and serves the selected runtime
parts publicly. Player APKs, master data, saves and resource dumps stay on
the player's device.

## Testing a first build

On Windows, run the following command using Node.js 22 or later and Microsoft
Edge. Use a new run directory under an existing parent. The test creates a
fresh Edge profile, verifies empty browser storage, disables the HTTP cache,
clicks Build, and verifies the downloaded EXE against the reference SHA-256.
Timing and output details are saved in `result.json` in the run directory.

```powershell
node recompiler/web/tests/cold-build.cjs https://veritr1x.github.io/bunker/ <original.apk> <master.bin.e> <new-run-directory> <verified-exe-sha256>
```

For an unpublished site, supply its local directory as the first argument.
The local test also checks that a corrupted runtime part is rejected.
