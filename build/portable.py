"""Reproduce the portable Global Windows EXE from the APK, master and Unity player."""
import argparse
import os
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE/'native_player')]
from generate_abi import generate
from native_release import MASTER_HASH
from portable_host import build_go, build_native, dependencies, prepare_site, public_bunker, run
from portable_inputs import PORTABLE, digest, prepare


def executable(name, fallback=None):
    return shutil.which(name) or fallback


def main():
    unity = Path(os.environ.get('ProgramFiles','C:/Program Files'))/'Unity/Hub/Editor/2019.4.29f1/Editor/Data/PlaybackEngines/WindowsStandaloneSupport/Variations/win64_nondevelopment_il2cpp'
    edge = Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Microsoft/Edge/Application/msedge.exe'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apk',type=Path,required=True)
    parser.add_argument('--master',type=Path,required=True)
    parser.add_argument('--unity-player',type=Path,default=unity,help='Unity 2019.4.29f1 win64_nondevelopment_il2cpp directory')
    parser.add_argument('--unity-symbols',type=Path,help='UnityPlayer_Win64_il2cpp_x64.pdb if installed separately')
    parser.add_argument('--out',type=Path,default=Path('build-output/portable'))
    parser.add_argument('--llvm',type=Path,default=Path('C:/Program Files/LLVM/bin'),help='directory containing llvm-pdbutil and llvm-undname')
    parser.add_argument('--cxx',type=Path,default=executable('g++','C:/mingw64/bin/g++.exe'))
    parser.add_argument('--go',type=Path,default=executable('go','C:/Program Files/Go/bin/go.exe'))
    parser.add_argument('--node',type=Path,default=executable('node','C:/Program Files/nodejs/node.exe'))
    parser.add_argument('--edge',type=Path,default=edge)
    parser.add_argument('--jobs',type=int,default=min(8,os.cpu_count() or 2))
    a = parser.parse_args()
    if os.name != 'nt' or sys.maxsize <= 2**32 or sys.version_info < (3,12):
        parser.error('Use 64-bit Windows Python 3.12 or newer')
    if not 1 <= a.jobs <= 64:
        parser.error('--jobs must be between 1 and 64')
    for name in ('apk','master','unity_player','llvm','cxx','go','node','edge','unity_symbols'):
        value = getattr(a,name)
        if value:
            if not value.exists():
                parser.error(f'Missing {name}: {value}')
            setattr(a,name,value.resolve())
    if digest(a.master) != MASTER_HASH:
        parser.error('Expected the original Global master 20240404193219.bin.e')
    out = a.out.resolve()
    out.mkdir(parents=True,exist_ok=True)
    work = out/'work'
    target = out/'Nier Reincarnation.exe'
    if target.exists():
        parser.error('Output EXE already exists. Select a new --out directory.')
    abi = work/'generated/abi'
    generate(a.apk,a.unity_player,a.llvm,abi,a.unity_symbols)
    site = prepare(work)
    windows = site/'windows'
    bunker = public_bunker(work)
    release = work/'release'
    dependencies(windows,release)
    build_go(bunker,work,a.go,release/'game')
    build_native(a,work,bunker,release,abi)
    prepare_site(a,work,windows,bunker,release)
    run(a.node,HERE/'portable_web.cjs',site,a.apk,a.master,target,a.edge,a.jobs,PORTABLE['sha256'])
    print(target)


if __name__=='__main__':
    main()
