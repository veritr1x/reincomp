"""Measure foreground presentations inside completed, identified gameplay windows."""
import argparse
import csv
import math
import re
import statistics
from bisect import bisect_left
from pathlib import Path


def report_frame_cpu(path, spans):
    """Measure work between two consecutive returns from Unity's render pipeline."""
    groups = {'under 8.3 ms': [], 'at least 8.3 ms': []}
    previous = None
    with path.open() as stream:
        for line in stream:
            if not line.endswith('\n'):
                break  # The timed runner can exit while writing the last record.
            frame, start, input_cycles, render, render_cycles, end, end_cycles, _ = line.split()
            frame, end, end_cycles = int(frame), float(end), int(end_cycles)
            if previous and previous[0]+1 == frame and any(a <= previous[1] < end <= b for a,b in spans):
                inter, update, draw = (int(input_cycles)-previous[2], int(render_cycles)-int(input_cycles),
                                       end_cycles-int(render_cycles))
                if min(inter, update, draw) >= 0 and float(start) <= float(render) <= end:
                    group = 'at least 8.3 ms' if end-previous[1] >= 8.3 else 'under 8.3 ms'
                    groups[group].append((inter, update, draw))
            previous = (frame, end, end_cycles)
    for label, rows in groups.items():
        if rows:
            inter, update, render = (statistics.median(col)/1e6 for col in zip(*rows))
            print(f'Main-thread render completions {label}: {len(rows)} frames, median Mcycles '
                  f'before input {inter:.2f}, input to render {update:.2f}, render {render:.2f}')


def summarize(capture, require_motion=False, moving_only=False, field_scene='tw002001'):
    actor = '3D' if capture.name.startswith('field-') else '2D'
    windows = []
    previous = None
    scene = None
    first_actor = None
    last_sample = 0.0
    scene_start = 0.0
    first_field_window = None
    cameras = {}
    for line in capture.with_suffix('.frames.log').read_text().splitlines():
        if match := re.fullmatch(r'Auto camera frame=(\d+) position=([-\d.,]+)', line):
            cameras[match[1]]=tuple(map(float,match[2].split(',')))
        if match := re.fullmatch(r'Frame \d+ active scene -?\d+ (.*)', line):
            scene = match[1]
            scene_start = last_sample
            previous = None
            first_actor = None
        if not line.startswith('qpcMs='):
            continue
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
        foreground, total = map(int, fields['foreground'].split('/'))
        end = float(fields['qpcMs'])
        last_sample = end
        if fields['actor'] == actor and fields['loaded'] == '1' and first_actor is None:
            first_actor = end
        eligible = (fields['size'] == '3840x2160' and fields['actor'] == actor
                    and fields['highest'] == fields['loaded'] == fields['renderInterval'] == '1'
                    and foreground > 0 and foreground == total
                    and ((actor == '3D' and scene == field_scene) or
                         (actor == '2D' and scene and scene.startswith('pb_') and first_actor is not None
                          and end-first_actor >= 45000)))
        camera = cameras.get(fields['frame'])
        moved = previous and previous[2] and camera and math.dist(previous[2], camera) > 0.1
        if eligible and previous and previous[1] and (not moving_only or moved):
            if actor == '3D' and first_field_window is None:
                first_field_window = (scene_start, previous[0])
            windows.append((previous[0], float(fields['qpcMs']), float(fields['mainCPU']), scene))
        previous = (float(fields['qpcMs']), eligible, camera)
    if not windows:
        if moving_only:
            raise ValueError('No verified foreground window with movement along the Auto route')
        expected = field_scene if actor == '3D' else 'a settled storybook'
        raise ValueError(f'No consecutive foreground windows in {expected}. Last active scene: {scene}')
    spans=[]
    for begin,end,_,_ in windows:
        if spans and spans[-1][1]==begin:
            spans[-1]=(spans[-1][0],end)
        else:
            spans.append((begin,end))
    print('Moving camera' if moving_only else 'Scene',actor, sorted(set(w[3] for w in windows)), 'foreground seconds',round(sum(b-a for a,b in spans)/1000,2),
          'mean main CPU ms/update',round(statistics.mean(w[2] for w in windows),3))
    counts={}
    def report(key,values):
        if not values:return
        values=sorted(values)
        counts[key]=len(values)
        mean = statistics.mean(values)
        percentile = lambda p: values[math.ceil(p*len(values))-1]
        fields=[key,'frames',len(values),'mean',round(mean,3),'p95',round(percentile(.95),3),
                'p99',round(percentile(.99),3),'max',round(values[-1],3),
                'over8.3',sum(v >= 8.3 for v in values)]
        if key not in ('MsGPUBusy','GPUBusy'):
            fields.extend(('Hz',round(1000/mean,2)))
        print(*fields)
    if capture.exists():
        pid_file=capture.with_suffix('.pid')
        pid=pid_file.read_text().strip() if pid_file.exists() else None
        with capture.open(encoding='utf-8-sig') as stream:
            reader=csv.DictReader(stream)
            timestamp=next((key for key in ('CPUStartQPCTimeInMs','CPUStartQPCTime','QPCTime')
                            if key in (reader.fieldnames or [])),None)
            rows=[row for row in reader if timestamp and row.get(timestamp) not in (None,'','NA') and
                  (pid is None or row.get('ProcessID')==pid)]
        route=capture.with_suffix('.route.log')
        if not moving_only and first_field_window and route.is_file() and timestamp=='CPUStartQPCTimeInMs':
            entries=map(float,re.findall(r' Loading\.Out .*?qpcMs=([\d.]+)',route.read_text()))
            entry=next((t for t in entries if first_field_window[0]<=t<=first_field_window[1]),None)
            if entry is not None:
                entry_times=[float(row['MsBetweenPresents']) for row in rows
                             if row.get('MsBetweenPresents') not in (None,'','NA') and
                             entry<=float(row[timestamp])<entry+5000]
                report(f'{field_scene} entry (first five seconds after Loading.Out)',entry_times)
        rows=[row for row in rows if any(a<=float(row[timestamp])<b for a,b in spans)]
        for key in ('MsBetweenPresents','MsBetweenDisplayChange','MsGPUBusy','FrameTime','DisplayedTime','GPUBusy'):
            report(key,[float(row[key]) for row in rows if row.get(key) not in (None,'','NA') and float(row[key])>0])
    for suffix,key in (('.submit.csv','Native render submissions'),('.render.csv','OpenGL render thread')):
        path=capture.with_suffix(suffix)
        if not path.exists():continue
        with path.open() as stream:
            rows=[row for row in csv.reader(stream) if len(row) in (2,6) and row[1]]
        intervals=[]
        swaps=0
        driver_ms=0
        for a,b in spans:
            samples=[r for r in rows if a<=float(r[1])<b]
            intervals.extend(float(cur[1])-float(prev[1]) for prev,cur in zip(samples,samples[1:]))
            if len(samples)>1 and all(len(r)==6 and r[2]=='1' for r in samples):
                swaps+=int(samples[-1][5])-int(samples[0][5])
                driver_ms+=float(samples[-1][1])-float(samples[0][1])
        report(key,intervals)
        if swaps>0: print('OpenGL driver completed',swaps,'buffer swaps in',round(driver_ms/1000,2),'seconds',round(1000*swaps/driver_ms,2),'Hz')
    if not (counts.get('MsBetweenPresents') or counts.get('FrameTime') or counts.get('OpenGL render thread')):
        raise ValueError('No native render-thread or presentation samples in the verified scene')
    os_frames=counts.get('MsBetweenPresents') or counts.get('FrameTime')
    if os_frames and counts.get('OpenGL render thread'):
        print('Foreground interval counts: Windows',os_frames,'and render thread',counts['OpenGL render thread'])
    cpu = capture.with_suffix('.cpu.tsv')
    if cpu.is_file():
        report_frame_cpu(cpu, spans)
    profile=capture.with_suffix('.managed.tsv')
    if profile.is_file() and not moving_only:
        with profile.open() as stream:
            methods=sorted(csv.DictReader(stream,delimiter='\t'),key=lambda row:float(row['exclusive_ms']),reverse=True)
        print('Main-thread managed callbacks over ten seconds (exclusive elapsed time)')
        for row in methods[:5]:
            print(f"{row['class']}.{row['method']}: {row['exclusive_ms']} ms in {row['calls']} calls")
    story_work = capture.with_suffix('.story.tsv')
    if story_work.is_file() and not moving_only:
        with story_work.open() as stream:
            samples = [(float(elapsed), method) for start, elapsed, method in csv.reader(stream, delimiter='\t')
                       if any(a <= float(start) and float(start) + float(elapsed) <= b for a, b in spans)]
        if samples:
            elapsed, method = max(samples)
            print(f'Longest traced main-thread operation in the measured scene: {method}, {elapsed:.3f} ms')
    if not moving_only and (require_motion or cameras):
        summarize(capture,moving_only=True,field_scene=field_scene)


def summarize_live(capture, count, thread_cpu):
    """Report complete player log windows within a capture of an existing process."""
    pid = capture.with_suffix('.pid').read_text().strip()
    with capture.open(encoding='utf-8-sig', newline='') as stream:
        rows = [(float(r['CPUStartQPCTimeInMs']), float(r['MsBetweenPresents']), r['SwapChainAddress'])
                for r in csv.DictReader(stream) if r['ProcessID'] == pid
                and r.get('CPUStartQPCTimeInMs') not in ('', 'NA', None)
                and r.get('MsBetweenPresents') not in ('', 'NA', None) and float(r['MsBetweenPresents']) > 0]
    if not rows or len({r[2] for r in rows}) != 1:
        raise ValueError('Expected one presentation stream from the running game')
    rows.sort()
    times = [r[0] for r in rows]
    previous = None
    reports = []
    for line in capture.with_suffix('.frames.log').read_text(encoding='utf-8').splitlines():
        if re.match(r'^Frame \d+ active scene ', line):
            previous = None
        if not line.startswith('qpcMs='):
            continue
        fields = dict(re.findall(r'(\w+)=([^ ]+)', line))
        end = float(fields['qpcMs'])
        start, previous = previous, end
        if start is None or start < times[0] or end > times[-1]:
            continue
        span = rows[bisect_left(times,start):bisect_left(times,end)]
        if not span:
            continue
        dt = sorted(r[1] for r in span)
        n = len(dt)
        pct = lambda p: dt[math.ceil(p*n)-1]
        cpu = f" | UnityMain CPU {fields['mainCPU']} ms/frame" if thread_cpu else ''
        reports.append(f"{fields['size']} {fields['actor']} | {n} presents, {1000/statistics.mean(dt):.1f} FPS"
                       f" | frame ms p50={pct(.5):.3f} p95={pct(.95):.3f} p99={pct(.99):.3f} max={dt[-1]:.3f}"
                       f" | at_or_above_8_30ms={sum(v>=8.3 for v in dt)}/{n}"
                       f" over_120hz_budget={sum(v>1000/120 for v in dt)}/{n}"
                       f" | game_foreground_samples={fields['foreground']}{cpu}")
        if len(reports) == count:
            print(*reports,sep='\n')
            return
    raise ValueError(f'Captured {len(reports)} of {count} full timing windows in the running game')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--require-motion', action='store_true')
    parser.add_argument('--live-windows', type=int)
    parser.add_argument('--thread-cpu', action='store_true')
    parser.add_argument('--field-scene', default='tw002001', help='Expected Unity scene for a 3D checkpoint')
    args = parser.parse_args()
    if args.live_windows:
        summarize_live(args.capture, args.live_windows, args.thread_cpu)
    else:
        summarize(args.capture, require_motion=args.require_motion, field_scene=args.field_scene)
