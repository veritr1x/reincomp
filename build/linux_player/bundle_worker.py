"""Convert UnityFS streams for the Linux player over its private socket."""
import hashlib
from pathlib import Path
import struct
import sys
from tempfile import TemporaryDirectory

from desktop_bundles import LINUX64, convert_bundle, use_fast_lz4


def serve(source, replies, cache):
    cache.mkdir(parents=True, exist_ok=True)
    while header := source.read(12):
        if len(header) != 12:
            return
        size, crc = struct.unpack('<QI', header)
        if not 0 < size <= 256 << 20:
            raise ValueError('Invalid bundle stream size')
        raw = source.read(size)
        if len(raw) != size:
            return
        try:
            key = f'{hashlib.sha256(raw).hexdigest()}-{crc:08x}.linuxbundle'
            dest = cache / key
            if not dest.is_file():
                with TemporaryDirectory(dir=cache) as work:
                    pending = Path(work) / key
                    pending.write_bytes(convert_bundle(raw, crc, target=LINUX64))
                    pending.replace(dest)
            message, error = bytes(dest), 0
        except Exception as exc:
            message, error = str(exc).encode('utf-8', errors='replace')[:4096], 1
        replies.write(struct.pack('<II', error, len(message)) + message)
        replies.flush()


if __name__ == '__main__':
    use_fast_lz4()
    serve(sys.stdin.buffer, sys.stdout.buffer, Path(sys.argv[1]).resolve())
