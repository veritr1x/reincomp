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
                env.pop("DISPLAY", None)
                env.pop("WAYLAND_DISPLAY", None)
                if shared:
                    env["REINCOMP_BUNKER_PORT_OFFSET"] = "4000"
                subprocess.run([sys.executable, launcher, *args], cwd=game, env=env, check=True, timeout=10)
                expected = "4000" if shared else "0"
                self.assertEqual((game / "server-offset").read_text(), expected)
                self.assertEqual((player / "player-offset").read_text(), expected)
                if shared:
                    self.assertFalse((library / "notices").exists())
                def set_port(value, ok=True):
                    return subprocess.run([sys.executable, launcher, "--server-ports", value], cwd=game,
                                          env=env, check=ok, capture_output=True, text=True, timeout=10)
                self.assertIn("Game 11868", set_port("11945").stdout)
                self.assertNotEqual(set_port("65536", ok=False).returncode, 0)
                subprocess.run([sys.executable, launcher, *args], cwd=game, env=env, check=True, timeout=10)
                expected = "4000" if shared else "3865"
                self.assertEqual((game / "server-offset").read_text(), expected)
                self.assertEqual((player / "player-offset").read_text(), expected)
                self.assertIn("Assets 8080", set_port("default").stdout)
                self.assertFalse((state / "server-ports.json").exists())

    def test_desktop_can_retry_import_and_start_the_player(self):
        with tempfile.TemporaryDirectory() as folder:
            game = Path(folder)
            state = game / ".reincomp"
            runtime = state / "runtime"
            tools = runtime / "tools"
            tools.mkdir(parents=True)
            shutil.copyfile(LAUNCHER, tools / LAUNCHER.name)
            (game / MASTER).touch()
            (game / "notices.zip").touch()
            (game / "fixed.zip").touch()
            server = runtime / "bunker-server"
            server.write_text(f"#!{sys.executable}\n" + """from pathlib import Path
import sys
if sys.argv[1] == 'import':
    if sys.argv[sys.argv.index('-dump')+1].endswith('bad.zip'):
        sys.exit('Invalid resource archive')
    print('{"step":"extracting","done":1,"total":2}')
    base = Path(sys.argv[sys.argv.index('-assets')+1]) / 'assets/revisions/0'
    (base/'assetbundle').mkdir(parents=True)
    (base/'resources').mkdir()
    (base/'list.bin').touch()
    (base/'info.json').touch()
else:
    print('ready', flush=True)
    sys.stdin.read()
    Path('server-stopped').touch()
""")
            server.chmod(0o755)
            player = state / "player-runtime/Reincarnation.x86_64"
            player.parent.mkdir()
            player.write_text("#!/bin/sh\ntouch played\n")
            player.chmod(0o755)
            (tools / "launcher_window.py").write_text("""import json, sys
from pathlib import Path
def deploy(name):
    print(json.dumps({'kind':'dump','path':str(Path(name).resolve())}), flush=True)
def event():
    return json.loads(input())
assert sys.argv[1:] == ['', '']
deploy('bad.zip')
assert 'Invalid resource archive' in event()['error']
deploy('fixed.zip')
assert event() == {'progress':50}
for stage in ('preparing', 'starting', 'playing'):
    assert event() == {'stage':stage}
assert not sys.stdin.read()
Path('window-closed').touch()
""")
            env = {**os.environ, "DISPLAY": ":test", "REINCOMP_BUNKER_PORT_OFFSET": "0"}
            subprocess.run([sys.executable, tools / LAUNCHER.name], cwd=game, env=env, check=True, timeout=15)
            self.assertTrue((player.parent / "played").exists())
            self.assertTrue((game / "server-stopped").exists())
            self.assertTrue((game / "window-closed").exists())

            (tools / "launcher_window.py").write_text("raise SystemExit(78)\n")
            (player.parent / "played").unlink()
            subprocess.run([sys.executable, tools / LAUNCHER.name], cwd=game, env=env, check=True, timeout=10)
            self.assertTrue((player.parent / "played").exists())
            explicit = subprocess.run([sys.executable, tools / LAUNCHER.name, "--launcher"],
                                      cwd=game, env=env, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(explicit.returncode, 0)
            self.assertIn("WebKitGTK", explicit.stderr)


if __name__ == "__main__":
    unittest.main()
