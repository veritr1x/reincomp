"""Run the Windows player against the prepared private save and local asset server."""
from pathlib import Path
import json
import os
import queue
import shutil
import time
import subprocess
import sys
import threading
from datetime import datetime

root = (Path(sys.argv[sys.argv.index('--root')+1]).resolve() if '--root' in sys.argv
        else Path(__file__).resolve().parent)
client = root / 'checkpoint-client'
install = root.parent / 'emu-client'
server_data = (client / (client/'active-server.txt').read_text().strip()).resolve(strict=True)
if server_data.parent != client or not (server_data / 'game.db').is_file():
    raise RuntimeError('Prepared private server data is missing')
args = [install/'server/bunker-server.exe', '-data', server_data, '-assets', install/'data/assets',
        '-port-offset', '31000', '-stdin']
flags = subprocess.CREATE_NO_WINDOW
server = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          creationflags=flags, text=True, encoding='utf-8', errors='replace')
ready = queue.Queue()
def drain():
    with (client/'server.log').open('w',encoding='utf-8') as log:
        for line in server.stdout:
            log.write(line)
            log.flush()
            try:
                event = json.loads(line)
            except ValueError:
                continue
            if isinstance(event,dict) and 'game' in event:
                ready.put(True)
        ready.put(False)
reader = threading.Thread(target=drain,daemon=True)
reader.start()
worker_stop=threading.Event()
if '--route' in sys.argv[2:]:
    from desktop_bundles import CACHE_DIRECTORY, serve
    worker=threading.Thread(target=serve,args=(client/CACHE_DIRECTORY,worker_stop),daemon=True)
    worker.start()
try:
    if not ready.get(timeout=60):
        raise RuntimeError(f'Local backend did not start. See {client / "server.log"}')
    env = dict(os.environ,REINCOMP_NATIVE_CHECKPOINT='1')
    if env.get('REINCOMP_TRACE_MENU_ASSETS'):
        (root/'runtime-player/menu-assets.log').write_text('')
    if any(flag in sys.argv[2:] for flag in ('--screenshots', '--cpu-profile', '--movie')):
        env['REINCOMP_NATIVE_CAPTURE']='1'
    else:
        env.pop('REINCOMP_NATIVE_CAPTURE', None)
    env['REINCOMP_ISOLATED_SERVER']='1'
    env['REINCOMP_NATIVE_PLAYER_ROOT']=str(root)
    profile=client/'frame-pacing.json'
    if profile.is_file():
        settings=json.loads(profile.read_text(encoding='utf-8-sig'))
        if settings.get('computer','').casefold()==env.get('COMPUTERNAME','').casefold():
            cpu=settings.get('field_main_cpu')
            rate=settings.get('field_present_rate')
            if type(cpu) is int and 0<=cpu<64 and '--compare-affinity' not in sys.argv[2:]:
                env.setdefault('REINCOMP_NATIVE_MAIN_CPU',str(cpu))
            if type(rate) is int and 120<rate<=165:
                env.setdefault('REINCOMP_NATIVE_PRESENT_RATE',str(rate))
    if '--route' in sys.argv[2:]:
        env.update(REINCOMP_ROUTE_CHECKPOINT='1',REINCOMP_DESKTOP_BUNDLES='1')
        env['REINCOMP_CHECKPOINT_PREFS'] = str(server_data / 'player-prefs.bin')
        if '--player-prefs' in sys.argv[2:]:
            env['REINCOMP_CHECKPOINT_PREFS'] = str(Path(sys.argv[sys.argv.index('--player-prefs')+1]).resolve())
        env['REINCOMP_BENCH_PROFILE'] = 'native-checkpoint-' + server_data.name.split('-server-')[0]
    if '--open-screen' in sys.argv[2:]:
        screens = {s.lower(): s for s in ('Organization','Enhance','Quest','Map','Library','Menu','Mama','All')}
        screens['summon'] = 'Gacha'
        env['REINCOMP_OPEN_SCREEN'] = screens[sys.argv[sys.argv.index('--open-screen')+1].lower()]
    if '--movie' in sys.argv[2:]:env['REINCOMP_TEST_TITLE_MOVIE']='1'
    if '--cpu-profile' in sys.argv[2:]:env['REINCOMP_SAMPLE_FIELD_CPU']='1'
    if '--managed-profile' in sys.argv[2:]:env['REINCOMP_PROFILE_MANAGED']='1'
    if '--compare-menu-pointer' in sys.argv[2:]:env['REINCOMP_COMPARE_MENU_POINTER']='1'
    if '--auto-walk' in sys.argv[2:]:env['REINCOMP_NATIVE_AUTO_WALK']='1'
    if '--compare-batching' in sys.argv[2:]:env['REINCOMP_COMPARE_SRP_BATCHING']='1'
    if '--compare-affinity' in sys.argv[2:]:env['REINCOMP_COMPARE_MAIN_AFFINITY']='1'
    if '--borderless' in sys.argv[2:]:
        env['REINCOMP_NATIVE_FULLSCREEN']='1'
    else:
        env.pop('REINCOMP_NATIVE_FULLSCREEN', None)
    if '--workers' in sys.argv[2:]:
        workers=int(sys.argv[sys.argv.index('--workers')+1])
        if not 1<=workers<=15:raise ValueError('Expected 1 to 15 job workers')
        env['REINCOMP_NATIVE_JOB_WORKERS']=str(workers)
    if '--present' in sys.argv[2:]:
        if env.get('REINCOMP_OPEN_SCREEN')!='Menu':
            env['REINCOMP_NATIVE_RENDER_TIMING']='1'
        if '--renderer-only' in sys.argv[2:]:
            env['REINCOMP_NATIVE_RENDER_ONLY']='1'
        stamp=datetime.now().strftime('%Y%m%d-%H%M%S')
        scene=server_data.name.split('-server-')[0]
        env['REINCOMP_NATIVE_PRESENT']=str(root/f'{scene}-presentation-{stamp}.csv')
        print(env['REINCOMP_NATIVE_PRESENT'],flush=True)
    started = time.time()
    player_args=['powershell','-NoProfile','-File',Path(__file__).with_name('test-runtime-host.ps1'),'-Windowed']
    if '--play' in sys.argv[2:]:
        player_args.append('-Play')
    else:
        player_args.extend(['-TimeoutSeconds',sys.argv[1] if len(sys.argv)>1 else '45'])
    if '--size' in sys.argv[2:]:
        player_args.extend(['-Render', sys.argv[sys.argv.index('--size')+1]])
    result = subprocess.run(player_args,
                            env=env,creationflags=flags,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    sys.stdout.buffer.write(result.stdout)
    if '--present' in sys.argv[2:]:
        capture=Path(env['REINCOMP_NATIVE_PRESENT'])
        picture='bridge-native.png' if scene=='field' else 'storybook-native.png'
        for name, suffix in [('frame-state.log','.frames.log'),('route-trace.log','.route.log'),
                             ('render-thread.csv','.render.csv'),('render-submit.csv','.submit.csv'),
                             ('managed-profile.tsv','.managed.tsv'),('slow-callbacks.tsv','.callbacks.tsv'),('menu-assets.log','.assets.log'),
                             ('story-work.tsv','.story.tsv'),
                             ('weapon-query-check.tsv','.weapons.tsv'),
                             ('posted-work-samples.tsv','.posted.tsv'),
                             ('transition-samples.tsv','.transition.tsv'),
                             ('field-inflation.tsv','.inflation.tsv'),
                             ('field-metadata.tsv','.metadata.tsv'),
                             ('field-load-queue.tsv','.loads.tsv'),
                             ('render-profile.tsv','.comparison.tsv'),
                             ('frame-release.csv','.release.csv'),('frame-cpu.tsv','.cpu.tsv'),
                             ('gc-pauses.tsv','.gc.tsv'),(picture,'.png')]:
            evidence=root/'runtime-player'/name
            if evidence.exists() and evidence.stat().st_mtime>=started:
                shutil.copy2(evidence,capture.with_suffix(suffix))
        if result.returncode == 124:
            if env.get('REINCOMP_OPEN_SCREEN')=='Menu':
                from summarize_menu import summarize
                summarize(capture,capture.with_suffix('.frames.log'))
            else:
                from summarize_present import summarize
                summarize(capture,require_motion='--auto-walk' in sys.argv[2:])
            sys.exit(0)
    sys.exit(result.returncode)
finally:
    worker_stop.set()
    server.stdin.close()
    try:
        server.wait(timeout=10)
    except subprocess.TimeoutExpired:
        server.terminate()
        server.wait(timeout=10)
    reader.join(timeout=5)
