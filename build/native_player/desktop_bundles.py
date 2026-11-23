"""Prepare desktop UnityFS assets from the game's original, already-open UnityFS streams."""
import hashlib
from pathlib import Path
import os
import sys
import time
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
if extra_packages := os.environ.get('REINCOMP_ASSET_PYTHON'):
    sys.path.append(extra_packages)
import UnityPy
from UnityPy.export.Texture2DConverter import parse_image_data
from UnityPy.files import SerializedFile
from UnityPy.streams import EndianBinaryReader
from desktop_shader import convert as convert_shader

CACHE_DIRECTORY = 'desktop-bundles-v2'
WINDOWS64, LINUX64 = 19, 24


def convert_texture(texture):
    # Unity stores each face's complete mip chain before the next face.
    formats = {34: (4, 8), 45: (4, 8), 46: (4, 8), 47: (4, 16)}
    formats.update({i: (block, 16) for i, block in enumerate((4, 5, 6, 8, 10, 12), 48)})
    formats.update({i: (block, 16) for i, block in enumerate((4, 5, 6, 8, 10, 12), 54)})
    layout = formats.get(texture.m_TextureFormat)
    if not layout:
        return False
    cube = texture.object_reader.type.name == 'Cubemap'
    faces = 6 if cube else 1
    if texture.m_ImageCount != faces or not (texture.m_TextureDimension == 2 or
                                              (cube and texture.m_TextureDimension == 4)):
        raise ValueError(f'Unsupported mobile texture layout for {texture.m_Name}')
    block, size = layout
    original = texture.get_image_data()
    output = bytearray()
    cursor = 0
    for _ in range(faces):
        for i in range(texture.m_MipCount):
            w, h = max(1, texture.m_Width >> i), max(1, texture.m_Height >> i)
            n = ((w + block - 1) // block) * ((h + block - 1) // block) * size
            pixels = parse_image_data(original[cursor:cursor+n], w, h, texture.m_TextureFormat,
                                      texture.object_reader.version, texture.object_reader.platform,
                                      texture.m_PlatformBlob, flip=False).convert('RGBA')
            output.extend(pixels.tobytes())
            cursor += n
    if cursor != len(original):
        raise ValueError(f'Unexpected mip payload for {texture.m_Name} ({cursor}/{len(original)})')
    texture.m_TextureFormat = 4
    texture.m_CompleteImageSize = len(output) // faces
    texture.image_data = bytes(output)
    if texture.m_StreamData:
        texture.m_StreamData.path = ''
        texture.m_StreamData.offset = texture.m_StreamData.size = 0
    texture.save()
    return True


def unityfs_blocks(bundle, raw):
    reader = EndianBinaryReader(raw, endian='>')
    reader.read_string_to_null()
    reader.read_u_int()
    reader.read_string_to_null()
    reader.read_string_to_null()
    return bundle.read_fs(reader)


def convert_text_bundle(bundle, target=WINDOWS64):
    # A standalone TextAsset needs only a new build target. Keep its type
    # trees, object table and data byte-for-byte instead of rebuilding them.
    if len(bundle.files) != 1:
        return None
    name, asset = next(iter(bundle.files.items()))
    if (not isinstance(asset, SerializedFile) or asset.header.version != 21
            or asset.header.endian != '<' or asset._m_target_platform not in (13, WINDOWS64, LINUX64)
            or {obj.type.name for obj in asset.objects.values()} != {'AssetBundle', 'TextAsset'}):
        return None
    source = asset.reader.bytes
    version = asset.unity_version.encode('ascii') + b'\0'
    # SerializedFile v21 has a 20-byte header followed by the engine version and target.
    offset = 20 + len(version)
    if source[20:offset + 4] != version + asset._m_target_platform.to_bytes(4, 'little'):
        return None
    expected = source[:offset] + target.to_bytes(4, 'little') + source[offset + 4:]
    member = EndianBinaryReader(expected)
    member.flags = asset.flags
    bundle.files[name] = member
    out = bundle.save(packer='lz4')
    nodes, blocks = unityfs_blocks(bundle, out)
    if len(nodes) != 1 or nodes[0].path != name or nodes[0].flags != member.flags:
        raise ValueError('Converted text bundle member differs')
    blocks.Position = nodes[0].offset
    if blocks.read(nodes[0].size) != expected:
        raise ValueError('Converted text bundle contents differ')
    return out


def convert_bundle(raw, crc=0, *, target=WINDOWS64):
    if target not in (WINDOWS64, LINUX64):
        raise ValueError(f'Unsupported desktop bundle platform {target}')
    if not raw.startswith(b'UnityFS\0'):
        raise ValueError(f'Unexpected file header {raw[:12].hex()}')
    bundle = UnityPy.load(raw).file
    if crc:
        _, blocks = unityfs_blocks(bundle, raw)
        if zlib.crc32(blocks.bytes) != crc:
            raise ValueError('Game-supplied bundle CRC does not match the original stream')
    text = convert_text_bundle(bundle, target)
    if text is not None:
        return text
    files = {}
    for name, asset in bundle.files.items():
        if not isinstance(asset, SerializedFile):
            continue
        files[name] = set(asset.objects)
        if asset._m_target_platform in (WINDOWS64, LINUX64):
            # Both players use the same converted desktop GL shader and texture data.
            asset._m_target_platform = target
            continue
        if asset._m_target_platform != 13:
            raise ValueError(f'{name} targets platform {asset._m_target_platform}')
        for obj in asset.objects.values():
            if obj.type.name in ('Texture2D', 'Cubemap'):
                convert_texture(obj.read())
            elif obj.type.name == 'Shader':
                shader = obj.read()
                if shader.platforms == [9]:
                    convert_shader(shader)
                elif 15 not in shader.platforms:
                    raise ValueError(f'No desktop shader for {shader.m_ParsedForm.m_Name}')
        asset._m_target_platform = target
    if not files:
        raise ValueError('UnityFS contains no serialized asset')
    out = bundle.save(packer='lz4')
    reopened = UnityPy.load(out).file
    if set(reopened.files) != set(bundle.files):
        raise ValueError('Converted bundle member list differs')
    for name, ids in files.items():
        asset = reopened.files[name]
        if asset._m_target_platform != target or set(asset.objects) != ids:
            raise ValueError(f'Converted object inventory differs in {name}')
    return out


def request_batches(folder, stop):
    """Arm the directory notification before looking for pending conversions."""
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        watch = kernel.FindFirstChangeNotificationW
        watch.argtypes = [wintypes.LPCWSTR, wintypes.BOOL, wintypes.DWORD]
        watch.restype = wintypes.HANDLE
        advance = kernel.FindNextChangeNotification
        advance.argtypes = [wintypes.HANDLE]
        wait = kernel.WaitForSingleObject
        wait.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        wait.restype = wintypes.DWORD
        close = kernel.FindCloseChangeNotification
        close.argtypes = [wintypes.HANDLE]
        handle = watch(str(folder), False, 1)  # FILE_NOTIFY_CHANGE_FILE_NAME
        if handle and handle != wintypes.HANDLE(-1).value:
            try:
                while not stop.is_set():
                    yield folder.glob('*.request')
                    result = wait(handle, 250)
                    while result == 258 and not stop.is_set():
                        result = wait(handle, 250)
                    if stop.is_set():
                        return
                    if result != 0 or not advance(handle):
                        break
            finally:
                close(handle)
    while not stop.is_set():
        yield folder.glob('*.request')
        stop.wait(0.05)


def serve(folder, stop):
    folder.mkdir(exist_ok=True)
    # The cache can hold thousands of converted files. Watch only the small
    # queue, including requests that arrived before the converter started.
    inbox = folder / 'pending'
    inbox.mkdir(exist_ok=True)
    for pending in request_batches(inbox, stop):
        for request in pending:
            destination = folder / (request.stem + '.winbundle')
            failure = destination.with_suffix('.error')
            try:
                if failure.exists():
                    # Earlier players cached brief Windows sharing violations as permanent failures.
                    if not retry_shared_file(failure.read_bytes).startswith(b'PermissionError:'):
                        retry_shared_file(request.unlink)
                        continue
                    retry_shared_file(failure.unlink)
                if destination.exists():
                    retry_shared_file(request.unlink)
                    continue
                digest, crc = request.stem.split('-')
                raw = retry_shared_file(request.read_bytes)
                if hashlib.sha256(raw).hexdigest() != digest:
                    raise ValueError('Input stream digest differs')
                out = convert_bundle(raw, int(crc, 16))
                temporary = destination.with_suffix('.writing')
                retry_shared_file(temporary.write_bytes, out)
                retry_shared_file(temporary.replace, destination)
                retry_shared_file(request.unlink)
            except Exception as error:
                if request.exists():
                    retry_shared_file(failure.write_text, f'{type(error).__name__}: {error}')
                    # Keeping a failed request in the watch queue makes every
                    # later load revisit it. The error remains in the cache.
                    retry_shared_file(request.unlink)


def retry_shared_file(operation, *args):
    # Antivirus and other file observers can hold newly published files briefly.
    deadline = time.monotonic() + 10
    while True:
        try:
            return operation(*args)
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def use_fast_lz4():
    import lz4.block
    from UnityPy.enums.BundleFile import CompressionFlags
    from UnityPy.helpers import CompressionHelper

    # UnityPy defaults to high-compression search even for ordinary LZ4.
    # The private runtime cache is decoded by Unity's same LZ4 reader.
    CompressionHelper.COMPRESSION_MAP[CompressionFlags.LZ4] = (
        lambda data: lz4.block.compress(data, mode='fast', store_size=False))


if __name__ == '__main__':
    import threading
    use_fast_lz4()
    stop = threading.Event()
    def stop_on_parent_exit():
        sys.stdin.buffer.read()
        stop.set()
    threading.Thread(target=stop_on_parent_exit, daemon=True).start()
    serve(Path(sys.argv[1]) / CACHE_DIRECTORY, stop)
