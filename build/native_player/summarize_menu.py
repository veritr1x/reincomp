"""Measure Menu tab presentation from a PresentMon CSV and the matching frame-state log."""
import argparse
import csv
import math
import re
import statistics
from bisect import bisect_right
from collections import defaultdict
from pathlib import Path


def summarize(csv_path, log_path, warmup=30):
    clicked = arrival = previous = None
    pending_click = pending_arrival = exact_click = False
    mode = 'player'
    windows = []
    conditions = []
    for line in log_path.read_text(encoding='utf-8').splitlines():
        if line.startswith('Menu tab tap qpcMs='):
            clicked = float(line.split('=',1)[1])
            exact_click = True
        if not exact_click and ('action=OnTap ' in line or 'action=OnTapMenu ' in line):
            pending_click = True
        if 'Screen route arrived at MenuTopScreenView' in line:
            pending_arrival = True
        if line.startswith('Menu pointer idleFilter='):
            mode = 'optimized' if 'idleFilter=1 ' in line else 'original'
        if not line.startswith('qpcMs='):
            continue
        fields = dict(re.findall(r'(\w+)=([^ ]+)',line))
        end = float(fields['qpcMs'])
        if pending_click:
            clicked = end
            pending_click = False
        if pending_arrival:
            arrival = end
            pending_arrival = False
        focused,total = map(int,fields['foreground'].split('/'))
        eligible = (fields.get('size')=='3840x2160' and fields.get('highest')=='1'
                    and fields.get('renderInterval')=='1' and focused==total and total>0)
        if previous is not None:
            conditions.append((previous,end,eligible))
        if previous is not None and arrival is not None and previous >= arrival + warmup*1000 and eligible:
            windows.append((previous,end,mode,float(fields['mainCPU'])))
        previous = end
    if arrival is None or clicked is None:
        raise ValueError('The frame log must contain the Settings and Profile tab navigation and arrival')
    pid_file=csv_path.with_suffix('.pid')
    target_pid=pid_file.read_text().strip() if pid_file.is_file() else None
    with csv_path.open(newline='', encoding='utf-8-sig') as stream:
        rows = [(float(r['CPUStartQPCTimeInMs']),float(r['MsBetweenPresents']),r['ProcessID'],r['SwapChainAddress'])
                for r in csv.DictReader(stream) if r.get('CPUStartQPCTimeInMs') not in (None,'','NA')
                and (target_pid is None or r['ProcessID']==target_pid)
                and r.get('MsBetweenPresents') not in (None,'','NA') and float(r['MsBetweenPresents'])>0]
    if not rows or len({(pid,swap) for _,_,pid,swap in rows})!=1:
        raise ValueError('A capture of one player PID and presentation stream is required')
    frames = defaultdict(list)
    samples = defaultdict(list)
    first,last = min(t for t,*_ in rows),max(t for t,*_ in rows)
    opening = [(start,end,valid) for start,end,valid in conditions if start<clicked+5000 and end>clicked]
    opening_valid = (first<=clicked and last>=clicked+5000 and bool(opening) and
                     opening[0][0]<=clicked and opening[-1][1]>=clicked+5000 and all(v for _,_,v in opening))
    if first>clicked or last<clicked+5000:
        print('The capture missed part of the opening five seconds. Only settled timing is available.')
    elif not opening_valid:
        print('The opening did not have complete focused 4K Highest timing windows.')
    windows = [w for w in windows if first<=w[0] and w[1]<=last]
    starts = [w[0] for w in windows]
    condition_starts = [w[0] for w in conditions]
    # Include the time between the opening sample and the first settled
    # window. In particular, a slow animation must not vanish into warmup.
    settling_end = starts[0] if starts else last
    for start,end,mode,cpu in windows:
        samples[mode].append(cpu)
    for timestamp,interval,_,_ in rows:
        if opening_valid and clicked <= timestamp < clicked+5000:
            frames['first five seconds'].append(interval)
        elif clicked+5000 <= timestamp < settling_end:
            i = bisect_right(condition_starts,timestamp)-1
            if i >= 0 and timestamp <= conditions[i][1] and conditions[i][2]:
                frames['after opening, before settled'].append(interval)
        index = bisect_right(starts,timestamp)-1
        if index>=0:
            start,end,mode,_ = windows[index]
            if start <= timestamp < end:
                frames[mode].append(interval)
    if not samples or not any(k in frames for k in samples):
        raise ValueError('No complete focused Menu timing windows overlap the presentation capture')
    for name,intervals in frames.items():
        ordered = sorted(intervals)
        p = lambda n: ordered[math.ceil(n*len(ordered)/100)-1]
        missed = sum(t>8.3 for t in ordered)
        cpu = f', median main thread {statistics.median(samples[name]):.3f} ms/frame' if name in samples else ''
        print(f'{name}: {len(ordered)} presents, {1000/statistics.mean(ordered):.1f} FPS, '
              f'p95 {p(95):.3f} ms, p99 {p(99):.3f} ms, max {ordered[-1]:.3f} ms, {missed} over 8.3 ms{cpu}')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('frame_log',type=Path)
    parser.add_argument('--warmup',type=float,default=30)
    args=parser.parse_args()
    summarize(args.capture,args.frame_log,args.warmup)
