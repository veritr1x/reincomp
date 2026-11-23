"""Build the pinned ARM64 lifter, instruction semantics and instrumenter on Linux."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shlex
import subprocess
import sys
from tempfile import TemporaryDirectory

RECOMPILER = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(RECOMPILER))
from windows.build_toolchain import REMILL, build_semantics, generate_lifter_sources, lifter_sources


def build(out, source, jobs):
    if out.exists():
        raise FileExistsError(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    config = "llvm-config-18"
    llvm = Path(subprocess.check_output([config, "--bindir"], text=True).strip())
    clang = llvm / "clang++"
    with TemporaryDirectory(dir=out.parent) as tmp:
        work = Path(tmp).resolve()
        tree, package = work / "remill", work / "toolchain"
        package.mkdir()
        subprocess.run(["git", "clone", "--quiet", "--no-checkout", source, tree], check=True)
        subprocess.run(["git", "-C", tree, "checkout", "--quiet", "--detach", REMILL[1]], check=True)
        for patch in sorted((RECOMPILER / "remill").glob("*.patch")):
            subprocess.run(["git", "-C", tree, "apply", patch], check=True)
        build_semantics(tree, work / "semantics", clang, llvm / "llvm-link", package / "aarch64.bc")
        generate_lifter_sources(tree, work)
        flags = shlex.split(subprocess.check_output([config, "--cppflags"], text=True))
        flags += ["-std=c++20", "-O2", "-DNDEBUG", f"-I{tree / 'include'}", f"-I{tree}", f"-I{work / 'include'}",
                  '-DREMILL_INSTALL_SEMANTICS_DIR=""']
        flags += [f'-DREMILL_BUILD_SEMANTICS_DIR_{arch}=""' for arch in
                  ("X86", "AARCH32", "AARCH64", "SPARC32", "SPARC64", "PPC64_32ADDR")]

        def compile_one(item):
            index, unit = item
            obj = work / f"lift-{index}.o"
            subprocess.run([clang, *flags, "-c", unit, "-o", obj], check=True)
            return obj

        with ThreadPoolExecutor(jobs) as pool:
            objects = list(pool.map(compile_one, enumerate(lifter_sources(tree, work))))
        libs = shlex.split(subprocess.check_output([config, "--ldflags", "--libs", "--system-libs"], text=True))
        subprocess.run([clang, *objects, *libs, "-lglog", "-lgflags", "-pthread", "-o", package / "remill-lift"], check=True)
        subprocess.run([sys.executable, RECOMPILER / "build_poll_instrument.py", "--out", package / "poll-instrument"], check=True)
        package.rename(out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path, help="new toolchain directory")
    parser.add_argument("--remill", default=REMILL[0], help="repository URL or local clone containing the pinned revision")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    build(args.out.resolve(), args.remill, args.jobs)
