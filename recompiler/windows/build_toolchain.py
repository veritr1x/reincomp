#!/usr/bin/env python3
"""Build the recompiler's toolchain for Windows: the lifter, its semantics and the instrumenter.

    python recompiler/windows/build_toolchain.py --llvm LLVM18_DIR --work WORK_DIR --out OUT_DIR

LLVM18_DIR is an unpacked LLVM 18 release for Windows (the clang+llvm-18.1.8-
x86_64-pc-windows-msvc package). Run from a Visual Studio developer prompt, or
anywhere INCLUDE and LIB name the MSVC and Windows SDK folders; cmake, ninja
and git must be on PATH. WORK_DIR receives the sources this fetches (the
Remill fork at the pinned commit, gflags, glog) and the build files.

OUT_DIR gets remill-lift.exe, aarch64.bc and poll-instrument.exe. The lifter
is built for AArch64 only: the other architectures' decoders, and with them
XED and sleigh, are left out.
"""
import argparse, concurrent.futures, os, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
REMILL = ('https://github.com/veritr1x/remill.git', '3fd14f3f64d4ab37b36bd985d1b492cec6e1f1bb')
GFLAGS = ('https://github.com/gflags/gflags.git', 'v2.2.2')
GLOG = ('https://github.com/google/glog.git', 'v0.6.0')
OTHER_ARCHITECTURES = '''#include <remill/Arch/Arch.h>
#include <glog/logging.h>
namespace remill {
#define ABSENT(name) Arch::ArchPtr Arch::name(llvm::LLVMContext *, OSName, ArchName) { LOG(FATAL) << #name " is not part of this AArch64-only build"; return {}; }
ABSENT(GetX86) ABSENT(GetSleighX86) ABSENT(GetAArch32) ABSENT(GetSleighThumb2) ABSENT(GetAArch64Sleigh)
ABSENT(GetSPARC32) ABSENT(GetSPARC64) ABSENT(GetSPARC32Sleigh) ABSENT(GetSleighPPC)
}
'''
# SleighLifter.h is included by the trace lifter, but nothing of sleigh is used in this build.
SLEIGH_STAND_IN = '''#pragma once
#include <string>
#include <vector>
namespace ghidra {}
struct VarnodeData {}; struct Address {}; class Sleigh {}; class PcodeEmit {}; class AssemblyEmit {};
class ContextInternal {}; class DocumentStorage {}; class LoadImage {}; struct PcodeOpRaw {};
'''


def run(command, **options):
    r = subprocess.run([str(x) for x in command], capture_output=True, text=True, **options)
    if r.returncode:
        sys.exit(f'FAILED: {" ".join(str(x) for x in command[:3])} ...\n{(r.stdout + r.stderr)[-4000:]}')
    return r.stdout


def fetch(work, name, source):
    url, revision = source
    target = work / name
    if not target.exists():
        run(['git', 'clone', '-q', url, target])
    run(['git', '-C', target, 'checkout', '-q', '--detach', revision])
    return target


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--llvm', type=Path, required=True)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    llvm, work, out = a.llvm.resolve(), a.work.resolve(), a.out.resolve()
    # A Visual Studio newer than LLVM 18 refuses this clang in its headers unless told otherwise.
    os.environ['CL'] = (os.environ.get('CL', '') + ' /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH').strip()
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    clang_cl, clangxx = llvm / 'bin/clang-cl.exe', llvm / 'bin/clang++.exe'
    config = lambda *x: run([llvm / 'bin/llvm-config.exe', *x]).split()
    system = [x for x in config('--system-libs') if x != 'libxml2s.lib']  # listed by LLVM, used by none of this

    remill = fetch(work, 'remill', REMILL)
    run(['git', '-C', remill, 'checkout', '-q', '--', '.'])
    for patch in sorted((ROOT / 'recompiler/remill').glob('*.patch')):
        run(['git', '-C', remill, 'apply', patch])
    deps = work / 'deps'
    env = dict(os.environ, CC=str(clang_cl), CXX=str(clang_cl))
    for name, source, extra in (('gflags', GFLAGS, []), ('glog', GLOG, ['-DWITH_GTEST=OFF', '-DWITH_UNWIND=OFF', '-DWITH_GFLAGS=ON'])):
        if (deps / 'lib' / ('glog.lib' if name == 'glog' else 'gflags_static.lib')).exists():
            continue
        tree = fetch(work, name, source)
        run(['cmake', '-S', tree, '-B', work / f'build-{name}', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
             f'-DCMAKE_INSTALL_PREFIX={deps}', f'-DCMAKE_PREFIX_PATH={deps}', '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF',
             '-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON', '-DREGISTER_INSTALL_PREFIX=OFF', f'-DCMAKE_LINKER={llvm / "bin/lld-link.exe"}',
             '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL', '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
             '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', *extra], env=env)
        run(['cmake', '--build', work / f'build-{name}', '--target', 'install'], env=env)
    print('gflags and glog: built')

    # The semantics: Remill's AArch64 instruction bodies as LLVM bitcode, with the flags of its BCCompiler.cmake.
    sem = work / 'semantics'
    sem.mkdir(exist_ok=True)
    sysroot = remill / 'include/remill/Arch/Runtime/sysroot'
    common = [f'-I{remill / "include"}', f'-I{remill}', '-target', 'aarch64-none-elf', '-DADDRESS_SIZE_BITS=64', '-DLITTLE_ENDIAN=1',
              f'--sysroot={sysroot}', '-nostdinc++', '-isystem', sysroot, '-emit-llvm', '-w', '-fno-vectorize', '-fno-slp-vectorize',
              '-ffreestanding', '-fno-common', '-fno-builtin', '-fno-exceptions', '-fno-rtti', '-fno-asynchronous-unwind-tables',
              '-std=c++17']
    units = (('i', 'lib/Arch/AArch64/Runtime/Instructions.cpp', ['-O3', '-g0']), ('b', 'lib/Arch/AArch64/Runtime/BasicBlock.cpp', ['-O0', '-g3']),
             ('n', 'lib/Arch/Runtime/Intrinsics.cpp', []), ('h', 'lib/Arch/Runtime/HyperCall.cpp', []))
    for name, source, level in units:
        run([clangxx, *common, *level, '-c', remill / source, '-o', sem / f'{name}.bc'])
    run([llvm / 'bin/llvm-link.exe', '--suppress-warnings', *[sem / f'{name}.bc' for name, _, _ in units], '-o', out / 'aarch64.bc'])
    print('semantics: aarch64.bc')

    # The lifter.
    build = work / 'build-lift'
    (build / 'include/sleigh').mkdir(parents=True, exist_ok=True)
    (build / 'include/sleigh/libsleigh.hh').write_text(SLEIGH_STAND_IN)
    (build / 'OtherArchitectures.cpp').write_text(OTHER_ARCHITECTURES)
    version = (remill / 'lib/Version/Version.cpp.in').read_text()
    version = version.replace('@GIT_RETRIEVED_STATE@', 'true').replace('@GIT_IS_DIRTY@', 'false').replace('@GIT_HEAD_SHA1@', REMILL[1])
    (build / 'Version.cpp').write_text(re.sub(r'@[A-Z_0-9]+@', '', version))
    sources = [remill / 'lib/Arch' / n for n in ('Arch.cpp', 'Context.cpp', 'Instruction.cpp', 'Name.cpp')]
    sources += [remill / 'lib/Arch/AArch64' / n for n in ('Arch.cpp', 'Decode.cpp', 'Extract.cpp')]
    sources += [remill / 'lib/Arch/Sleigh/AArch64Base.cpp']
    sources += [p for p in sorted((remill / 'lib/BC').glob('*.cpp')) if p.name not in ('SleighLifter.cpp', 'PcodeCFG.cpp')]
    sources += sorted((remill / 'lib/OS').glob('*.cpp'))
    sources += [remill / 'bin/lift/Lift.cpp', remill / 'bin/lift/GuestCallLowering.cpp', build / 'Version.cpp', build / 'OtherArchitectures.cpp']
    flags = ['/nologo', '/std:c++20', '/O2', '/EHsc', '/MD', '/GR', '/w', f'/I{remill / "include"}', f'/I{remill}', f'/I{deps / "include"}',
             f'/I{llvm / "include"}', f'/I{build / "include"}', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN', '/DNDEBUG',
             '/DGLOG_NO_ABBREVIATED_SEVERITIES', '/DGFLAGS_IS_A_DLL=0', '/DGLOG_STATIC_DEFINE', '/DGOOGLE_GLOG_DLL_DECL=',
             '/DREMILL_INSTALL_SEMANTICS_DIR=""', *[f'/DREMILL_BUILD_SEMANTICS_DIR_{n}=""' for n in
                                                    ('X86', 'AARCH32', 'AARCH64', 'SPARC32', 'SPARC64', 'PPC64_32ADDR')]]

    def compile_one(source):
        obj = build / f'{source.parent.name}-{source.stem}.obj'
        run([clang_cl, *flags, '/c', source, f'/Fo{obj}'])
        return obj

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        objects = list(pool.map(compile_one, sources))
    libs = config('--libs', 'core', 'irreader', 'bitreader', 'bitwriter', 'linker', 'passes', 'transformutils', 'support')
    run([llvm / 'bin/lld-link.exe', '/nologo', f'/OUT:{out / "remill-lift.exe"}', *objects, deps / 'lib/glog.lib', deps / 'lib/gflags_static.lib',
         f'/LIBPATH:{llvm / "lib"}', *libs, *system, 'shlwapi.lib', 'dbghelp.lib', 'msvcrt.lib', 'msvcprt.lib'])
    print('lifter: remill-lift.exe')

    # The instrumenter.
    run([clang_cl, '/nologo', '/std:c++17', '/O2', '/EHsc', '/MD', '/w', f'/I{llvm / "include"}', '/D_CRT_SECURE_NO_WARNINGS', '/DNOMINMAX',
         '-fuse-ld=lld', ROOT / 'recompiler/poll_instrument.cpp', f'/Fo{build / "poll_instrument.obj"}', f'/Fe{out / "poll-instrument.exe"}',
         '/link', f'/LIBPATH:{llvm / "lib"}', *config('--libs', 'core', 'irreader'), *system])
    print('instrumenter: poll-instrument.exe')
    shutil.copyfile(remill / 'LICENSE', out / 'LICENSE.remill.txt')
    print(out)


if __name__ == '__main__':
    main()
