"""Exercise cache seeding with a real Octo-shaped catalog and filesystem links."""
import errno
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "build/native_player"))
from octo_cache import seed


def field(number, value):
    assert len(value) < 128
    return bytes((number << 3 | 2, len(value))) + value


class OctoCacheTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.assets, self.data = root / "assets", root / "data"
        self.revision = self.assets / "assets/revisions/0"
        self.cache = self.data / "storage/files/octo"
        self.catalog = self.revision / "list.bin"
        self.revision.mkdir(parents=True)
        self.catalog.touch()

    def entry(self, kind, key, name, payload):
        md5 = hashlib.md5(payload).hexdigest()
        row = field(3, name.encode()) + field(10, md5.encode()) + field(11, key.encode())
        with self.catalog.open("ab") as f:
            f.write(field(kind, row))
        raw = (("A" if kind == 2 else "R") + key).encode().hex().upper()
        return self.cache / "v1/301" / raw[-1] / raw / (md5 + (".mp4" if name.endswith(".mp4") else ""))

    def test_fresh_upgrade_and_repeat(self):
        for kind, key, name, path in [(2, "hqA3", "ui)title", "assetbundle/ui/title.assetbundle"),
                                      (4, "vF71", "movie.mp4", "resources/movie.mp4")]:
            payload = key.encode()
            dest = self.entry(kind, key, name, payload)
            source = self.revision / path
            source.parent.mkdir(parents=True)
            source.write_bytes(payload)
            if kind == 2:
                dest.parent.mkdir(parents=True)
                dest.write_bytes(payload)
            seed(self.data, self.assets)
            self.assertTrue(dest.samefile(source))
            self.assertEqual((dest.parent / ".meta").stat().st_size, 8)
            seed(self.data, self.assets)
            self.assertTrue(dest.samefile(source))

    def test_wrong_content_and_unavailable_links(self):
        dest = self.entry(4, "x", "tag.txt", b"verified")
        source = self.revision / "resources/tag.txt"
        source.parent.mkdir()
        source.write_bytes(b"damaged!")
        seed(self.data, self.assets)
        self.assertFalse(dest.exists())
        with patch("os.link", side_effect=OSError(errno.EXDEV, "different volumes")):
            self.entry(4, "another", "tag.txt", b"damaged!")
            seed(self.data, self.assets)
        self.assertEqual(list(self.cache.rglob(".meta")), [])

    def test_preserves_private_cache_and_ignores_parent_paths(self):
        dest = self.entry(4, "x", "tag.txt", b"good")
        source = self.revision / "resources/tag.txt"
        source.parent.mkdir()
        source.write_bytes(b"good")
        dest.parent.mkdir(parents=True)
        dest.write_bytes(b"user")
        escaped = self.entry(4, "y", "..)tag.txt", b"good")
        (self.revision / "tag.txt").write_bytes(b"good")
        seed(self.data, self.assets)
        self.assertEqual(dest.read_bytes(), b"user")
        self.assertFalse(dest.samefile(source))
        self.assertFalse(escaped.exists())


if __name__ == "__main__":
    unittest.main()
