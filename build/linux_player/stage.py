"""Stage the Linux Unity player and startup data from a prepared reincomp install."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tarfile
from tempfile import TemporaryDirectory
from zipfile import ZipFile


# Unity 2019.4.29f1 (0eeae20b1d82), linux64_withgfx_nondevelopment_il2cpp.
IL2CPP_SHA256 = "ff5c66ad2fdaa957fa005f1957988b3277331abefcdedcdb0c82e59ba2e80522"
GRPC_LINUX_SHA256 = "dacd6efa49f2a7f0fc54091c15c8ce4a3edd778446205f7642c7f14f1bdba9a9"
FIREBASE_LINUX_SHA256 = "ca0cab5049d6e8ae28a3c188294240c1275b53c4a2e7a97068a17902cd195633"
UNITY_SYMBOLS_SHA256 = "dc554c57525f77f0f359947f17d109c76d3a81384e6e0d44d58ffed5f1127655"
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


def stage(install, unity, out, server=None, game_assembly=None, grpc_package=None, firebase_package=None):
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
    if grpc_package:
        with ZipFile(grpc_package) as package:
            grpc = package.read("runtimes/linux/native/libgrpc_csharp_ext.x64.so")
        if hashlib.sha256(grpc).hexdigest() != GRPC_LINUX_SHA256:
            raise ValueError("Expected Grpc.Core 2.26.0's Linux x86-64 extension")
    if firebase_package:
        plugin = "FirebaseCppApp-9_6_0.so"
        with tarfile.open(firebase_package, "r:gz") as package:
            with package.extractfile("package/Firebase/Plugins/x86_64/" + plugin) as member:
                firebase = member.read()
        if hashlib.sha256(firebase).hexdigest() != FIREBASE_LINUX_SHA256:
            raise ValueError("Expected Firebase Unity 9.6.0's Linux x86-64 plugin")
    out.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(dir=out.parent) as tmp:
        package = Path(tmp) / "player"
        linux_data = package / "Reincarnation_Data"
        shutil.copytree(data, linux_data, copy_function=copy_data)
        shutil.copyfile(unity / "Data/Resources/unity default resources", linux_data / "Resources/unity default resources")
        shutil.copyfile(unity / "UnityPlayer.so", package / "UnityPlayer.so")
        if game_assembly or grpc_package or firebase_package:
            native = linux_data / "Native"
            native.mkdir(exist_ok=True)
        if grpc_package:
            (native / "libgrpc_csharp_ext.x64.so").write_bytes(grpc)
            shutil.copytree(install / "licenses/grpc", package / "licenses/grpc")
        if firebase_package:
            (native / plugin).write_bytes(firebase)
            shutil.copytree(install / "licenses/firebase", package / "licenses/firebase")
        if firebase_package or game_assembly:
            assets = install / "data/apk-assets"
            if firebase_package and not (assets / "google-services-desktop.json").is_file():
                raise ValueError("Missing the game's Firebase desktop config in data/apk-assets")
            if assets.is_dir():
                # bin/Data is the Android player data. The Linux data above
                # already contains the converted version. DEX is Android code.
                streaming = linux_data / "StreamingAssets"
                streaming.mkdir(exist_ok=True)
                for entry in assets.iterdir():
                    if entry.name == "bin" or entry.suffix == ".dex":
                        continue
                    if entry.is_dir():
                        shutil.copytree(entry, streaming / entry.name, dirs_exist_ok=True)
                    else:
                        shutil.copyfile(entry, streaming / entry.name)
        if game_assembly:
            shutil.copyfile(source, native / "libil2cpp.so")
            shutil.copyfile(game_assembly, package / "GameAssembly.so")
            tools = package / "tools"
            tools.mkdir()
            here = Path(__file__).resolve().parent
            shutil.copyfile(here / "bundle_worker.py", tools / "bundle_worker.py")
            for name in ("desktop_bundles.py", "desktop_shader.py"):
                shutil.copyfile(here.parent / "native_player" / name, tools / name)
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
    parser.add_argument("--grpc-package", type=Path, help="include the Linux native extension from Grpc.Core 2.26.0")
    parser.add_argument("--firebase-package", type=Path, help="include the Linux plugin from Google's com.google.firebase.app-9.6.0.tgz")
    args = parser.parse_args()
    stage(args.install, args.unity, args.out, args.server, args.game_assembly, args.grpc_package, args.firebase_package)
