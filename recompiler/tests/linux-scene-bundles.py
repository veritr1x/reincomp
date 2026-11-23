"""Load original gameplay assets through the native Linux player."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import threading
import time


def probe():
    import gdb

    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("set print thread-events off")
    gdb.execute("set debuginfod enabled off")
    gdb.execute("handle " + " ".join(f"SIG{i}" for i in range(34, 65)) + " nostop noprint pass")
    gdb.execute("catch load GameAssembly")
    gdb.execute("run")
    process = gdb.selected_inferior()

    def pointer(address):
        return int.from_bytes(process.read_memory(address, 8), "little")

    class SceneReady(gdb.Breakpoint):
        expected = b"Internal_ActiveSceneChanged"
        def stop(self):
            method = int(gdb.parse_and_eval("$rdi"))
            name = pointer(method + 16)
            return bytes(process.read_memory(name, len(self.expected) + 1)) == self.expected + b"\0"

    ready = SceneReady("il2cpp_runtime_invoke")
    gdb.execute("continue")
    activate = os.environ.get("REINCOMP_TEST_ACTIVATE") == "1"
    if activate:
        ready.expected = b"DoSendMouseEvents"
        gdb.execute("continue")
    ready.delete()

    def call(function, result, types, args):
        expression = f"(({result}(*)({types})){function})({args})"
        value = gdb.parse_and_eval(expression)
        return int(value) if result != "void" else None

    def api(name, result, types, args=""):
        return call("&il2cpp_" + name, result, types, args)

    def engine(name, result, types, args):
        function = api("resolve_icall", "void *", "const char *", json.dumps("UnityEngine." + name))
        assert function, name
        return call(function, result, types, args)

    domain = api("domain_get", "void *", "void")
    scratch = call("&malloc", "void *", "unsigned long", 128)

    def klass(image, namespace, name):
        assembly = api("domain_assembly_open", "void *", "void *,const char *", f'{domain},"{image}"')
        image = api("assembly_get_image", "void *", "void *", assembly)
        result = api("class_from_name", "void *", "void *,const char *,const char *",
                     f'{image},"{namespace}","{name}"')
        assert result, name
        return result

    def invoke(owner, method, obj, args):
        target = api("class_get_method_from_name", "void *", "void *,const char *,int",
                     f'{owner},"{method}",{len(args)}')
        assert target, method
        process.write_memory(scratch, b"".join(struct.pack("<Q", x) for x in args) + bytes(8))
        process.write_memory(scratch + 64, bytes(8))
        result = api("runtime_invoke", "void *", "void *,void *,void **,void **",
                     f"{target},{obj},{scratch if args else 0},{scratch + 64}")
        assert pointer(scratch + 64) == 0, f"{method} raised a managed exception"
        return result

    def managed_string(value):
        # Hand GDB an ASCII expression even when the Linux path contains UTF-8.
        encoded = "".join(f"\\{byte:03o}" for byte in os.fsencode(value))
        return api("string_new", "void *", "const char *", f'"{encoded}"')

    def text(value):
        chars = api("string_chars", "void *", "void *", value)
        length = api("string_length", "int", "void *", value)
        return bytes(process.read_memory(chars, length * 2)).decode("utf-16le")

    if os.environ.get("REINCOMP_TEST_DOWNLOAD_URL"):
        locale = klass("LocalizeTime", "Dark.Localization", "LocalizeTime")
        zone = text(invoke(locale, "GetLocalTimeZoneId", 0, []))
        assert zone == "Europe/Berlin", zone
        assert invoke(locale, "get_LocalTimeZone", 0, []), "Mission time zone could not be resolved"
        print("PASS original mission clock resolved the configured Linux time zone", flush=True)
        def nested(owner, expected):
            process.write_memory(scratch + 96, bytes(8))
            while True:
                item = api("class_get_nested_types", "void *", "void *,void **", f"{owner},{scratch+96}")
                assert item, expected
                name = api("class_get_name", "void *", "void *", item)
                if bytes(process.read_memory(name, len(expected) + 1)) == expected.encode() + b"\0":
                    return item

        request_type = klass("Octo", "Octo.Network", "AndroidFileDownloadRequest")
        bridge = nested(request_type, "Bridge")
        task_type = nested(request_type, "NativeHttpAsyncTask")
        result_type = nested(request_type, "NativeHttpResult")
        cache = Path(text(engine("Application::get_persistentDataPath", "void *", "void", "")))
        volume = os.statvfs(cache)
        available = volume.f_bavail * volume.f_frsize
        storage = klass("Assembly-CSharp", "Framework.Network.Download", "Storage")
        checker = api("object_new", "void *", "void *", storage)
        invoke(storage, ".ctor", checker, [])
        for required, expected in ((1, True), (available + 1024**3, False)):
            process.write_memory(scratch + 80, struct.pack("<q", required))
            enough = invoke(storage, "CheckDownloadFileSize", checker, [scratch + 80])
            value = api("object_unbox", "void *", "void *", enough)
            assert bool(bytes(process.read_memory(value, 1))[0]) == expected, required
        print("PASS native game accepts a small download and rejects one larger than the cache volume", flush=True)
        destination = cache / "octo" / "test download"
        url = os.environ["REINCOMP_TEST_DOWNLOAD_URL"]
        payload = b"Reincomp native Octo test\n" * 1024
        task = invoke(bridge, "DownloadFile", 0, [managed_string(url), managed_string(destination), 0])
        assert task
        try:
            result = invoke(task_type, "get_Result", task, [])
            assert result
            deadline = time.monotonic() + 10
            while True:
                done = invoke(result_type, "get_IsComplete", result, [])
                if bytes(process.read_memory(api("object_unbox", "void *", "void *", done), 1)) == b"\x01":
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Octo did not finish the local download")
                time.sleep(.02)
            assert destination.read_bytes() == payload
            assert text(invoke(result_type, "get_Md5", result, [])).lower() == hashlib.md5(payload).hexdigest()
            print("PASS original Octo downloaded the local fixture with matching bytes and MD5", flush=True)
        finally:
            invoke(task_type, "Dispose", task, [])

    if os.environ.get("REINCOMP_TEST_STREAMING") == "1":
        data = Path("Reincarnation_Data").resolve()
        web = klass("UnityEngine.UnityWebRequestModule", "UnityEngine.Networking", "UnityWebRequest")
        download = klass("UnityEngine.UnityWebRequestModule", "UnityEngine.Networking", "DownloadHandler")
        icon = data / "StreamingAssets/Icon-60@3x.jpg"
        request = invoke(web, "Get", 0, [managed_string(f"jar:file://{data}!/assets/{icon.name}")])
        assert request
        pin = api("gchandle_new", "unsigned", "void *,unsigned char", f"{request},1")
        try:
            invoke(web, "SendWebRequest", request, [])
            deadline = time.monotonic() + 15
            while True:
                done = invoke(web, "get_isDone", request, [])
                address = api("object_unbox", "void *", "void *", done)
                if bytes(process.read_memory(address, 1)) != b"\x00":
                    break
                if time.monotonic() >= deadline:
                    raise TimeoutError("Unity did not finish reading the packaged icon")
                time.sleep(0.01)
            handler = invoke(web, "get_downloadHandler", request, [])
            result = invoke(download, "get_data", handler, [])
            assert result, "Unity returned no icon data"
            header = api("array_object_header_size", "unsigned", "void")
            size = api("array_length", "unsigned", "void *", result)
            assert bytes(process.read_memory(result + header, size)) == icon.read_bytes()
            print("PASS native Unity read the original packaged icon bytes", flush=True)
        finally:
            invoke(web, "Dispose", request, [])
            api("gchandle_free", "void", "unsigned", pin)

    asset = klass("UnityEngine.AssetBundleModule", "UnityEngine", "AssetBundle")
    memory_stream = klass("mscorlib", "System.IO", "MemoryStream")
    byte = klass("mscorlib", "System", "Byte")
    field, storybook = map(Path, json.loads(os.environ["REINCOMP_TEST_BUNDLES"]))

    def wait_for_frames(operation):
        assert operation, "Unity refused the scene operation"
        pin = api("gchandle_new", "unsigned", "void *,unsigned char", f"{operation},1")
        frame = SceneReady("il2cpp_runtime_invoke")
        frame.expected = b"DoSendMouseEvents"
        try:
            for _ in range(180):
                if engine("AsyncOperation::get_isDone", "unsigned char", "void *", operation):
                    return
                gdb.execute("continue", to_string=True)
            raise TimeoutError("Unity did not finish the scene operation in 180 frames")
        finally:
            frame.delete()
            api("gchandle_free", "void", "unsigned", pin)

    def activate_scene(path):
        scene_path = managed_string(path)
        scene_pin = api("gchandle_new", "unsigned", "void *,unsigned char", f"{scene_path},1")
        try:
            # LoadSceneParameters contains LoadSceneMode.Additive and LocalPhysicsMode.None.
            process.write_memory(scratch + 80, struct.pack("<ii", 1, 0))
            operation = engine("SceneManagement.SceneManagerAPIInternal::LoadSceneAsyncNameIndexInternal_Injected",
                               "void *", "void *,int,void *,unsigned char", f"{scene_path},-1,{scratch+80},0")
            wait_for_frames(operation)
            engine("SceneManagement.SceneManager::GetSceneByPath_Injected", "void", "void *,void *",
                   f"{scene_path},{scratch+80}")
            handle = int.from_bytes(process.read_memory(scratch + 80, 4), "little", signed=True)
            assert engine("SceneManagement.Scene::GetIsLoadedInternal", "unsigned char", "int", handle)
            roots = engine("SceneManagement.Scene::GetRootCountInternal", "int", "int", handle)
            assert roots > 0, f"{path} loaded without root GameObjects"
            print(f"PASS native scene {path} instantiated {roots} root GameObjects", flush=True)
            wait_for_frames(engine("SceneManagement.SceneManager::UnloadSceneAsyncInternal_Injected",
                                   "void *", "void *,int", f"{scratch+80},0"))
        finally:
            api("gchandle_free", "void", "unsigned", scene_pin)

    def check_request(request, expected):
        assert request, expected
        bundle = engine("AssetBundleCreateRequest::get_assetBundle", "void *", "void *", request)
        assert bundle, expected
        paths = engine("AssetBundle::GetAllScenePaths", "void *", "void *", bundle)
        count = api("array_length", "unsigned", "void *", paths)
        header = api("array_object_header_size", "unsigned", "void")
        scenes = [text(pointer(paths + header + 8 * i)) for i in range(count)]
        matches = [path for path in scenes if path.lower().endswith("/" + expected)]
        assert matches, scenes
        if activate:
            activate_scene(matches[0])
        engine("AssetBundle::Unload", "void", "void *,unsigned char", f"{bundle},1")
        print("PASS native Unity loaded " + ", ".join(scenes), flush=True)

    check_request(invoke(asset, "LoadFromFileAsync", 0, [managed_string(field)]), "tw002001.unity")
    if os.environ.get("REINCOMP_TEST_BATTLE_FIELD"):
        field_path = managed_string(os.environ["REINCOMP_TEST_BATTLE_FIELD"])
        request = invoke(asset, "LoadFromFileAsync", 0, [field_path])
        assert request, "Battle field bundle request failed"
        bundle = engine("AssetBundleCreateRequest::get_assetBundle", "void *", "void *", request)
        assert bundle, "Battle field bundle did not load"
        game_object = klass("UnityEngine.CoreModule", "UnityEngine", "GameObject")
        object_type = api("class_get_type", "void *", "void *", game_object)
        reflection_type = api("type_get_object", "void *", "void *", object_type)
        asset_name = managed_string("assets/workspace/prefabs/turnbattle/field/field01.prefab")
        prefab = engine("AssetBundle::LoadAsset_Internal", "void *", "void *,void *,void *",
                        f"{bundle},{asset_name},{reflection_type}")
        assert prefab, "Unity could not load field01.prefab as a GameObject"
        clone = engine("Object::Internal_CloneSingle", "void *", "void *", prefab)
        assert clone and clone != prefab, "Unity failed to instantiate the battle field"
        transform = engine("GameObject::get_transform", "void *", "void *", clone)
        children = engine("Transform::GetChildCount", "int", "void *", transform)
        assert children > 0, "The battle field instance has no children"
        print(f"PASS native Unity instantiated turn-battle field01 with {children} children", flush=True)
        engine("Object::DestroyImmediate", "void", "void *,unsigned char", f"{clone},0")
        engine("AssetBundle::Unload", "void", "void *,unsigned char", f"{bundle},1")
        calculator = klass("Assembly-CSharp", "Dark.Game.TurnBattle", "CalculatorMath")
        # Reference values from the ARM64 GetBaseDamage instruction sequence.
        # It selects min(attack, defense) and rounds each operation to float32.
        for attack, defense, expected in ((50000, 428400, 10175), (50000, 50000, 44275),
                                           (50000, 10000, 107804), (10000000, 1, 300000)):
            process.write_memory(scratch + 80, struct.pack("<ii", attack, defense))
            boxed = invoke(calculator, "GetBaseDamage", 0, [scratch + 80, scratch + 84])
            assert boxed
            address = api("object_unbox", "void *", "void *", boxed)
            damage, = struct.unpack("<i", process.read_memory(address, 4))
            assert damage == expected, (attack, defense, damage, expected)
        print("PASS native turn-battle damage against high, equal, and low defense and at the cap", flush=True)
    raw = storybook.read_bytes()
    array = api("array_new", "void *", "void *,unsigned long", f"{byte},{len(raw)}")
    pins = [api("gchandle_new", "unsigned", "void *,unsigned char", f"{array},1")]
    try:
        stream = api("object_new", "void *", "void *", memory_stream)
        pins.append(api("gchandle_new", "unsigned", "void *,unsigned char", f"{stream},1"))
        header = api("array_object_header_size", "unsigned", "void")
        process.write_memory(array + header, raw)
        # Use the five-argument ctor. The one-argument overload also accepts
        # a capacity and IL2CPP's name/arity lookup cannot distinguish them.
        process.write_memory(scratch + 80, struct.pack("<iiii", 0, len(raw), 0, 1))
        invoke(memory_stream, ".ctor", stream, [array, scratch + 80, scratch + 84, scratch + 88, scratch + 92])
        check_request(invoke(asset, "LoadFromStreamAsync", 0, [stream]), "pb_ma0100001.unity")
        position = invoke(memory_stream, "get_Position", stream, [])
        assert pointer(api("object_unbox", "void *", "void *", position)) == 0, "stream position changed"
    finally:
        for pin in pins:
            api("gchandle_free", "void", "unsigned", pin)
        call("&free", "void", "void *", scratch)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("field", type=Path, help="original decrypted tw002001 UnityFS bundle")
    parser.add_argument("storybook", type=Path, help="original decrypted pb_ma0100001 UnityFS bundle")
    parser.add_argument("--battle-field", type=Path, help="original decrypted turnbattle/field/field01 UnityFS bundle")
    parser.add_argument("--check-streaming", action="store_true", help="check the packaged icon through the full game's UnityWebRequest")
    parser.add_argument("--check-download", action="store_true", help="verify a real Octo download from a loopback fixture server")
    parser.add_argument("--activate-scenes", action="store_true", help="additively load both scenes and verify their root GameObjects")
    args = parser.parse_args()
    for path in filter(None, (args.field, args.storybook, args.battle_field)):
        with path.open("rb") as stream:
            if stream.read(8) != b"UnityFS\0":
                parser.error(f"Expected a decrypted UnityFS bundle at {path}")
    stage = args.stage.resolve()
    with tempfile.TemporaryDirectory() as work:
        config = Path(work)
        alsa = config / "asound.conf"
        alsa.write_text("pcm.!default { type null }\n")
        environment = dict(os.environ, XDG_CONFIG_HOME=work, XDG_CACHE_HOME=work,
                           PULSE_SERVER=f"unix:{work}/no-pulse", ALSA_CONFIG_PATH=str(alsa),
                           REINCOMP_TEST_BUNDLES=json.dumps([str(p.resolve()) for p in (args.field, args.storybook)]),
                           REINCOMP_TEST_BATTLE_FIELD=str(args.battle_field.resolve()) if args.battle_field else "",
                           REINCOMP_TEST_STREAMING="1" if args.check_streaming else "0",
                           REINCOMP_TEST_ACTIVATE="1" if args.activate_scenes else "0")
        script = json.dumps(str(Path(__file__).resolve()))
        command = ["gdb", "-q", "-nx", "-batch", "-ex",
                   f"python import runpy; runpy.run_path({script})['probe']()",
                   "--args", str(stage / "Reincarnation.x86_64"), "-batchmode", "-nographics",
                   "-logFile", str(config / "unity.log")]
        server = None
        if args.check_download:
            class Fixture(BaseHTTPRequestHandler):
                def do_GET(self):
                    if self.path != "/asset":
                        self.send_error(404)
                        return
                    data = b"Reincomp native Octo test\n" * 1024
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)

                def log_message(self, *args):
                    pass

            server = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
            port = server.server_port
            environment.update(TZ="Europe/Berlin", REINCOMP_BUNKER_PORT_OFFSET=str(port - 8080),
                               REINCOMP_TEST_DOWNLOAD_URL=f"http://127.0.0.1:{port}/asset")
            threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            subprocess.run(command, cwd=stage, env=environment, check=True, timeout=180)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            log = config / "unity.log"
            if log.is_file():
                print("\n".join(log.read_text(errors="replace").splitlines()[-15:]))
            raise
        finally:
            if server:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
