"""Package RecompLauncher's two desktop installs from verified public release inputs."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
from portable_inputs import KIT, NOTICES, PORTABLE, RELEASE, digest, download

VERSION = (ROOT / "launcher/VERSION").read_text().strip()
LINUX = json.loads((ROOT / "release.lock.json").read_bytes())["linuxRuntime"]
MASTER_HASH = "60555788242de9a0580b0d86f6d109d67bd9cabd16d7bcda9b9a99745f270778"
PAGES = "https://veritr1x.github.io/bunker/windows/runtime/"
WHEELS = {
    "4d/ec/fd869e2567cc9c01278a736cfd1697941ba0d4b81a43e0aa2e8d71dab208/msgpack-1.1.1-cp312-cp312-manylinux_2_17_x86_64.manylinux2014_x86_64.whl":
        "a494554874691720ba5891c9b0b39474ba43ffb1aaf32a5dac874effb1619e1a",
    "ea/66/6f2b7ddb457b19f73b82053ecc83ba768680609d56dd457dbc7e902c41aa/pycryptodome-3.21.0-cp36-abi3-manylinux_2_17_x86_64.manylinux2014_x86_64.whl":
        "0714206d467fc911042d01ea3a1847c847bc10884cf674c82e12915cfe1649f8",
}


def public_file(cache, item, base, local=None):
    if local and local.is_file():
        if digest(local) != item[1]:
            raise ValueError(f"Unexpected checksum for {local}")
        return local
    return download(cache, item, base)


def windows(folder, kit, cache, site):
    description = json.loads(kit.read("player.json"))
    with tempfile.TemporaryFile() as merged:
        for name, sha in description["parts"]:
            part = kit.read(name)
            if hashlib.sha256(part).hexdigest() != sha:
                raise ValueError(f"Corrupt player kit part {name}")
            merged.write(part)
        with ZipFile(merged) as files:
            for name in ["launcher.exe", *description["members"]]:
                files.extract(name, folder)
    (folder / "game/GameAssembly.dll").rename(folder / "setup/GameAssembly.template")
    (folder / "game/windows-version.txt").write_text(PORTABLE["bunkerVersion"] + "\n")
    runtime = public_file(cache, ("runtime.json", PORTABLE["runtimeManifestSHA256"]), PAGES,
                          site and site / "windows/runtime/runtime.json")
    info = json.loads(runtime.read_bytes())
    if info["sourceExeSHA256"] != PORTABLE["sha256"] or info["kitSHA256"] != hashlib.sha256(kit.read("kit.json")).hexdigest():
        raise ValueError("The Windows runtime and browser kit must match the tagged player.")
    with tempfile.TemporaryFile() as packed:
        for i, part in enumerate(info["parts"], 1):
            name = f"runtime.part{i:03}"
            path = public_file(cache, (name, part["sha256"]), PAGES, site and site / "windows/runtime" / name)
            with path.open("rb") as source:
                shutil.copyfileobj(source, packed)
        packed.seek(0)
        with gzip.GzipFile(fileobj=packed) as source, (folder / "game/GuestRuntime.dll").open("wb") as out:
            shutil.copyfileobj(source, out, 1024 * 1024)
    if (folder / "game/GuestRuntime.dll").stat().st_size != info["size"]:
        raise ValueError("Incomplete guest runtime")


def linux(folder, profile, cache, runtime_archive):
    if sys.platform != "linux":
        raise ValueError("Package the native runtime on Linux to preserve executable permissions.")
    name = LINUX["asset"]
    if name != f"reincomp-linux-x86_64-{VERSION}.tar.gz":
        raise ValueError("The native runtime pin must match the release.")
    base = f"https://github.com/veritr1x/reincomp/releases/download/v{VERSION}/"
    archive = public_file(cache, (name, LINUX["sha256"]), base, runtime_archive)
    runtime = f"runtime/{VERSION}"
    with tarfile.open(archive) as files:
        files.extractall(folder / runtime, filter="data")
    for rel, sha in WHEELS.items():
        url, name = rel.rsplit("/", 1)
        wheel = download(cache, (name, sha), "https://files.pythonhosted.org/packages/" + url + "/")
        with ZipFile(wheel) as files:
            files.extractall(folder / runtime / "python/lib/python3.12/site-packages")
    python = runtime + "/python/bin/python3"
    profile["prepare"]["program"] = [python]
    profile["prepare"]["done"] = ["{data}/ready", "{data}/.reincomp/player-" + VERSION + "/Reincarnation_Data/globalgamemanagers",
                                  "{data}/.reincomp/resources/assets/release/20240404193219.bin.e"]
    profile["start"] = {"program": [python], "args": ["-I", "-B", "{install}/setup/prepare.py", "--play", "--"]}
    profile["config"][0]["file"] = [{"path": "{data}/.reincomp/server-ports.json"}]
    port = next(setting for setting in profile["settings"] if setting["id"] == "assetPort")
    port["defaults"]["assetPort"] = "8080"
    profile["settings"] = [port, {"id": "startup", "label": "On launch", "in": "args", "choices": [
        {"label": "Start the game", "args": []}, {"label": "Open Bunker", "args": ["--launcher"]}]},
        {"id": "window", "label": "Window mode", "in": "args", "choices": [
        {"label": "Game setting", "args": []}, {"label": "Windowed", "args": ["-screen-fullscreen", "0"]},
        {"label": "Fullscreen", "args": ["-screen-fullscreen", "1"]}]}]
    profile["saves"] = [path.replace("{install}/data/server", "{data}/.reincomp/save") for path in profile["saves"]]
    profile["folders"]["logs"] = [{"path": "{data}/.reincomp"}]


def package(args):
    args.cache.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    kit_path = public_file(args.cache, KIT, RELEASE, args.kit)
    notices = public_file(args.cache, NOTICES, RELEASE, args.site and args.site / "bundles/notices.zip")
    with tempfile.TemporaryDirectory(dir=args.out) as tmp, ZipFile(kit_path) as kit:
        work = Path(tmp)
        folder = work / "install"
        setup = folder / "setup"
        setup.mkdir(parents=True)
        plan = json.loads(kit.read("game-plan.json"))
        recipe = {"version": VERSION, "apkSHA256": plan["apkSHA256"], "masterSHA256": MASTER_HASH, "metadata": plan["metadata"]}
        (setup / "recipe.json").write_text(json.dumps(recipe))
        (setup / "patch_masterdata.py").write_bytes(kit.read("patch_masterdata.py"))
        shutil.copyfile(HERE / "prepare.py", setup / "prepare.py")
        profile = json.loads((HERE / "recomplauncher.json").read_bytes())
        if args.platform == "windows":
            windows(folder, kit, args.cache, args.site)
            shutil.copyfile(notices, folder / "game/notices.zip")
            suffix = "zip"
        else:
            linux(folder, profile, args.cache, args.linux_runtime)
            shutil.copyfile(notices, folder / "notices.zip")
            suffix = "tar.gz"
        (folder / "recomplauncher.json").write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
        name = f"reincomp-{VERSION}-{args.platform}-x64.{suffix}"
        result = work / name
        if args.platform == "windows":
            with ZipFile(result, "w", compression=ZIP_DEFLATED, compresslevel=4) as out:
                for file in sorted(folder.rglob("*")):
                    if file.is_file():
                        out.write(file, file.relative_to(folder))
        else:
            with tarfile.open(result, "w:gz", compresslevel=4) as out:
                for entry in sorted(folder.iterdir()):
                    out.add(entry, arcname=entry.name)
        result.replace(args.out / name)
        print(args.out / name, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["windows", "linux"])
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--kit", type=Path, help="local copy of the tagged public browser kit")
    parser.add_argument("--site", type=Path, help="local verified Bunker Pages build")
    parser.add_argument("--linux-runtime", type=Path, help="local copy of the released native runtime")
    package(parser.parse_args())
