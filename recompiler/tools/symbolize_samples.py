"""Flat and inclusive profiles from rip_sampler.py output.

    python symbolize_samples.py SAMPLES.json EXE.nm MODULES.txt [--top 40] [--tid T]

EXE.nm is `nm -n --defined-only` of the sampled executable (image base taken
from the nm addresses' 0x140000000 default); MODULES.txt has "base size name"
hex lines for every loaded module. Addresses outside the executable are
attributed to their DLL.
"""
import argparse, bisect, collections, json


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples'); ap.add_argument('nm'); ap.add_argument('modules')
    ap.add_argument('--top', type=int, default=40); ap.add_argument('--tid')
    a = ap.parse_args()
    d = json.load(open(a.samples))
    addrs, names = [], []
    for line in open(a.nm, errors='replace'):
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in 'tTwW':
            addrs.append(int(p[0], 16)); names.append(p[2].strip())
    image = 0x140000000
    mods = []
    for line in open(a.modules):
        b, s, n = line.split(None, 2); mods.append((int(b, 16), int(s, 16), n.strip()))
    base, size = d['base'], d['size']

    def name(x):
        if base <= x < base + size:
            i = bisect.bisect_right(addrs, x - base + image) - 1
            return names[i] if i >= 0 else '?exe'
        for b, s, n in mods:
            if b <= x < b + s:
                return n
        return '?%x' % (x >> 16)

    tids = [a.tid] if a.tid else [t for t, s in d['samples'].items() if s]
    for t in tids:
        s = d['samples'][t]
        flat = collections.Counter(name(c[0]) for c in s)
        incl = collections.Counter()
        first_exe = collections.Counter()
        for c in s:
            seen = set(name(x) for x in c)
            incl.update(seen)
            e = next((name(x) for x in c if base <= x < base + size), None)
            if e: first_exe[e] += 1
        n = len(s)
        print('thread', t, 'samples', n)
        print(' flat:')
        for k, v in flat.most_common(a.top): print('  %5.1f%% %s' % (100 * v / n, k))
        print(' nearest exe frame (attributes DLL time to its caller):')
        for k, v in first_exe.most_common(a.top): print('  %5.1f%% %s' % (100 * v / n, k))
        print(' inclusive (any heuristic frame):')
        for k, v in incl.most_common(a.top): print('  %5.1f%% %s' % (100 * v / n, k))


if __name__ == '__main__':
    main()
