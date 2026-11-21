from pathlib import Path
from struct import pack
import sys
from tempfile import TemporaryDirectory
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from checkpoint_prefs import import_playerprefs_xml


class PlayerPrefsImportTest(unittest.TestCase):
    def test_android_encoding_and_window_settings(self):
        def string(value):
            data = value.encode('utf-16-le')
            return pack('<I', len(data) // 2) + data

        with TemporaryDirectory() as directory:
            source = Path(directory, 'prefs.xml')
            target = Path(directory, 'snapshot.bin')
            xml = b'''<map><string name="label%20key">A%2BB%252F&amp;%F0%9F%8C%99</string>
                <int name="counter" value="-7"/><float name="scale" value="1.5"/>
                <int name="Screenmanager%20Resolution%20Width" value="1280"/></map>'''
            source.write_bytes(xml)
            import_playerprefs_xml(source, target)
            self.assertEqual(source.read_bytes(), xml)
            self.assertEqual(target.read_bytes(), b'BNKPREF\x01' + pack('<I', 3)
                             + string('label key') + b'\x01' + string('A+B%2F&🌙')
                             + string('counter') + b'\x02' + pack('<i', -7)
                             + string('scale') + b'\x03' + pack('<f', 1.5))


if __name__ == '__main__':
    unittest.main()
