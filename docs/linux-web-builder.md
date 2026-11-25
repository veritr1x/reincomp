# Native Linux web build

Select **Linux** in Bunker's web builder and supply the original Global 3.7.1
ARM64 APK and `20240404193219.bin.e`. The browser checks both files, patches
the master and metadata locally and produces `NieR-Reincarnation-Linux-x86_64.tar`.

Extract the TAR on an x86-64 Linux filesystem with executable permissions.
Run `./Play.sh` in the extracted folder. On a desktop, Bunker opens its
setup window. Click **Choose** and select a resource archive (.zip or .7z)
or the imported folder containing `assets/revisions`. Click **Deploy** to
prepare the files and start Unity. Later launches remember the library.

The window uses the system Python 3, GTK 3 and WebKitGTK 4.1 (WebKit 2.40
or newer). On Ubuntu 24.04, install its dependencies with
`sudo apt install python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1`.
Use `./Play.sh --launcher` to explicitly request the window, including
when supplying Unity arguments. `./Play.sh --no-launcher` starts directly.
Headless sessions and desktops without the GUI dependencies use the same
terminal setup as before. Use `--dump "/path/to/archive"` or
`--assets "/path/to/library"` for the first direct launch.

The first launch downloads a versioned native runtime from the reincomp
release and checks its SHA-256. The archive includes the native Unity
player, the translated x86-64 game code in `GameAssembly.so`, the Bunker
server and the Python asset converter. For offline setup, place
the matching `reincomp-linux-x86_64-<version>.tar.gz` release asset beside
`Play.sh` before running it. Keep the installation in a writable folder.
The local server stores its save in `.reincomp/save`.

The asset server listens on port 8080 by default. In Bunker, open
**⋮ → Server ports**, enter an asset port and click **Save**. Run `./Play.sh --server-ports`
to see the ports and choose a new asset port. You can also save a choice directly
with `./Play.sh --server-ports 11945`. The next launch uses port 11945 for
assets, 11868 for the game and 6865 for accounts. Use
`./Play.sh --server-ports default` to restore the defaults.
`REINCOMP_BUNKER_PORT_OFFSET` overrides the saved setting for that launch. To request
a 1280 × 720 window, run
`./Play.sh -- -screen-width 1280 -screen-height 720 -screen-fullscreen 0`.
The native launcher currently uses the game's frame-rate setting.

The tested configuration is Ubuntu 24.04 x86-64 with OpenGL, OpenSSL 3 and
`libsecret-1-0`. Story movies use FFmpeg with the VP8 and Vorbis encoders
(`sudo apt install ffmpeg` on Ubuntu). The first play converts each movie
for Unity and saves it in `reincomp-movies-v1` under the game's Unity data
directory. Later plays use that copy. The launcher uses Bash, curl, flock,
tar and sha256sum.

## Preparing a release

On Linux, use the full gameplay-tested stage, the server pinned in
`bunker.lock.json` and a relocatable python-build-standalone CPython 3.12.14
install. Use its Python to build the converter wheels. The Python running
`web_runtime.py` needs Jinja2 to render the shared launcher page during packaging.

```sh
"$python_prefix/bin/python3" -m pip wheel -r build/linux_player/converter-requirements.txt -w "$wheels"
python3 build/linux_player/web_runtime.py --stage "$tested_player" \
    --assembly-sha256 "$tested_game_assembly_sha256" --server "$pinned_bunker_server" \
    --python-prefix "$python_prefix" --wheels "$wheels" --bunker "$bunker_checkout" \
    --remill-license "$toolchain/LICENSE.remill.txt" --out "$artifacts"
```

Copy the generated `linux-release.json` to Bunker's `web/`. Build a local
Pages site using its normal inputs and the additional
`--linux-release web/linux-release.json` argument. Test it with a fresh
browser profile.

```sh
node recompiler/web/tests/cold-build.cjs "$site" "$apk" "$master" "$new_run_dir" --linux
```

Extract the browser download and run its launcher with the candidate
runtime archive beside it. Publish the **runtime archive** as an asset on
the matching reincomp release. The browser download contains the player's
own game files and stays local. Bunker's Pages workflow enables the Linux
option when the public runtime name, size and SHA-256 match the pin and the
desktop kit matches both release versions.
