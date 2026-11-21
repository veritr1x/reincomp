"""Import the emulator's managed PlayerPrefs into a private snapshot."""
from io import BytesIO
from pathlib import Path
from struct import pack, unpack
from urllib.parse import unquote
import xml.etree.ElementTree as ET


MAGIC = b'BNKPREF\x01'
WINDOW_KEYS = ('Screenmanager', 'unity.', '__UNITY_')


def text(value):
    raw = value.encode('utf-16-le')
    if len(raw) // 2 > 262144:
        raise ValueError('A game preference exceeds the snapshot string limit')
    return pack('<I', len(raw) // 2) + raw


def save_entries(entries, destination):
    data = MAGIC + pack('<I', len(entries)) + b''.join(text(key) + value for key, value in entries.items())
    if len(entries) > 16384 or len(data) > 4 * 1024 * 1024:
        raise ValueError('Game preferences exceed the player snapshot limits')
    destination.write_bytes(data)


def import_playerprefs_xml(source, destination):
    if source.stat().st_size > 4 * 1024 * 1024:
        raise ValueError('Android PlayerPrefs XML exceeds 4 MiB')
    root = ET.parse(source).getroot()
    if root.tag != 'map':
        raise ValueError('Expected an Android SharedPreferences map')
    entries = {}
    for node in root:
        name = unquote(node.attrib['name'])
        if name.startswith(WINDOW_KEYS):
            continue
        if name in entries:
            raise ValueError('Duplicate Android PlayerPrefs key')
        if node.tag == 'string':
            value = b'\x01' + text(unquote(node.text or ''))
        elif node.tag == 'int':
            value = b'\x02' + pack('<i', int(node.attrib['value']))
        elif node.tag == 'float':
            value = b'\x03' + pack('<f', float(node.attrib['value']))
        else:
            raise ValueError(f'Unsupported Unity PlayerPrefs XML element {node.tag!r}')
        entries[name] = value
    save_entries(entries, destination)


def write_snapshot(route, destination):
    import winreg
    package = 'com.square_enix.android_googleplay.nierspww'
    profiles = r'Software\Bunker\Apps' + '\\' + package + r'\BenchmarkProfiles'
    original = 'emulator-storybook-prologue' if route == 'storybook' else 'emulator-scene8-prefread'
    target = 'native-checkpoint-' + route
    def copy_value(store, name):
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, '\\'.join((profiles, original, store))) as key:
            value, kind = winreg.QueryValueEx(key, name)
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, '\\'.join((profiles, target, store))) as key:
            winreg.SetValueEx(key, name, 0, kind, value)
        return value

    copy_value('SecureSettings', 'android_id')
    data = copy_value('SharedPreferences', package + '.v2.playerprefs')
    source = BytesIO(data)
    if source.read(8) != MAGIC:
        raise ValueError('Unexpected Android preference snapshot')
    def integer():
        return unpack('<I', source.read(4))[0]
    def read_text():
        count = integer()
        raw = source.read(count * 2)
        if len(raw) != count * 2:
            raise ValueError('Truncated Android string')
        return raw.decode('utf-16-le')

    entries = {}
    for _ in range(integer()):
        name = unquote(read_text())
        kind = source.read(1)
        if kind == bytes([1]):
            value = kind + text(unquote(read_text()))
        elif kind in (bytes([2]), bytes([3])):
            value = kind + source.read(4)
        else:
            raise ValueError(f'Unsupported preference type {kind!r}')
        if name.startswith(WINDOW_KEYS):
            continue
        if name in entries:
            raise ValueError(f'Duplicate preference {name}')
        entries[name] = value
    if 'PlayerRegistrations' not in entries or source.read(1):
        raise ValueError('The emulator player registration is missing')
    save_entries(entries, destination)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='Prepare a private replay snapshot from Unity Android PlayerPrefs XML.')
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.destination.resolve():
        parser.error('Choose a separate snapshot destination')
    import_playerprefs_xml(args.source, args.destination)
