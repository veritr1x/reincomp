import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from desktop_bundles import convert_texture


class CubemapTest(unittest.TestCase):
    def test_preserve_each_face_and_mip(self):
        compressed = bytearray()
        expected = bytearray()
        for face in range(6):
            for mip, side in enumerate((5, 2, 1)):
                rgba = (20 + 30 * face, 80 * mip, 35, 171)
                # An ASTC LDR void-extent block stores a single 16-bit RGBA color.
                block = bytes.fromhex('fc fd ff ff ff ff ff ff') + struct.pack('<4H', *(c * 257 for c in rgba))
                compressed.extend(block * (((side + 3) // 4) ** 2))
                expected.extend(bytes(rgba) * side * side)

        reader = SimpleNamespace(type=SimpleNamespace(name='Cubemap'), version=(2019, 4, 29, 1), platform=13)
        texture = SimpleNamespace(object_reader=reader, m_Name='cube', m_TextureFormat=54, m_TextureDimension=2,
                                  m_ImageCount=6, m_Width=5, m_Height=5, m_MipCount=3, m_PlatformBlob=None,
                                  m_StreamData=SimpleNamespace(path='original', offset=128, size=len(compressed)),
                                  get_image_data=lambda: bytes(compressed), save=lambda: None)
        self.assertTrue(convert_texture(texture))
        self.assertEqual(texture.image_data, expected)
        self.assertEqual(texture.m_CompleteImageSize, len(expected) // 6)
        self.assertEqual(texture.m_TextureFormat, 4)
        self.assertEqual((texture.m_StreamData.path, texture.m_StreamData.offset, texture.m_StreamData.size),
                         ('', 0, 0))


if __name__ == '__main__':
    unittest.main()
