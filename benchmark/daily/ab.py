#!/usr/bin/env python3
"""A/B of ish builds or switches by CPU cycles, on a machine others are using (Windows).

  ab.py REPS LABEL=[ENV=VALUE,...]@ISH.EXE ... [--cases py.loop,py.import] [--rootfs C:/src/rootfs/daily-x86_64]

Runs the cases of cases.txt like run.py does (a fresh ish per command, in /tmp/daily), the variants
interleaved, and prints per case each variant's median wall time and its least CPU cycles
(QueryProcessCycleTime, in billions): others' load only ever adds to a run's cycles, and far less
than to its times (GetProcessTimes ticks in 15.6 ms and counts what a hyperthread sibling costs).
The ratios and their geometric mean are of the cycles, to the first variant. Outputs must match.

  ab.py 5 head=@C:/src/ish-head.exe new=@build-x86_64-jit/ish.exe nogroups=ISH_XLAT_NOSPGROUP=1@build-x86_64-jit/ish.exe
"""
import argparse, ctypes, hashlib, os, statistics, subprocess
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ENV = ['HOME=/root', 'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin', 'LANG=C.UTF-8', 'TERM=dumb',
       'GOCACHE=/root/.cache/go-build', 'GOFLAGS=-buildvcs=false']


def load_cases(path, only):
    cases = []
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, cat, cmd = line.split(None, 2)
        if cat == 'net' or (only and name not in only):
            continue
        cases.append((name, cmd))
    return cases


def run(exe, env, rootfs, cmd):
    import time
    e = dict(os.environ)
    e.update(env)
    t = time.perf_counter()
    p = subprocess.Popen([exe, '-f', rootfs, '-d', '/tmp/daily', '/usr/bin/env', '-i'] + ENV + ['/bin/sh', '-c', cmd], env=e,
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    out, _ = p.communicate()
    wall = time.perf_counter() - t
    cycles = ctypes.c_ulonglong()
    ctypes.windll.kernel32.QueryProcessCycleTime(int(p._handle), ctypes.byref(cycles))
    return wall, cycles.value / 1e9, hashlib.md5(out).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('reps', type=int)
    ap.add_argument('variants', nargs='+')
    ap.add_argument('--cases', default='')
    ap.add_argument('--rootfs', default='C:/src/rootfs/daily-x86_64')
    a = ap.parse_args()
    variants = []
    for v in a.variants:
        label, rest = v.split('=', 1)
        envs, exe = rest.rsplit('@', 1)
        variants.append((label, exe, dict(x.split('=', 1) for x in envs.split(',') if x)))
    cases = load_cases(os.path.join(HERE, 'cases.txt'), set(filter(None, a.cases.split(','))))
    if not cases:
        return print('no cases')
    print('%-18s' % 'case' + ''.join('%22s' % (v[0] + ' wall/Gcyc') for v in variants) + '   cycles to first', flush=True)
    geo = [1.0] * len(variants)
    bad = set()
    for name, cmd in cases:
        res = {v[0]: [] for v in variants}
        ref = None
        for i in range(a.reps + 1):   # the first round warms the file cache
            for label, exe, env in variants:
                w, c, h = run(exe, env, a.rootfs, cmd)
                ref = ref or h
                if h != ref and (label, name) not in bad:
                    bad.add((label, name))
                    print('  ❌ output differs:', label, name, flush=True)
                if i:
                    res[label].append((w, c))
        best = [(statistics.median(x[0] for x in res[v[0]]), min(x[1] for x in res[v[0]])) for v in variants]
        ratios = [b[1] / best[0][1] if best[0][1] > 0 else 1.0 for b in best]
        geo = [g * r for g, r in zip(geo, ratios)]
        print('%-18s' % name[:18] + ''.join('%14.2f /%6.2f' % b for b in best) + '   ' + ' '.join('%.3f' % r for r in ratios), flush=True)
    print('%-18s' % 'geomean' + ' ' * 22 * len(variants) + '   ' + ' '.join('%.3f' % (g ** (1 / len(cases))) for g in geo))


if __name__ == '__main__':
    main()
