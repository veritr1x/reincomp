"""Build the Linux Unity IL2CPP adapter from the supported original game image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

from stage import IL2CPP_SHA256, UNITY_SYMBOLS_SHA256
from engine_calls import registrations

ROOT = Path(__file__).resolve().parents[2]
BIAS = 0x80000000
sys.path.insert(0, str(ROOT / "build/native_player"))
from generate_abi import ArmImage


def relocated_pointers(arm, slots):
    result = {}
    for section in arm.sections:
        if section[1] != 4:  # SHT_RELA
            continue
        start, size = section[4:6]
        for at, kind, value in struct.iter_unpack("<QQq", memoryview(arm.data)[start:start + size]):
            if kind == 1027 and at in slots:  # R_AARCH64_RELATIVE, symbol zero
                result[at] = value
    return result


def managed_modules(arm):
    # Il2CppCodeGenModule registrations in the pinned Global ELF. The original
    # relocation addends identify each managed method before the image is loaded.
    expected = {
        0x7230bb8: ("mscorlib.dll", 11443),
        0x7235a20: ("UnityEngine.CoreModule.dll", 3231),
        0x7235cc0: ("UnityEngine.UI.dll", 1650),
        0x7235ec8: ("Cinemachine.dll", 915),
        0x7235f48: ("UnityEngine.Purchasing.Stores.dll", 848),
        0x7235fb0: ("UniRx.Async.dll", 798),
        0x72360e8: ("Unity.Postprocessing.Runtime.dll", 615),
        0x72361b8: ("UnityEngine.IMGUIModule.dll", 559),
        0x72362f0: ("Unity.RenderPipelines.Lightweight.Runtime.dll", 317),
        0x72363c0: ("UnityEngine.Purchasing.dll", 298),
        0x7236530: ("Unity.Services.Analytics.dll", 259),
        0x72366d0: ("Unity.Services.Core.Internal.dll", 173),
        0x7237580: ("UnityEngine.UIModule.dll", 103),
        0x72376b8: ("ArtCore.dll", 88),
        0x7237c00: ("UnityEngine.SharedInternalsModule.dll", 67),
        0x7237cd0: ("Unity.RenderPipelines.Core.Runtime.dll", 53),
        0x7237e90: ("UnityEngine.TextRenderingModule.dll", 49),
        0x7237f60: ("UnityEngine.InputLegacyModule.dll", 53),
        0x7238490: ("UnityEngine.AssetBundleModule.dll", 22),
        0x72389a8: ("Unity.Services.Core.dll", 15),
    }

    fields = relocated_pointers(arm, {module + offset for module in expected for offset in (0, 16, 32, 40)})
    owners = {}
    invoker_indices, adjustor_slots = set(), set()
    for module, (name, count) in expected.items():
        pos = arm.offset(fields[module])
        if not arm.data.startswith(name.encode() + b"\0", pos) or arm.word(module + 8) != count:
            raise ValueError(f"Unexpected {name} registration in the original IL2CPP image")
        table = fields[module + 16]
        owners.update((table + 8 * index, name) for index in range(count))
        invoker_indices.update(struct.unpack_from(f"<{count}i", arm.data, arm.offset(fields[module + 40])))
        # An adjustor thunk accepts a boxed value type and passes its payload
        # to the original instance method. The pairs are (metadata token, PC).
        for index in range(arm.word(module + 24)):
            adjustor_slots.add(fields[module + 32] + 16 * index + 8)
    groups = {name: set() for name, _ in expected.values()}
    for slot, pc in relocated_pointers(arm, owners).items():
        if pc:
            groups[owners[slot]].add(BIAS + pc)
    code = 0x7237e08
    invoker_count = arm.word(code + 40)
    invokers = relocated_pointers(arm, {code + 48})[code + 48]
    invoker_indices.discard(-1)
    if invoker_count != 12168 or not all(0 <= i < invoker_count for i in invoker_indices):
        raise ValueError("Invalid IL2CPP runtime invoker registration")
    dispatch_slots = adjustor_slots | {invokers + 8 * i for i in invoker_indices}
    dispatch = relocated_pointers(arm, dispatch_slots)
    if dispatch.keys() != dispatch_slots:
        raise ValueError("Missing IL2CPP runtime invoker or value-type adjustor")
    return groups, {BIAS + pc for pc in dispatch.values()}


def generic_variant_groups(arm, groups):
    # The game can select a different List<T> or Dictionary<K,V> body for the
    # same managed method depending on which assets Unity deserializes.
    code, metadata = 0x7237e08, 0x7239230
    fields = relocated_pointers(arm, {code + 0x18, metadata + 0x28, metadata + 0x48})
    count = arm.word(code + 0x10)
    function_count, spec_count = arm.word(metadata + 0x20), arm.word(metadata + 0x40)
    if (count, function_count, spec_count) != (82112, 116310, 121746):
        raise ValueError("Unexpected IL2CPP generic method registration")
    table = fields[code + 0x18]
    targets = relocated_pointers(arm, range(table, table + 8 * count, 8))
    functions = arm.offset(fields[metadata + 0x28])
    specs = arm.offset(fields[metadata + 0x48])
    variants = {}
    for index in range(function_count):
        spec_index, pointer_index = struct.unpack_from("<ii", arm.data, functions + 16 * index)
        if not 0 <= spec_index < spec_count or not -1 <= pointer_index < count:
            raise ValueError("Invalid generic method index in the supported game")
        entry = targets.get(table + 8 * pointer_index)
        if entry:
            definition, = struct.unpack_from("<i", arm.data, specs + 12 * spec_index)
            variants.setdefault(definition, set()).add(BIAS + entry)
    return [{pc for family in variants.values() if not family.isdisjoint(roots) for pc in family}
            for roots in groups]


def first_scene_scripts(arm):
    # Zero-based MethodDef ranges for the Assembly-CSharp components in level0
    # (dark_gameplay). Four of its components load serialized Paradigm graphs,
    # which instantiate other node types and their nested delegates by token.
    # UI, post-processing and Cinemachine are already included as whole modules.
    components = {
        "RenderManager": (46283, 30), "TransitionPlayIn": (90104, 8),
        "CameraManager": (101054, 12), "PPVOverrider": (101067, 10),
        "Adam.Framework.Gameplay.Paradigm": (102601, 2592),
        "Adjust": (110940, 37), "AdjustPurchase": (111120, 6),
    }
    module = 0x6fd4938
    fields = relocated_pointers(arm, {module, module + 16})
    if arm.word(module + 8) != 111740 or not arm.data.startswith(b"Assembly-CSharp.dll\0", arm.offset(fields[module])):
        raise ValueError("Unexpected first-scene game assembly registration")
    table = fields[module + 16]
    slots = {table + 8 * index for start, count in components.values() for index in range(start, start + count)}
    return {BIAS + pc for pc in relocated_pointers(arm, slots).values() if pc}


def build(image_path, unity, tools, out, heads, jobs, scratch=None):
    if jobs < 1:
        raise ValueError("--jobs must be positive")
    image = image_path.read_bytes()
    if hashlib.sha256(image).hexdigest() != IL2CPP_SHA256:
        raise ValueError("Expected the Global 3.7.1 ARM64 libil2cpp.so")
    arm = ArmImage(image)
    with (unity / "UnityPlayer_s.debug").open("rb") as src:
        if hashlib.file_digest(src, "sha256").hexdigest() != UNITY_SYMBOLS_SHA256:
            raise ValueError("Expected Unity 2019.4.29f1 Linux IL2CPP symbols")
    symbols = subprocess.check_output(["nm", "--defined-only", unity / "UnityPlayer_s.debug"], text=True)
    names = sorted(re.findall(r"^[0-9a-f]+ b (il2cpp_\w+)$", symbols, re.M))
    exports = arm.exports()
    if len(names) != 222 or set(names) - exports.keys():
        raise ValueError("Linux Unity and the original IL2CPP export table do not match")
    init, = [section for section in arm.sections if section[1] == 14]  # SHT_INIT_ARRAY
    out.mkdir(parents=True, exist_ok=True)
    partial = not heads
    if partial:
        # VM, PLT, static constructors and bundled C++ library in the pinned binary.
        pcs = {int(pc, 16) for pc in (ROOT / "game/heads-il2cpp.txt").read_text().split()}
        heads = out / "heads.txt"
        bootstrap = {pc for pc in pcs if 0x820ca440 <= pc < 0x820cdb00 or 0x8254d000 <= pc < 0x82631000}
        def write_heads(path, entries):
            path.write_text("".join(f"{pc:#x}\n" for pc in sorted(entries)))

        write_heads(heads, bootstrap)

    def compile_heads(source, traces, options):
        subprocess.run([sys.executable, ROOT / "recompiler/lift_all.py", "--lib", image_path,
                        "--bias", hex(BIAS), "--heads", source, "--toolchain", tools,
                        "--poll", tools / "poll-instrument", "--jobs", str(jobs), "--shard-size", "64",
                        "--target", "x86_64-linux-gnu", "--out", traces, "--image-cache", (scratch or out) / "images",
                        "--patches", ROOT / "game/patches.json",
                        *(["--scratch",scratch] if scratch else []),
                        *options], check=True)
        manifest = json.loads((traces / "result.json").read_bytes())
        return [traces / name for name in manifest["objects"]], set(manifest.get("unlinked", []))

    boundaries = ["--trace-boundaries", ROOT / "game/heads-il2cpp.txt"]
    options = [*boundaries, "--entry-only"] if partial else []
    objects, unresolved = compile_heads(heads, out / "traces", options)
    if partial:
        modules, managed_dispatch = managed_modules(arm)
        covered = set(bootstrap)
        def compile_group(group, roots):
            roots = roots - covered
            if not roots:
                return set()
            if not roots <= pcs:
                raise ValueError(f"{group} contains an unknown ARM method entry")
            source = out / (group + "-heads.txt")
            write_heads(source, roots)
            extra, references = compile_heads(source, out / group, options)
            objects.extend(extra)
            unresolved.update(references)
            covered.update(roots)
            return references

        compile_group("core-library", modules.pop("mscorlib.dll"))
        asset_bundles = modules.pop("UnityEngine.AssetBundleModule.dll")
        ui_modules = ("UI", "IMGUIModule", "UIModule", "TextRenderingModule", "InputLegacyModule")
        user_interface = set().union(*(modules.pop(f"UnityEngine.{name}.dll") for name in ui_modules))
        compile_group("unity-managed", set.union(*modules.values()))
        # RuntimeInitializeOnLoadMethodAttribute entries from Global 3.7.1
        # metadata. Unity invokes these by metadata token, across assemblies.
        initializers = Path(__file__).with_name("scene-initializers.txt").read_text().splitlines()
        compile_group("scene-initializers", {int(row.split()[0], 16) for row in initializers})
        # Generic collection bodies and cross-module helpers called directly by
        # these modules are link dependencies even before Unity selects a scene.
        dependencies = (unresolved & pcs) - covered
        callbacks = {int(pc, 16) for pc in Path(__file__).with_name("bootstrap-callbacks.txt").read_text().split()}
        framework_generics, extra_generics = generic_variant_groups(arm, (dependencies, callbacks))
        compile_group("module-dependencies", dependencies)
        compile_group("module-generics", framework_generics)
        compile_group("generic-methods", extra_generics)
        compile_group("callbacks", callbacks)
        compile_group("unity-ui", user_interface)
        compile_group("asset-bundles", asset_bundles)
        compile_group("managed-dispatch", managed_dispatch)
        scene_references = compile_group("first-scene", first_scene_scripts(arm))
        compile_group("first-scene-dependencies", scene_references & pcs)
        scene_generics, = generic_variant_groups(arm, (scene_references,))
        compile_group("first-scene-generics", scene_generics)
    entries = sorted(pc for obj in objects for pc in json.loads(obj.with_suffix(".done").read_bytes())["defs"])
    def generate(name, contents):
        path = out / name
        if not path.exists() or path.read_text() != contents:
            path.write_text(contents)

    generate("game-traces.inc", "".join(f"TRACE({pc:x})\n" for pc in entries))
    generate("unlinked.inc", "".join(f"UNLINKED({pc:x})\n" for pc in sorted(unresolved.difference(entries))))
    generate("engine-names.inc", "".join(json.dumps(name) + ",\n" for name in sorted(registrations(unity))))
    generate("api.inc", "".join(f"API({name}, {BIAS + exports[name]:#x})\n" for name in names))
    generate("init.inc", f"constexpr uintptr_t initOffset = {init[3]:#x}, initCount = {init[5] // 8};\n")
    generate("objects.txt", "".join(str(obj) + "\n" for obj in objects))
    generate("exports.map", "{ global: " + "; ".join(names) + "; local: *; };\n")
    build_dir = out / "cmake"
    subprocess.run(["cmake", "-S", Path(__file__).resolve().parent, "-B", build_dir,
                    "-DGENERATED=" + str(out)], check=True)
    subprocess.run(["cmake", "--build", build_dir, "--parallel", str(jobs)], check=True)
    target = out / "GameAssembly.so"
    pending = out / "GameAssembly.so.tmp"
    shutil.copyfile(build_dir / target.name, pending)
    pending.replace(target)
    print(f"Built {target} with {len(names)} native Unity exports and {len(entries)} translated traces")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for arg in ("lib", "unity", "toolchain", "out"):
        parser.add_argument("--" + arg, type=Path, required=True)
    parser.add_argument("--heads", type=Path, help="original guest entry addresses to compile (default: VM bootstrap)")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--scratch", type=Path, help="store temporary LLVM IR and the shared input image on a fast local filesystem")
    args = parser.parse_args()
    import fcntl
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with (out / ".assembly.lock").open("a+b") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.exit(1, f"Another Linux assembly build is using {out}\n")
        build(args.lib, args.unity, args.toolchain.resolve(), out, args.heads, args.jobs, args.scratch)
