"""The kit's emblem recipe must point at a blank placeholder in GameAssembly.dll."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "web"))
from check_player import check_emblem, check_kit

PNG = b"\x89PNG\r\n\x1a\n" + bytes(range(200)) * 3


def player(dll, offset, size=len(PNG), digest=None):
    packed = io.BytesIO()
    with ZipFile(packed, "w") as z:
        z.writestr("game/GameAssembly.dll", dll)
        z.writestr("game/portable-emblem.json", json.dumps(
            {"offset": offset, "size": size, "sha256": digest or hashlib.sha256(PNG).hexdigest()}))
    return packed.getvalue()


class EmblemTest(unittest.TestCase):
    def test_blank_placeholder_passes(self):
        dll = b"code" * 100 + bytes(len(PNG)) + b"more" * 50
        check_emblem(player(dll, 400))

    def test_stale_offset_is_rejected(self):
        # The 0.3.10 and 0.3.11 kits: the adapter moved and still held the image.
        dll = b"code" * 150 + PNG + b"more" * 50
        with self.assertRaisesRegex(ValueError, "placeholder"):
            check_emblem(player(dll, 400))

    def test_embedded_image_is_rejected(self):
        dll = b"code" * 100 + bytes(len(PNG)) + b"more" * 50 + PNG
        with self.assertRaisesRegex(ValueError, "still contains"):
            check_emblem(player(dll, 400))

    def test_recipe_outside_the_library_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "placeholder"):
            check_emblem(player(b"code" * 10, 400))

    def test_kit_parts_are_joined(self):
        dll = b"code" * 150 + PNG
        packed = player(dll, 400)
        with tempfile.TemporaryDirectory() as tmp:
            kit = Path(tmp)
            parts = []
            for n, start in enumerate(range(0, len(packed), 300), 1):
                (kit / f"player.part{n:02d}").write_bytes(packed[start:start + 300])
                parts.append([f"player.part{n:02d}", ""])
            (kit / "player.json").write_text(json.dumps({"parts": parts}))
            with self.assertRaisesRegex(ValueError, "placeholder"):
                check_kit(kit)


if __name__ == "__main__":
    unittest.main()
