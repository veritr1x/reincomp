"""Check the files Bunker sees with a local or shared Linux resource library."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

LAUNCHER = Path(__file__).resolve().parents[2] / "build/linux_player/web_launch.py"
MASTER = "20240404193219.bin.e"


class ResourceLibraryTest(unittest.TestCase):
    def test_both_library_choices_serve_downloaded_notices(self):
        for shared in (False, True):
            with self.subTest(shared=shared), tempfile.TemporaryDirectory() as folder:
                game = Path(folder)
                state = game / ".reincomp"
                runtime = state / "runtime"
                (runtime / "tools").mkdir(parents=True)
                launcher = runtime / "tools/web_launch.py"
                shutil.copyfile(LAUNCHER, launcher)
                player = state / "player-runtime"
                player.mkdir()
                binary = player / "Reincarnation.x86_64"
                binary.write_text("#!/bin/sh\nprintf '%s' \"$REINCOMP_BUNKER_PORT_OFFSET\" > player-offset\n")
                binary.chmod(0o755)
                library = game / "shared" if shared else state / "resources"
                revision = library / "assets/revisions/0"
                (revision / "assetbundle").mkdir(parents=True)
                (revision / "resources").mkdir()
                for name in ("list.bin", "info.json"):
                    (revision / name).touch()
                (game / MASTER).write_bytes(b"patched master")
                (game / "notices.zip").write_bytes(b"browser notice pack")
                server = runtime / "bunker-server"
                server.write_text(f"#!{sys.executable}\n" + """from pathlib import Path
import sys
assets = Path(sys.argv[sys.argv.index('-assets') + 1])
Path('server-offset').write_text(sys.argv[sys.argv.index('-port-offset') + 1])
assert (assets / 'notices/notices.zip').read_bytes() == b'browser notice pack'
if Path('shared').exists():
    assert (assets / 'assets/release/20240404193219.bin.e').read_bytes() == b'patched master'
print('ready', flush=True)
sys.stdin.read()
""")
                server.chmod(0o755)
                args = ["--assets", str(library)] if shared else []
                env = os.environ.copy()
                env.pop("REINCOMP_BUNKER_PORT_OFFSET", None)
                if shared:
                    env["REINCOMP_BUNKER_PORT_OFFSET"] = "4000"
                subprocess.run([sys.executable, launcher, *args], cwd=game, env=env, check=True, timeout=10)
                expected = "4000" if shared else "0"
                self.assertEqual((game / "server-offset").read_text(), expected)
                self.assertEqual((player / "player-offset").read_text(), expected)
                if shared:
                    self.assertFalse((library / "notices").exists())


if __name__ == "__main__":
    unittest.main()
