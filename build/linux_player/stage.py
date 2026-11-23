"""Stage the Linux Unity player and startup data from a prepared reincomp install."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
from tempfile import TemporaryDirectory


# Unity 2019.4.29f1 (0eeae20b1d82), linux64_withgfx_nondevelopment_il2cpp.
IL2CPP_SHA256 = "ff5c66ad2fdaa957fa005f1957988b3277331abefcdedcdb0c82e59ba2e80522"
UNITY_FILES = {
    "LinuxPlayer": "19d9b4735933c6ff013a05831a69acd3a10269b2934b38e51ff641b0202e01b1",
    "UnityPlayer.so": "13a0f9f2b0c6cde8330c4a95ad2ea285acaf25fcae29bbfcc953fd70da9beec7",
    "Data/Resources/unity default resources": "28f34dd870e9ef576828fca791d86a90f932a3d6814b947dc191fbed2354b888",
}


def linux_header(header, source):
    if header[8:12] == (21).to_bytes(4, "big") and header[20:32] == b"2019.4.29f1\0":
        if header[16:20] != bytes(4) or header[32:36] != (19).to_bytes(4, "little"):
            raise ValueError(f"Expected little-endian StandaloneWindows64 data in {source}")
        return header[:32] + (24).to_bytes(4, "little")
    return header


def copy_data(source, dest):
    # The Android texture and shader conversion already happened when the
    # install was prepared. Only the BuildTarget field differs for Linux GL.
    with open(source, "rb") as src, open(dest, "wb") as out:
        header = src.read(36)
        out.write(linux_header(header, source))
        shutil.copyfileobj(src, out)


def stage(install, unity, out, server=None, game_assembly=None):
    data = install / "game/WindowsPlayer_Data"
    for name in ("globalgamemanagers", "Managed/Metadata/global-metadata.dat"):
        if not (data / name).is_file():
            raise ValueError(f"Missing prepared game file: {data / name}")
    managers = data / "globalgamemanagers"
    with managers.open("rb") as stream:
        header = stream.read(36)
    if linux_header(header, managers) == header:
        raise ValueError(f"Expected Unity 2019.4.29f1 startup data in {managers}")
    if out.exists() or out.resolve().is_relative_to(data.resolve()):
        raise ValueError("Choose a new output directory outside WindowsPlayer_Data")
    for name, expected in UNITY_FILES.items():
        with (unity / name).open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != expected:
                raise ValueError(f"Unexpected Unity 2019.4.29f1 Linux module: {name}")
    if game_assembly:
        with game_assembly.open("rb") as src:
            header = src.read(64)
        if (len(header) != 64 or header[:7] != b"\x7fELF\x02\x01\x01"
                or struct.unpack_from("<HH", header, 16) != (3, 62)):
            raise ValueError("Expected an x86-64 Linux shared object for GameAssembly.so")
        source = install / "data/native/libil2cpp.so"
        with source.open("rb") as src:
            if hashlib.file_digest(src, "sha256").hexdigest() != IL2CPP_SHA256:
                raise ValueError("Expected the original Global 3.7.1 IL2CPP image")
    if server:
        lock = Path(__file__).resolve().parents[2] / "bunker.lock.json"
        pin = json.loads(lock.read_bytes())["bunker"]["server"]["linux-x86_64"]
        with server.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != pin["sha256"]:
                raise ValueError(f"Expected the pinned Bunker server {pin['file']}")
    out.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=out.parent) as tmp:
        package = Path(tmp) / "player"
        linux_data = package / "Reincarnation_Data"
        shutil.copytree(data, linux_data, copy_function=copy_data)
        shutil.copyfile(unity / "Data/Resources/unity default resources", linux_data / "Resources/unity default resources")
        shutil.copyfile(unity / "UnityPlayer.so", package / "UnityPlayer.so")
        if game_assembly:
            native = linux_data / "Native"
            native.mkdir(exist_ok=True)
            shutil.copyfile(source, native / "libil2cpp.so")
            shutil.copyfile(game_assembly, package / "GameAssembly.so")
        player = package / "Reincarnation.x86_64"
        shutil.copyfile(unity / "LinuxPlayer", player)
        player.chmod(0o755)
        if server:
            backend = package / "bunker-server"
            shutil.copyfile(server, backend)
            backend.chmod(0o755)
        package.rename(out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", type=Path, required=True, help="extracted .bunker-player directory")
    parser.add_argument("--unity", type=Path, required=True, help="linux64_withgfx_nondevelopment_il2cpp directory")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--server", type=Path, help="include the pinned Bunker Linux x86-64 server")
    parser.add_argument("--game-assembly", type=Path, help="include the locally built native Linux GameAssembly.so")
    args = parser.parse_args()
    stage(args.install, args.unity, args.out, args.server, args.game_assembly)
