"""Prepare a private build tree from the same pinned source and patches as Windows."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'windows'))
import build_toolchain as native


def prepare(source, work):
    patches = sorted((native.ROOT / 'recompiler/remill').glob('*.patch'))
    key = hashlib.sha256(native.REMILL[1].encode())
    for patch in patches:
        key.update(patch.name.encode() + b'\0' + patch.read_bytes())
    dest = work / ('remill-' + key.hexdigest()[:20])
    if not dest.exists():
        with tempfile.TemporaryDirectory(dir=work) as tmp:
            tmp = Path(tmp)
            tree = tmp / 'source'
            tree.mkdir()
            archive = tmp / 'source.tar'
            subprocess.run(['git', '-c', f'safe.directory={source}', '-C', source, 'archive',
                            '-o', archive, native.REMILL[1]], check=True)
            with tarfile.open(archive) as members:
                members.extractall(tree, filter='data')
            # Apply only to the exported copy, including when work sits under a checkout.
            env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(tmp))
            for patch in patches:
                subprocess.run(['git', 'apply', '--whitespace=nowarn', patch], cwd=tree, env=env, check=True)
            tree.rename(dest)
    generated = work / 'generated'
    native.generate_lifter_sources(dest, generated)
    paths = [str(p) for p in native.lifter_sources(dest, generated)]
    (generated / 'sources.cmake').write_text(
        'set(REMILL_ROOT "' + dest.as_posix() + '")\nset(LIFTER_SOURCES\n' +
        ''.join(f'  "{p}"\n' for p in paths) + ')\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('work', type=Path)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    prepare(args.source.resolve(), args.work.resolve())
