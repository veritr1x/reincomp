"""Prepare a private writable game storage clone and import the emulator save."""
from contextlib import closing
from pathlib import Path
import os
import shutil
import sqlite3
import subprocess
import sys
from checkpoint_prefs import write_snapshot
from tempfile import mkdtemp
from uuid import uuid4
from zipfile import ZipFile

root = Path(sys.argv[2]).resolve() if len(sys.argv)>2 else Path(__file__).resolve().parent
install = root.parent / 'emu-client'
route = sys.argv[1] if len(sys.argv) >= 2 else 'field'
if route not in ('field', 'storybook', 'menu'):
    raise SystemExit('Expected field, storybook or menu')
client = root / 'checkpoint-client'
client.mkdir(exist_ok=True)
storage = client / 'storage'
apk_assets=client/'apk-assets'
if not apk_assets.exists():
    staging=Path(mkdtemp(dir=client,prefix='assets-'))
    with ZipFile(install/'data/game-sound.apk') as apk:
        for entry in apk.infolist():
            if entry.is_dir() or not entry.filename.startswith('assets/'):continue
            dest=(staging/entry.filename.removeprefix('assets/')).resolve()
            if not dest.is_relative_to(staging):raise ValueError('Invalid APK asset path')
            dest.parent.mkdir(parents=True,exist_ok=True)
            with apk.open(entry) as src,dest.open('wb') as dst:shutil.copyfileobj(src,dst)
    staging.rename(apk_assets)
if not storage.exists():
    staging = Path(mkdtemp(dir=client, prefix='storage-'))
    shutil.copytree(install / 'data/storage', staging, dirs_exist_ok=True)
    staging.rename(storage)
archive = root.parent / 'emulator-restore' / f'{route}-checkpoint.zip'
server_data = Path(mkdtemp(dir=client, prefix=f'{route}-server-'))
# The game's importer merges progress into an existing device identity.
source = install / 'data/server'
for file in ('game.db', 'auth.db'):
    with closing(sqlite3.connect((source/file).as_uri()+'?mode=ro',uri=True)) as src, closing(sqlite3.connect(server_data/file)) as dst:
        src.backup(dst)
for file in ('auth.key','bunker.version','translation-state.json'):
    if (source/file).is_file():
        shutil.copy2(source/file,server_data/file)
if route == 'menu':
    fixtures = [root.parent / 'emulator-restore/menu-checkpoint.db',
                Path('E:/bunker-repo-data/.build/regions/screens/global-fixture/files/saves/game.db')]
    fixture = Path(sys.argv[3]) if len(sys.argv)>3 else next((p for p in fixtures if p.is_file()), None)
    if not fixture or not fixture.is_file():
        raise SystemExit('Supply a progressed Android game.db with -MenuSave.')
    # The fixture and this device can already share a second, fresh character.
    # Only change device associations in the private copy. Keep each character's progress.
    archive = server_data / 'menu-import.db'
    newest = 'SELECT user_id, uuid FROM users ORDER BY game_start_datetime DESC, user_id DESC LIMIT 1'
    with closing(sqlite3.connect(fixture.resolve().as_uri()+'?mode=ro', uri=True)) as src, closing(sqlite3.connect(archive)) as saved:
        src.backup(saved)
        player, original = saved.execute(newest).fetchone()
        with closing(sqlite3.connect(server_data/'game.db')) as device:
            _, local = device.execute(newest).fetchone()
        if original != local:
            with saved:
                saved.execute('UPDATE users SET uuid=? WHERE uuid=?', (str(uuid4()), local))
                saved.execute('UPDATE users SET uuid=? WHERE user_id=?', (local, player))
                saved.execute('DELETE FROM sessions')
        quests, = saved.execute('SELECT COUNT(*) FROM user_quests WHERE user_id=?', (player,)).fetchone()
        print(f'Menu checkpoint uses the last active saved character with {quests} quest records.', flush=True)
subprocess.run([install / 'server/bunker-server.exe', 'saves', 'import',
                '-data', server_data, '-from', archive], check=True)
if not (server_data / 'game.db').is_file():
    raise SystemExit('Save import did not create the game database')
write_snapshot(route, server_data / 'player-prefs.bin')
temp = server_data / 'active-server.txt'
temp.write_text(server_data.name)
os.replace(temp, client / 'active-server.txt')
(client / 'python-runtime.txt').write_text(sys.executable, encoding='utf-8')
print(f'Prepared {route} save at {server_data}')
