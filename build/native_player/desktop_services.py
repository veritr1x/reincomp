"""Serve Bunker's editors, Archive and the Windows player's bundle conversions."""
import hmac
import json
import logging
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
from urllib.request import Request as HTTPRequest, urlopen

import anyio
import android_archive
import android_runtime
import desktop_runtime
from fastapi import Request
from fastapi.responses import JSONResponse, RedirectResponse, PlainTextResponse
from starlette.concurrency import run_in_threadpool

sys.path.insert(0, str(Path(__file__).resolve().parent))
from desktop_bundles import CACHE_DIRECTORY, serve
import desktop_display


def install_player_restart(control):
    """Keep the authenticated editor alive across changes that close the game on Android."""
    original_guard = android_runtime.install_game_guard
    def player(action):
        request = HTTPRequest(control + '/' + action, data=b'',
                              headers={'X-Bunker-Token': os.environ['BUNKER_CONTROL_TOKEN']})
        with urlopen(request, timeout=50) as response:
            if response.status != 204:
                raise OSError('Unexpected response from the game launcher')

    def install(app, live, close):
        original_guard(app, live, close)
        # Bunker's session and Origin checks wrap the guard. Buffer the form
        # before closing WebView2, which may disconnect its HTTP client.
        @app.middleware('http')
        async def calendar_restart(request, call_next):
            if request.method != 'POST' or request.url.path not in android_runtime.CLOSES:
                return await call_next(request)
            await request.body()
            with anyio.CancelScope(shield=True):
                paused = False
                try:
                    await run_in_threadpool(player, 'stop')
                    error = await run_in_threadpool(desktop_runtime._result, '/pause')
                    if error:
                        raise OSError(error)
                    paused = True
                    # The stock editor sees the paused server and applies the
                    # same calendar handler it uses when the game is closed.
                    return await call_next(request)
                except OSError as error:
                    logging.error('Bunker calendar change failed: %s', error)
                    return JSONResponse({'ok': False, 'error': str(error)}, status_code=503)
                finally:
                    try:
                        if paused:
                            error = await run_in_threadpool(desktop_runtime._result, '/resume?title=0')
                            if error:
                                logging.error('Bunker server could not resume: %s', error)
                    finally:
                        await run_in_threadpool(player, 'start')
    android_runtime.install_game_guard = install


def main():
    def argument(name):
        return Path(sys.argv[sys.argv.index(name)+1]).resolve()
    assets, data = argument('--assets'), argument('--data').parent
    helper = Path(__file__).with_name('archive-media.exe')
    def convert(action, *args):
        result = subprocess.run([helper, action, *map(str, args)], capture_output=True,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        return result.stderr.decode(errors='replace') if result.returncode else ''
    converters = {action: (lambda *args, name=action: convert(name, *args))
                  for action in ('decode', 'sound', 'model', 'motion', 'scenario')}
    archive_root = data/'archive'
    archive_root.mkdir(exist_ok=True)
    # Archive looks under <data>/tools on Android. The desktop editors keep their
    # original master in --tools, including when the user has patched the live copy.
    editor_master = argument('--tools')/'patcher/origin.bin.e'
    upstream_original = android_archive.original_master
    def original_master(asset_root, archive_root):
        return (editor_master, True) if editor_master.is_file() else upstream_original(asset_root, archive_root)
    android_archive.original_master = original_master
    lock = threading.Lock()
    def archive():
        with lock:
            android_archive.build_index(str(assets), str(archive_root), converters['scenario'])
            return json.loads(android_archive.start(str(assets), str(archive_root), converters))
    if control := os.environ.get('REINCOMP_PLAYER_CONTROL'):
        install_player_restart(control)
    original = android_runtime.create_app
    def app_with_archive(token, origin):
        desktop_display.install(os.environ.get('REINCOMP_PLAYER_CONTROL'), os.environ.get('BUNKER_CONTROL_TOKEN', ''))
        app = original(token, origin)
        @app.get('/bunker/archive')
        @app.get('/bunker/archive/{path:path}')
        async def open_archive(request: Request, path: str = ''):
            if request.headers.get('host') != origin.split('//', 1)[1] or not hmac.compare_digest(request.cookies.get('lunar_tools',''), token):
                return PlainTextResponse('Forbidden', status_code=403)
            if path and not re.fullmatch(r'schedule/(summon|event)/[0-9]+', path):
                return PlainTextResponse('Unknown Archive link', status_code=404)
            session = await run_in_threadpool(archive)
            response = RedirectResponse(session['url']+'/'+path)
            response.set_cookie('lunar_archive', session['token'], httponly=True, samesite='strict')
            return response
        return app
    android_runtime.create_app = app_with_archive
    stop = threading.Event()
    worker = threading.Thread(target=serve, args=(data/CACHE_DIRECTORY, stop), daemon=True)
    worker.start()
    try:
        desktop_runtime.main()
    finally:
        stop.set()
        android_archive.stop()

if __name__ == '__main__':
    main()
