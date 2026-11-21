"""Prepare a browser runtime pack from a previously verified Windows build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import tempfile


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def runtime_member(stream, length):
    stream.seek(-28, 2)
    magic, manifest_size, payload_size = struct.unpack('<16sIQ', stream.read(28))
    if (magic != b'BUNKER.WINDOWS.1' or not 0 < manifest_size <= 32 * 1024**2
            or manifest_size + payload_size + 28 >= length):
        raise ValueError('Invalid portable Windows package')
    manifest_start = length - 28 - manifest_size
    stream.seek(manifest_start)
    manifest = json.loads(stream.read(manifest_size))
    offset = manifest_start - payload_size
    runtime = None
    for entry in manifest['files']:
        stored = entry.get('storedSize', entry['size'])
        if not isinstance(stored, int) or stored < 0 or offset + stored > manifest_start:
            raise ValueError('Invalid package member size')
        if entry['path'] == 'game/GuestRuntime.dll':
            if runtime or entry.get('compression') != 'gzip' or not stored:
                raise ValueError('Expected one gzip-compressed guest runtime')
            size, chunks = entry['size'], entry['chunks']
            if (not isinstance(size, int) or size <= 0 or not isinstance(chunks, list)
                    or len(chunks) != (size + 4 * 1024**2 - 1) // (4 * 1024**2)
                    or not all(isinstance(h, str) and re.fullmatch('[0-9a-f]{64}', h) for h in chunks)):
                raise ValueError('Invalid guest runtime checksums')
            runtime = offset, stored, size, chunks
        offset += stored
    if runtime is None or offset != manifest_start:
        raise ValueError('Incomplete Windows package')
    return runtime


def prepare(exe, expected_exe, kit, output):
    if json.loads(kit.read_bytes()).get('preparedRuntimeFormat') != 1:
        raise ValueError('Rebuild the compiler kit with prepared runtime support')
    # The digest identifies a build that has been accepted in gameplay, including
    # its translator, linked native adapters and original Global inputs.
    if sha256(exe) != expected_exe.lower():
        raise ValueError('The source EXE differs from the verified build')
    if output.exists():
        raise ValueError('Choose a new runtime pack directory')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as work:
        stage = Path(work) / 'runtime'
        stage.mkdir()
        parts = []
        with exe.open('rb') as src:
            offset, remaining, size, chunks = runtime_member(src, exe.stat().st_size)
            src.seek(offset)
            while remaining:
                data = src.read(min(32 * 1024**2, remaining))
                if not data:
                    raise ValueError('Truncated guest runtime')
                (stage / f'runtime.part{len(parts) + 1:03}').write_bytes(data)
                parts.append({'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
                remaining -= len(data)
        info = {'format': 1, 'sourceExeSHA256': expected_exe, 'kitSHA256': sha256(kit),
                'size': size, 'chunks': chunks, 'parts': parts}
        manifest = stage / 'runtime.json'
        manifest.write_bytes(json.dumps(info, separators=(',', ':')).encode())
        digest = sha256(manifest)
        stage.rename(output)
    return digest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--exe-sha256', required=True, help='SHA-256 of the tested EXE built with this kit')
    parser.add_argument('--kit', type=Path, required=True, help='kit.json used to build the tested EXE')
    parser.add_argument('--out', type=Path, required=True, help='New private directory outside public release artifacts')
    args = parser.parse_args()
    print(prepare(args.exe, args.exe_sha256, args.kit, args.out), flush=True)
