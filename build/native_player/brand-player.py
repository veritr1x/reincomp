"""Embed the original Android app icon in local Windows executables."""
import argparse
import ctypes as ct
from ctypes import wintypes as wt
from io import BytesIO
from pathlib import Path
import shutil
import struct
import tempfile
import zipfile

from PIL import Image


def icon_images(apk):
    with zipfile.ZipFile(apk) as source:
        images = {}
        for density in ('ldpi', 'mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi'):
            with Image.open(BytesIO(source.read(f'res/mipmap-{density}-v4/app_icon.png'))) as icon:
                images[icon.width] = icon.convert('RGBA')
    result = {}
    for size in (16, 24, 32, 48, 64, 96, 128, 192, 256):
        original = images[next((n for n in sorted(images) if n >= size), max(images))]
        image = original.resize((size, size), Image.Resampling.LANCZOS)
        stream = BytesIO()
        image.save(stream, format='PNG')
        result[size] = stream.getvalue()
    return result


def embed(executable, images):
    kernel = ct.WinDLL('kernel32', use_last_error=True)
    def api(name, result, *args):
        proc = getattr(kernel, name)
        proc.argtypes, proc.restype = args, result
        return proc
    begin = api('BeginUpdateResourceW', wt.HANDLE, wt.LPCWSTR, wt.BOOL)
    update = api('UpdateResourceW', wt.BOOL, wt.HANDLE, ct.c_void_p, ct.c_void_p, wt.WORD, ct.c_void_p, wt.DWORD)
    end = api('EndUpdateResourceW', wt.BOOL, wt.HANDLE, wt.BOOL)
    load = api('LoadLibraryExW', wt.HMODULE, wt.LPCWSTR, wt.HANDLE, wt.DWORD)
    free = api('FreeLibrary', wt.BOOL, wt.HMODULE)
    name_callback = ct.WINFUNCTYPE(wt.BOOL, wt.HMODULE, ct.c_void_p, ct.c_void_p, ct.c_ssize_t)
    lang_callback = ct.WINFUNCTYPE(wt.BOOL, wt.HMODULE, ct.c_void_p, ct.c_void_p, wt.WORD, ct.c_ssize_t)
    names = api('EnumResourceNamesW', wt.BOOL, wt.HMODULE, ct.c_void_p, name_callback, ct.c_ssize_t)
    langs = api('EnumResourceLanguagesW', wt.BOOL, wt.HMODULE, ct.c_void_p, ct.c_void_p, lang_callback, ct.c_ssize_t)
    find = api('FindResourceExW', wt.HRSRC, wt.HMODULE, ct.c_void_p, ct.c_void_p, wt.WORD)
    resource_size = api('SizeofResource', wt.DWORD, wt.HMODULE, wt.HRSRC)
    resource_load = api('LoadResource', wt.HANDLE, wt.HMODULE, wt.HRSRC)
    resource_lock = api('LockResource', ct.c_void_p, wt.HANDLE)
    def check(result):
        if not result:
            raise ct.WinError(ct.get_last_error())
        return result
    def data(module, kind, name, lang):
        resource = find(module, kind, name, lang)
        if not resource:
            return None
        return ct.string_at(check(resource_lock(check(resource_load(module, resource)))), resource_size(module, resource))
    # Keep Unity's group IDs and languages. The launcher can start without resources.
    groups = set()
    @name_callback
    def group(module, kind, name, unused):
        @lang_callback
        def language(_module, _kind, _name, lang, _):
            groups.add((ct.wstring_at(name) if name > 65535 else name, lang))
            return True
        return langs(module, kind, name, language, 0)
    def key(name):
        return ct.cast(ct.c_wchar_p(name), ct.c_void_p) if isinstance(name, str) else name
    def resources():
        for group_name, lang in groups:
            rows = []
            for i, (size, image) in enumerate(images.items(), 30000):
                yield 3, i, lang, image
                rows.append(struct.pack('<4B2HIH', size % 256, size % 256, 0, 0, 1, 32, len(image), i))
            yield 14, group_name, lang, struct.pack('<3H', 0, 1, len(rows)) + b''.join(rows)
    def matches(module):
        return all(data(module, kind, key(name), lang) == value for kind, name, lang, value in resources())
    with tempfile.TemporaryDirectory(dir=executable.parent) as staging:
        temp = Path(staging) / executable.name
        shutil.copy2(executable, temp)
        module = check(load(str(temp), None, 2))
        try:
            names(module, 14, group, 0)
            groups.add((101, 0))
            if matches(module):
                return
        finally:
            free(module)
        handle = check(begin(str(temp), False))
        try:
            for kind, name, lang, value in resources():
                check(update(handle, kind, key(name), lang, value, len(value)))
        except BaseException:
            end(handle, True)
            raise
        check(end(handle, False))
        module = check(load(str(temp), None, 2))
        try:
            if not matches(module):
                raise ValueError(f'Embedded icon verification failed for {executable.name}')
        finally:
            free(module)
        temp.replace(executable)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('executables', type=Path, nargs='+')
    args = parser.parse_args()
    icons = icon_images(args.apk)
    for exe in args.executables:
        embed(exe.resolve(strict=True), icons)
        print(f'{exe.name}: verified {len(icons)} APK icon sizes')
