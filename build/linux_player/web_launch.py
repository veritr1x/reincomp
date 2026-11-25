"""First run and play entry point for the native Linux browser download."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
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


def save_asset_port(path, value):
    port = asset_port(str(value))
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=path.parent) as work:
        pending = Path(work) / path.name
        pending.write_text(json.dumps({"assetPort": port}))
        pending.replace(path)
    return port


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
        port = save_asset_port(path, value)
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


def resource_source(root, state, assets=None, dump=None):
    if assets:
        return "assets", assets.resolve()
    if dump:
        return "dump", dump.resolve()
    saved = state / "asset-library"
    if saved.is_file():
        return "assets", Path(saved.read_text())
    if has_resources(state / "resources"):
        return "local", state / "resources"
    candidates = sorted(p for p in root.glob("resource_dump*") if p.is_file() and p.suffix.lower() in (".7z", ".zip"))
    if len(candidates) == 1:
        return "dump", candidates[0]
    return "", None


@contextmanager
def resource_library(root, state, source, report=None):
    kind, path = source
    local = state / "resources"
    saved = state / "asset-library"
    if kind == "assets":
        external = path
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
    if not kind:
        raise ValueError("Choose a resource archive in the launcher or run ./Play.sh --dump '/path/to/resources.zip'.")
    if kind == "dump":
        command = [RUNTIME / "bunker-server", "import", "-assets", local, "-dump", path, "-master", root / MASTER]
        with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as errors, subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=errors, text=True, encoding="utf-8") as importer:
            for line in importer.stdout:
                if report:
                    progress = json.loads(line)
                    if progress.get("total", 0) > 0:
                        report({"progress": round(100 * progress["done"] / progress["total"])})
                else:
                    print(line, end="", flush=True)
            if importer.wait():
                errors.seek(0)
                raise ValueError(errors.read().strip() or "Bunker could not import the resource archive.")
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
    window = parser.add_mutually_exclusive_group()
    window.add_argument("--launcher", action="store_true", help="open the graphical Bunker launcher")
    window.add_argument("--no-launcher", action="store_true", help="start the game directly")
    parser.add_argument("--server-ports", nargs="?", const="", metavar="PORT|default",
                        help="show or save the local asset port and exit")
    parser.add_argument("unity_args", nargs=argparse.REMAINDER, help="Unity player arguments after --")
    args = parser.parse_args()
    root = Path.cwd()
    state = root / ".reincomp"
    if args.server_ports is not None:
        configure_ports(state / "server-ports.json", args.server_ports)
        if "REINCOMP_BUNKER_PORT_OFFSET" in os.environ:
            print("REINCOMP_BUNKER_PORT_OFFSET overrides the saved setting while it is set.")
        return 0
    state.mkdir(exist_ok=True)
    source = resource_source(root, state, args.assets, args.dump)
    desktop = os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    direct = args.no_launcher or args.dump or args.assets or args.unity_args
    if not args.prepare_only and (args.launcher or (desktop and not direct)):
        result = open_launcher(root, state, source, args)
        if result is not None:
            return result
        if args.launcher:
            raise RuntimeError("The Bunker window requires a desktop session, Python GObject, GTK 3 and WebKitGTK 4.1.")
    return play(root, state, source, args)


def open_launcher(root, state, source, args):
    # GTK uses the desktop's GI bindings. The bundled interpreter continues to own
    # the asset converter, Bunker server and Unity process, as it does for CLI play.
    python = Path("/usr/bin/python3")
    if not python.is_file():
        return None
    helper = Path(__file__).with_name("launcher_window.py")
    kind, path = source
    with subprocess.Popen([python, "-I", helper, kind, str(path or "")], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True, encoding="utf-8") as window:
        def notify(event):
            if window.poll() is not None:
                return False
            try:
                window.stdin.write(json.dumps(event) + "\n")
                window.stdin.flush()
                return True
            except BrokenPipeError:
                return False

        try:
            for line in window.stdout:
                choice = json.loads(line)
                try:
                    result = play(root, state, (choice["kind"], Path(choice["path"])), args, report=notify)
                    if result == 0:
                        return 0
                    message = f"Unity exited with code {result}. See the player log."
                except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
                    message = str(error)
                if not notify({"error": message}):
                    break
            result = window.wait()
            return None if result == 78 else result
        finally:
            try:
                window.stdin.close()
            except BrokenPipeError:
                pass


def play(root, state, source, args, report=None):
    with resource_library(root, state, source, report) as assets:
        pack = root / "notices.zip"
        if not pack.is_file():
            raise ValueError("Missing notices.zip. Extract the complete browser download before launching.")
        notices = assets / "notices" / "notices.zip"
        notices.parent.mkdir(exist_ok=True)
        if not notices.exists():
            notices.symlink_to(os.path.relpath(pack, notices.parent))
        if report and not report({"stage": "preparing"}):
            return 0
        player = prepare_player(root, state)
        if args.prepare_only:
            print("Linux player and game resources are ready.")
            return 0
        if report and not report({"stage": "starting"}):
            return 0
        log = state / "server.log"
        override = os.environ.get("REINCOMP_BUNKER_PORT_OFFSET")
        offset = int(override) if override is not None else saved_asset_port(state / "server-ports.json") - BASE_ASSET_PORT
        if not 0 <= offset <= 57455:
            raise ValueError("Invalid Bunker port offset")
        with log.open("w") as errors, subprocess.Popen(
                [RUNTIME / "bunker-server", "-assets", assets, "-data", state / "save",
                 "-port-offset", str(offset), "-stdin"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors) as server:
            try:
                with selectors.DefaultSelector() as ready:
                    ready.register(server.stdout, selectors.EVENT_READ)
                    if not ready.select(90) or not server.stdout.readline():
                        with log.open("rb") as failure:
                            failure.seek(max(0, log.stat().st_size - 2048))
                            lines = failure.read().decode(errors="replace").splitlines()
                        detail = lines[-1] if lines else ""
                        conflict = re.search(r"\bPort (\d+) is already used\b", detail)
                        if conflict:
                            detail = (f"Port {conflict[1]} is already in use. Choose another asset port under "
                                      "⋮ → Server ports, or run ./Play.sh --server-ports.")
                        raise RuntimeError(f"Lunar Tear could not start.\n{detail}\nSee {log}")
                game_args = args.unity_args
                if game_args[:1] == ["--"]:
                    game_args = game_args[1:]
                if report and not report({"stage": "playing"}):
                    return 0
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
