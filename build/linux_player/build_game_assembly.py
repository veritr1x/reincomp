"""Build the Linux Unity IL2CPP adapter from the supported original game image."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from stage import IL2CPP_SHA256
from engine_calls import registrations

ROOT = Path(__file__).resolve().parents[2]
BIAS = 0x80000000
sys.path.insert(0, str(ROOT / "build/native_player"))
from generate_abi import ArmImage


def build(image_path, unity, tools, out, heads, jobs):
    if jobs < 1:
        raise ValueError("--jobs must be positive")
    image = image_path.read_bytes()
    if hashlib.sha256(image).hexdigest() != IL2CPP_SHA256:
        raise ValueError("Expected the Global 3.7.1 ARM64 libil2cpp.so")
    arm = ArmImage(image)
    with (unity / "UnityPlayer_s.debug").open("rb") as src:
        if hashlib.file_digest(src, "sha256").hexdigest() != "dc554c57525f77f0f359947f17d109c76d3a81384e6e0d44d58ffed5f1127655":
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
        pcs = (int(pc, 16) for pc in (ROOT / "game/heads-il2cpp.txt").read_text().split())
        heads = out / "heads.txt"
        heads.write_text("".join(f"{pc:#x}\n" for pc in pcs
                                 if 0x820ca440 <= pc < 0x820cdb00 or 0x8254d000 <= pc < 0x82631000))
    def compile_heads(source, traces, options):
        subprocess.run([sys.executable, ROOT / "recompiler/lift_all.py", "--lib", image_path,
                        "--bias", hex(BIAS), "--heads", source, "--toolchain", tools,
                        "--poll", tools / "poll-instrument", "--jobs", str(jobs), "--shard-size", "64",
                        "--target", "x86_64-linux-gnu", "--out", traces,
                        *options], check=True)
        manifest = json.loads((traces / "result.json").read_bytes())
        return [traces / name for name in manifest["objects"]], set(manifest.get("unlinked", []))

    boundaries = ["--trace-boundaries", ROOT / "game/heads-il2cpp.txt"]
    objects, unresolved = compile_heads(heads,out / "traces",[*boundaries, "--entry-only"] if partial else [])
    if partial:
        extra, references = compile_heads(Path(__file__).with_name("bootstrap-callbacks.txt"),out / "callbacks",
                                          [*boundaries, "--linked-heads", heads])
        objects.extend(extra)
        unresolved.update(references)
    entries = sorted(pc for obj in objects for pc in json.loads(obj.with_suffix(".done").read_bytes())["defs"])
    def generate(name, contents):
        path = out / name
        if not path.exists() or path.read_text() != contents:
            path.write_text(contents)

    generate("game-traces.inc", "".join(f"TRACE({pc:x})\n" for pc in entries))
    generate("unlinked.inc", "".join(f"UNLINKED({pc:x})\n" for pc in sorted(unresolved.difference(entries))))
    generate("engine-names.inc","".join(json.dumps(name)+",\n" for name in sorted(registrations(unity))))
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
    args = parser.parse_args()
    build(args.lib, args.unity, args.toolchain.resolve(), args.out.resolve(), args.heads, args.jobs)
