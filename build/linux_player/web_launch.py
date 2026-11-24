"""First run and play entry point for the native Linux browser download."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import tempfile

RUNTIME = Path(__file__).resolve().parents[1]
MASTER = "20240404193219.bin.e"
BASE_ASSET_PORT = 8080


def asset_port(value):
    if not value.isascii() or not value.isdecimal() or not BASE_ASSET_PORT <= int(value) <= 65535:
        raise ValueError("Enter a whole asset port from 8080 to 65535.")
    return int(value)


def saved_asset_port(path):
    try:
        prefs = json.loads(path.read_text())
        if not isinstance(prefs, dict):
            raise ValueError(f"Invalid server port settings in {path}")
        return asset_port(str(prefs.get("assetPort", BASE_ASSET_PORT)))
    except FileNotFoundError:
        return BASE_ASSET_PORT


def show_ports(port):
    offset = port - BASE_ASSET_PORT
    print(f"Assets {port} · Game {8003 + offset} · Accounts {3000 + offset}")


def configure_ports(path, value):
    if not value:
        show_ports(saved_asset_port(path))
        if not sys.stdin.isatty():
            return
        value = input("Asset port (8080–65535), 'default' to reset, or Enter to keep: ").strip()
        if not value:
            return
    if value == "default":
        path.unlink(missing_ok=True)
        port = BASE_ASSET_PORT
    else:
        port = asset_port(value)
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=path.parent) as work:
            pending = Path(work) / path.name
            pending.write_text(json.dumps({"assetPort": port}))
            pending.replace(path)
    print("Saved for the next server start.")
    show_ports(port)


def prepare_player(root, state):
    player = state / ("player-" + RUNTIME.name)
    if player.is_dir():
        return player
    from package_assets import stage
    print("Preparing the Linux game data from your APK…", flush=True)
    with tempfile.TemporaryDirectory(dir=state) as tmp:
        work = Path(tmp)
        stage(root / "game.apk", work, (root / "global-metadata.dat").read_bytes(), linux=True)
        data = work / "Reincarnation_Data"
        for src in (RUNTIME / "Reincarnation_Data").rglob("*"):
            if src.is_file():
                dst = data / src.relative_to(RUNTIME / "Reincarnation_Data")
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.unlink(missing_ok=True)
                os.link(src, dst)
        for name in ("Reincarnation.x86_64", "UnityPlayer.so", "GameAssembly.so"):
            os.link(RUNTIME / name, work / name)
        (work / "tools").symlink_to(RUNTIME / "tools", target_is_directory=True)
        shutil.rmtree(work / "data")  # Temporary APK extraction made by stage().
        work.rename(player)
    return player


def has_resources(library):
    base = library / "assets/revisions/0"
    return any((d / "assetbundle").is_dir() and (d / "resources").is_dir()
               and (d / "list.bin").is_file() and ((d / "info.json").is_file() or (base / "info.json").is_file())
               for d in (base, base / "android"))


@contextmanager
def resource_library(root, state, args):
    local = state / "resources"
    saved = state / "asset-library"
    if args.assets:
        external = args.assets.resolve()
    elif not args.dump and saved.is_file():
        external = Path(saved.read_text())
    else:
        external = None
    if external:
        if not has_resources(external):
            raise ValueError(f"Missing a Global asset catalog in {external}. Choose the folder containing assets/revisions.")
        # Use the browser's patched master in our own directory even when the
        # player shares an existing (possibly read-only) imported asset library.
        with tempfile.TemporaryDirectory(dir=state) as tmp:
            view = Path(tmp) / "assets"
            view.mkdir()
            for entry in (external / "assets").iterdir():
                if entry.name != "release":
                    (view / entry.name).symlink_to(entry, target_is_directory=entry.is_dir())
            (view / "release").mkdir()
            shutil.copyfile(root / MASTER, view / "release" / MASTER)
            saved.write_text(str(external))
            yield Path(tmp)
        return
    dump = args.dump
    if not dump and not has_resources(local):
        candidates = sorted(p for p in root.glob("resource_dump*") if p.suffix.lower() in (".7z", ".zip"))
        if len(candidates) != 1:
            raise ValueError("Place resource_dump_android.7z next to Play.sh, or run ./Play.sh --dump '/path/to/resources.zip'.")
        dump = candidates[0]
    if dump:
        subprocess.run([RUNTIME / "bunker-server", "import", "-assets", local, "-dump", dump.resolve(),
                        "-master", root / MASTER], check=True)
        saved.unlink(missing_ok=True)
    if not has_resources(local):
        raise ValueError("The resource dump is missing the Global revision catalog.")
    yield local


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    resources = parser.add_mutually_exclusive_group()
    resources.add_argument("--dump", type=Path, help="import a resource dump (.7z or .zip)")
    resources.add_argument("--assets", type=Path, help="use an existing imported resource directory")
    parser.add_argument("--prepare-only", action="store_true", help="prepare and verify the local installation, then exit")
    parser.add_argument("--server-ports", nargs="?", const="", metavar="PORT|default",
                        help="show or save the local asset port and exit")
    parser.add_argument("unity_args", nargs=argparse.REMAINDER, help="Unity player arguments after --")
    args = parser.parse_args()
    root = Path.cwd()
    state = root / ".reincomp"
    port_file = state / "server-ports.json"
    if args.server_ports is not None:
        configure_ports(port_file, args.server_ports)
        if "REINCOMP_BUNKER_PORT_OFFSET" in os.environ:
            print("REINCOMP_BUNKER_PORT_OFFSET overrides the saved setting while it is set.")
        return 0
    with resource_library(root, state, args) as assets:
        pack = root / "notices.zip"
        if not pack.is_file():
            raise ValueError("Missing notices.zip. Extract the complete browser download before launching.")
        notices = assets / "notices" / "notices.zip"
        notices.parent.mkdir(exist_ok=True)
        if not notices.exists():
            notices.symlink_to(os.path.relpath(pack, notices.parent))
        player = prepare_player(root, state)
        if args.prepare_only:
            print("Linux player and game resources are ready.")
            return 0
        log = state / "server.log"
        override = os.environ.get("REINCOMP_BUNKER_PORT_OFFSET")
        offset = int(override) if override is not None else saved_asset_port(port_file) - BASE_ASSET_PORT
        if not 0 <= offset <= 57455:
            raise ValueError("Invalid Bunker port offset")
        with log.open("w") as errors, subprocess.Popen(
                [RUNTIME / "bunker-server", "-assets", assets, "-data", state / "save",
                 "-port-offset", str(offset), "-stdin"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors) as server:
            try:
                with selectors.DefaultSelector() as ready:
                    ready.register(server.stdout, selectors.EVENT_READ)
                    if not ready.select(90) or not server.stdout.readline():
                        raise RuntimeError(f"Bunker could not start. See {log}")
                game_args = args.unity_args
                if game_args[:1] == ["--"]:
                    game_args = game_args[1:]
                return subprocess.call([player / "Reincarnation.x86_64", "-force-glcore", *game_args],
                                       cwd=player, env={**os.environ, "REINCOMP_BUNKER_PORT_OFFSET": str(offset)})
            finally:
                server.stdin.close()
                try:
                    server.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    server.kill()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
