"""Prepare a RecompLauncher install using its bundled Python and the player's files."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZipFile

SETUP = Path(__file__).resolve().parent
INSTALL = SETUP.parent
DATA = Path(os.environ.get("RECOMPLAUNCHER_DATA", INSTALL / "recompdata")).resolve()
MASTER = "20240404193219.bin.e"
WINDOWS = os.name == "nt"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def has_resources(assets):
    revision = assets / "assets/revisions/0"
    return any((base / "list.bin").is_file() and (base / "assetbundle").is_dir() and (base / "resources").is_dir()
               and ((base / "info.json").is_file() or (revision / "info.json").is_file())
               for base in (revision, revision / "android"))


def linux_play(args):
    script = Path(sys.executable).resolve().parents[2] / "tools/web_launch.py"
    # The profile puts Unity's options after --. Bunker's own flag goes before it.
    if "--launcher" in args:
        args = ["--launcher", *(arg for arg in args if arg != "--launcher")]
    os.chdir(DATA)
    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ.get("PATH", "")
    os.environ["PYTHONNOUSERSITE"] = "1"
    os.execv(sys.executable, [sys.executable, "-B", str(script), *args])


def prepare_windows(apk, master, patched, metadata, work):
    game = work / "game"
    game.mkdir()
    shutil.copyfile(SETUP / "GameAssembly.template", game / "GameAssembly.dll")
    shutil.copyfile(INSTALL / "game/portable-emblem.json", game / "portable-emblem.json")
    metadata_file = work / "global-metadata.dat"
    metadata_file.write_bytes(metadata)
    # UnityPy holds mapped files until its interpreter exits on Windows.
    subprocess.run([sys.executable, "-I", "-B", "-X", "utf8", INSTALL / "game/package_assets.py",
                    apk, work, metadata_file], check=True)
    shutil.copyfile(master, work / "data/original-master.bin.e")
    shutil.copyfile(patched, work / "data/import-master.bin.e")
    for directory in (game, work / "data"):
        for src in directory.rglob("*"):
            if src.is_file():
                dest = INSTALL / src.relative_to(work)
                dest.parent.mkdir(parents=True, exist_ok=True)
                src.replace(dest)


def prepare_linux(apk, patched, metadata, work, runtime, state):
    sys.path.insert(0, str(runtime / "tools"))
    from web_launch import prepare_player
    shutil.copyfile(apk, work / "game.apk")
    (work / "global-metadata.dat").write_bytes(metadata)
    for name in ("game.apk", "global-metadata.dat"):
        (work / name).replace(DATA / name)
    shutil.copyfile(patched, DATA / MASTER)
    shutil.copyfile(INSTALL / "notices.zip", DATA / "notices.zip")
    player = state / ("player-" + runtime.name)
    if player.is_dir() and not (player / "Reincarnation_Data/globalgamemanagers").is_file():
        player.rename(work / "incomplete-player")
    prepare_player(DATA, state)


def prepare(apk, master, dump):
    recipe = json.loads((SETUP / "recipe.json").read_bytes())
    print("progress 0.02 Checking the APK and master data", flush=True)
    if digest(apk) != recipe["apkSHA256"] or digest(master) != recipe["masterSHA256"]:
        raise ValueError("Choose the original Global 3.7.1 ARM64 APK and the April 2024 master data.")
    info = dump.stat()
    receipt = {"version": recipe["version"], "dump": [str(dump), info.st_size, info.st_mtime_ns]}
    DATA.mkdir(parents=True, exist_ok=True)
    ready = DATA / "ready"
    state = INSTALL / "data" if WINDOWS else DATA / ".reincomp"
    assets = state / ("assets" if WINDOWS else "resources")
    runtime = INSTALL if WINDOWS else Path(sys.executable).resolve().parents[2]
    server = runtime / ("server/bunker-server.exe" if WINDOWS else "bunker-server")
    installed_master = assets / "assets/release" / MASTER
    player = INSTALL / "game/WindowsPlayer_Data" if WINDOWS else state / ("player-" + runtime.name) / "Reincarnation_Data"
    previous = json.loads(ready.read_bytes()) if ready.exists() else {}
    if not dump.is_file() or dump.suffix.lower() not in (".zip", ".7z"):
        raise ValueError("Choose the Android resource dump as a ZIP or 7z archive.")
    same_dump = previous.get("dump") == receipt["dump"] and has_resources(assets)
    if previous == receipt and same_dump and installed_master.is_file() and (player / "globalgamemanagers").is_file():
        print("progress 1 Game files are ready", flush=True)
        return
    ready.unlink(missing_ok=True)
    state.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=DATA) as tmp:
        work = Path(tmp)
        spec = recipe["metadata"]
        with ZipFile(apk) as source:
            metadata = bytearray(source.read(spec["path"]))
        for edit in spec["edits"]:
            offset, value = edit["offset"], bytes.fromhex(edit["bytes"])
            metadata[offset:offset + len(value)] = value
        if hashlib.sha256(metadata).hexdigest() != spec["patchedSHA256"]:
            raise ValueError("The prepared APK metadata failed its checksum.")
        print("progress 0.12 Patching master data", flush=True)
        patched = work / MASTER
        subprocess.run([sys.executable, "-I", "-B", "-X", "utf8", SETUP / "patch_masterdata.py",
                        "--input", master, "--output", patched], check=True)
        print("progress 0.25 Preparing Unity player assets", flush=True)
        if WINDOWS:
            prepare_windows(apk, master, patched, metadata, work)
        else:
            prepare_linux(apk, patched, metadata, work, runtime, state)
        print("progress 0.55 Importing the Android resource dump", flush=True)
        command = [server, "import", "-assets", assets, "-master", patched]
        if not same_dump:
            command.extend(["-dump", dump])
        with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as proc:
            for line in proc.stdout:
                item = json.loads(line)
                if item.get("total", 0) > 0:
                    fraction = min(1, item["done"] / item["total"])
                    print(f"progress {0.55 + 0.44 * fraction:.3f} Importing resources", flush=True)
            if proc.wait():
                raise ValueError("Bunker could not import the resource dump.")
        if not has_resources(assets):
            raise ValueError("The dump is missing the Global Android resource catalog.")
        pending = work / "ready"
        pending.write_text(json.dumps(receipt))
        pending.replace(ready)
    print("progress 1 Game files are ready", flush=True)


if __name__ == "__main__":
    try:
        if not WINDOWS and sys.argv[1:2] == ["--play"]:
            linux_play(sys.argv[2:])
        if len(sys.argv) != 4:
            sys.exit("Expected the original APK, master data and resource archive.")
        prepare(*(Path(arg).resolve() for arg in sys.argv[1:]))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
