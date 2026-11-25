"""Seed the Global Windows player's Octo cache from its verified Android resource library."""
import hashlib
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import struct
import sys
import tempfile
import time


def varint(data, offset):
    value = 0
    for shift in range(0, 70, 7):
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
    raise ValueError("Invalid Octo catalog varint")


def fields(data):
    offset = 0
    while offset < len(data):
        tag, offset = varint(data, offset)
        wire = tag & 7
        if wire == 0:
            _, offset = varint(data, offset)
            continue
        if wire == 2:
            size, offset = varint(data, offset)
        elif wire in (1, 5):
            size = 8 if wire == 1 else 4
        else:
            raise ValueError("Invalid Octo catalog wire type")
        if size > len(data) - offset:
            raise ValueError("Truncated Octo catalog")
        if wire == 2:
            yield tag >> 3, data[offset:offset + size]
        offset += size


def catalog_entries(catalog):
    for kind, message in fields(catalog):
        if kind not in (2, 4):
            continue
        row = dict(fields(message))
        key, md5, name = row.get(11, b""), row.get(10, b""), row.get(3, b"")
        if not re.fullmatch(b"[A-Za-z0-9_-]{1,96}", key) or not re.fullmatch(b"[a-fA-F0-9]{32}", md5):
            continue
        parts = name.decode("utf-8").split(")")
        if any(not p or p in (".", "..") or re.search(r'[\\/:*?"<>|\x00-\x1f]', p) or
               p.endswith((" ", ".")) or PureWindowsPath(p).is_reserved() for p in parts):
            continue
        hashed = md5.decode().lower()
        if kind == 2:
            path = Path("assetbundle", *parts[:-1], parts[-1] + ".assetbundle")
            name = hashed
            prefix = b"A"
        else:
            path = Path("resources", *parts)
            name = hashed + (".mp4" if path.suffix.lower() == ".mp4" else "")
            prefix = b"R"
        object_dir = (prefix + key).hex().upper()
        yield path, Path(object_dir[-1], object_dir, name), hashed


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "md5").hexdigest()


def create_once(path, data):
    # Publish initialized metadata atomically. Octo owns it once it exists.
    if path.exists():
        return
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temp:
        temp.write(data)
    try:
        try:
            os.link(temp.name, path)
        except FileExistsError:
            pass
    finally:
        os.unlink(temp.name)


# Octo removes files it does not know from its cache, so the receipt for a
# finished check lives beside the launcher's other state.
RECEIPT = "octo-cache-linked-v1"
WORK = "reincomp-link-"


def identity(*paths):
    return repr([(st.st_dev, st.st_ino) for st in map(os.stat, paths)]).encode()


def linked_entry(source, dest):
    # Three reads settle an entry that an earlier check already linked.
    try:
        return os.path.samestat(os.stat(dest), os.stat(source)) and os.path.isfile(dest.parent / ".meta")
    except OSError:
        return False


def seed(data, assets):
    base = assets / "assets/revisions/0"
    catalogs = [(p.resolve(), (p / "list.bin").read_bytes()) for p in (base / "android", base)
                if (p / "list.bin").is_file()]
    if not catalogs:
        return
    cache = data / "storage/files/octo"
    if not cache.resolve().is_relative_to(data.resolve()):
        return
    cache.mkdir(parents=True, exist_ok=True)
    cache = cache.resolve()
    version = cache / "v"
    cache_version = b"\x04\x08\x02\x00"
    if version.exists() and version.read_bytes() != cache_version:
        return
    # The same catalog can be mounted from a different drive or replaced in place.
    # Include its filesystem identity so reimporting the dump revisits the links.
    fingerprint = hashlib.sha256()
    for folder, catalog in catalogs:
        st = (folder / "list.bin").stat()
        fingerprint.update(repr((str(folder), st.st_dev, st.st_ino)).encode())
        fingerprint.update(catalog)
    app = cache / "v1/301"
    if not app.resolve().is_relative_to(cache):
        return
    # A work folder stays behind when another process holds the catalog open.
    for stale in cache.glob(WORK + "*"):
        shutil.rmtree(stale, ignore_errors=True)
    stamp = data / RECEIPT
    token = fingerprint.digest()
    # A deleted or replaced cache gets new folders, which the receipt does not match.
    if version.is_file() and app.is_dir() and stamp.is_file() and stamp.read_bytes() == token + identity(version, app):
        return
    # Probe the actual mounted revision. A library on another volume uses the
    # ordinary Octo download without opening or hashing all its payloads.
    with tempfile.TemporaryDirectory(dir=cache, prefix=WORK, ignore_cleanup_errors=True) as work:
        available = []
        for folder, catalog in catalogs:
            try:
                os.link(folder / "list.bin", Path(work) / str(len(available)))
            except OSError:
                continue
            available.append((folder, catalog))
        if not available:
            return
        app.mkdir(parents=True, exist_ok=True)
        create_once(version, cache_version)
        # Octo's .meta is a little-endian DateTime.ToBinary() access time.
        ticks = 621355968000000000 + time.time_ns() // 100
        meta = struct.pack("<Q", ticks | (1 << 62))  # UTC
        linked = 0
        retry = False
        staged = Path(work) / "payload"
        # The launcher shows these whole percentages while a large library is hashed.
        total = sum(1 for _, catalog in available for _ in catalog_entries(catalog))
        done, shown = 0, -1
        for folder, catalog in available:
            for relative, key, md5 in catalog_entries(catalog):
                percent = done * 100 // total
                done += 1
                if percent != shown:
                    shown = percent
                    print(f"progress {percent}", flush=True)
                source, dest = folder / relative, app / key
                if linked_entry(source, dest):
                    continue
                try:
                    if not source.is_file():
                        continue
                    source = source.resolve()
                    if not source.is_relative_to(folder):
                        continue
                    # Keep writes inside the game's private cache when opening an old install.
                    if not dest.resolve().is_relative_to(cache):
                        continue
                    exists = dest.exists()
                    if exists and dest.samefile(source):
                        create_once(dest.parent / ".meta", meta)
                        continue
                    # Hash the link we are about to publish. A concurrent library
                    # reimport can replace the source pathname during verification.
                    os.link(source, staged)
                    try:
                        if digest(staged) != md5 or (exists and digest(dest) != md5):
                            continue
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        if exists:
                            os.replace(staged, dest)
                        else:
                            os.link(staged, dest)
                    finally:
                        staged.unlink(missing_ok=True)
                    create_once(dest.parent / ".meta", meta)
                    linked += 1
                except OSError as exc:
                    # Locked entries, missing variants and nested mounts retain Octo's fallback.
                    if getattr(exc, "winerror", None) in (32, 33):
                        retry = True
                    continue
        if total:
            print("progress 100", flush=True)
        if not retry:
            pending = Path(work) / "receipt"
            pending.write_bytes(token + identity(version, app))
            os.replace(pending, stamp)
        print(f"Linked {linked} verified Octo files", flush=True)


if __name__ == "__main__":
    seed(Path(sys.argv[1]), Path(sys.argv[2]))
