"""Combine prepared browser tools, link inputs and player dependencies for Bunker's build_site.py."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
from zipfile import ZIP_DEFLATED, ZipFile

HERE = Path(__file__).resolve().parent
import sys
sys.path.insert(0, str(HERE))
from check_player import check_kit
ROOT = HERE.parents[1]


def assemble(a):
    if a.out.exists() and any(a.out.iterdir()):
        raise ValueError('Use an empty directory for the public compiler kit')
    a.out.mkdir(parents=True,exist_ok=True)
    files={}
    def put(source, name=None):
        name=name or source.name
        dest=a.out/name
        dest.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source,dest)
        with dest.open('rb') as f:
            files[name]=hashlib.file_digest(f,'sha256').hexdigest()
    for name in ('compiler.mjs','build-objects.mjs','compile-library.mjs','toolchain.mjs','translate.mjs',
                 'apk.mjs','shard-worker.mjs','gnu-link.mjs','player-methods.mjs','liveness-edge.mjs',
                 'package-exe.mjs','master-worker.js','master_digest.py'):
        put(HERE/name)
    for name in ('lift','poll','coff'):
        for suffix in ('.mjs','.wasm'):put(a.tools/(name+suffix))
    put(a.gnu/'gnu-link.mjs','binutils.mjs')
    put(a.gnu/'gnu-link.wasm')
    put(a.gnu.parents[1]/'binutils-2.46.0.tar.xz','sources/binutils-2.46.0.tar.xz')
    put(HERE/'build-gnu-linker.sh','sources/build-gnu-linker.sh')
    put(a.semantics,'aarch64.bc')
    put(a.plan/'spring_group.entries')
    put(a.bunker/'android/app/src/main/python/patcher/patch_masterdata.py')
    for name in ('game-plan.json','heads-il2cpp.txt','heads-unity.txt'):put(a.plan/name)
    link=json.loads((a.link/'runtime-link.json').read_text())
    put(a.link/'runtime-link.json','link/runtime-link.json')
    for name,expected in link['files'].items():
        if Path(name).name!=name or not name.endswith(('.o','.a')):raise ValueError('Invalid adapter or MinGW library')
        put(a.link/name,'link/'+name)
        if files['link/'+name]!=expected:raise ValueError('Changed link input '+name)
    player=json.loads((a.player/'player.json').read_text())
    put(a.player/'player.json')
    for n,(name,expected) in enumerate(player['parts'],1):
        if name!=f'player.part{n:02d}':raise ValueError('Invalid player part')
        put(a.player/name)
        if files[name]!=expected:raise ValueError('Changed player part '+name)
    # A recipe from another build of the adapter makes every first launch fail.
    check_kit(a.out)
    for name in ('pyodide.js','pyodide.asm.js','pyodide.asm.wasm','pyodide-lock.json','python_stdlib.zip'):
        put(a.pyodide/name,'pyodide/'+name)
    lock=json.loads((a.pyodide/'pyodide-lock.json').read_text())
    for pkg in ('lz4','msgpack','pycryptodome'):
        entry=lock['packages'][pkg]
        name=entry['file_name']
        put(a.pyodide/name,'pyodide/'+name)
        if files['pyodide/'+name]!=entry['sha256']:raise ValueError('Unexpected Pyodide package '+name)
    version = (a.bunker / 'VERSION').read_text().strip()
    reincomp = (ROOT/'launcher/VERSION').read_text().strip()
    (a.out/'kit.json').write_text(json.dumps({'format':1,'preparedRuntimeFormat':1,'bunkerVersion':version,'reincompVersion':reincomp,'files':files},sort_keys=True))
    if a.archive:
        with ZipFile(a.archive,'x',compression=ZIP_DEFLATED,compresslevel=5) as bundle:
            for name in sorted(['kit.json', *files]):
                bundle.write(a.out/name, name)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for option in ('bunker','tools','gnu','semantics','plan','link','player','pyodide','out'):
        p.add_argument('--'+option,required=True,type=Path)
    p.add_argument('--archive',type=Path,help='Also write a public compiler ZIP for the matching Bunker release')
    assemble(p.parse_args())
