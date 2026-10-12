#!/usr/bin/env python3
"""Time the daily cases (cases.txt) on one or more ish builds, interleaved.

Every run is a fresh ish process, as Minis runs each command:
  ish -f ROOTFS -d /tmp/daily /usr/bin/env -i ENV... /bin/sh -c COMMAND
with stdin closed. Builds are interleaved per repetition (the test machine is
shared and noisy), each case reports the median time per build and the median
of the per-repetition ratios against the first build. The stdout of every run
must match the reference build's (default: the first), except net cases.

  run.py --ish new=C:/src/ish/build-x86_64-jit/ish.exe --ish base=C:/src/ish-base.exe \\
         --rootfs C:/src/rootfs/daily-x86_64 --reps 5 [--filter 'py\\.'] [--no-net]
"""
import argparse, hashlib, json, os, re, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ENV = ['HOME=/root', 'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
       'LANG=C.UTF-8', 'TERM=dumb', 'GOCACHE=/root/.cache/go-build', 'GOFLAGS=-buildvcs=false']

try:
    from tqdm import tqdm
except ImportError:
    tqdm = None


def load_cases(path, pattern, net):
    cases = []
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, cat, cmd = line.split(None, 2)
        if pattern and not re.search(pattern, name):
            continue
        if cat == 'net' and not net:
            continue
        cases.append({'name': name, 'cat': cat, 'cmd': cmd})
    return cases


def cpu_seconds(p):
    """User + kernel time of the finished child, all its threads (Windows)."""
    if os.name != 'nt':
        return float('nan')
    import ctypes
    from ctypes import wintypes
    ft = [wintypes.FILETIME() for _ in range(4)]
    if not ctypes.windll.kernel32.GetProcessTimes(int(p._handle), *[ctypes.byref(f) for f in ft]):
        return float('nan')
    return sum((f.dwHighDateTime << 32 | f.dwLowDateTime) for f in ft[2:]) / 1e7


def run_once(ish, rootfs, cmd, timeout):
    argv = [ish, '-f', rootfs, '-d', '/tmp/daily', '/usr/bin/env', '-i'] + ENV + ['/bin/sh', '-c', cmd]
    t0 = time.perf_counter()
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, _ = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return {'t': timeout, 'cpu': float('nan'), 'rc': 'timeout', 'out': b''}
    return {'t': time.perf_counter() - t0, 'cpu': cpu_seconds(p), 'rc': p.returncode, 'out': out}


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')  # the Windows console is GBK
    ap = argparse.ArgumentParser()
    ap.add_argument('--ish', action='append', required=True, help='NAME=PATH, first is the baseline')
    ap.add_argument('--rootfs', required=True)
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--filter')
    ap.add_argument('--no-net', action='store_true')
    ap.add_argument('--warmup', type=int, default=1, help='untimed runs per case and build first')
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--cases', default=os.path.join(HERE, 'cases.txt'))
    ap.add_argument('--out', default=os.path.join(HERE, 'results'))
    a = ap.parse_args()
    builds = [tuple(s.split('=', 1)) for s in a.ish]
    names = [b[0] for b in builds]
    cases = load_cases(a.cases, a.filter, not a.no_net)
    if not cases:
        return print('no cases')

    res = {c['name']: {n: [] for n in names} for c in cases}
    cpu = {c['name']: {n: [] for n in names} for c in cases}
    outs = {c['name']: {} for c in cases}
    total = len(cases) * len(builds) * (a.reps + a.warmup)
    bar = tqdm(total=total, unit='run') if tqdm else None
    print(f'🚀 {len(cases)} cases x {len(builds)} builds x {a.reps} reps (+{a.warmup} warmup)', flush=True)
    for rep in range(-a.warmup, a.reps):
        for c in cases:
            # rotate the build order so no build always runs first
            order = builds[rep % len(builds):] + builds[:rep % len(builds)]
            for name, ish in order:
                r = run_once(ish, a.rootfs, c['cmd'], a.timeout)
                if bar:
                    bar.update()
                outs[c['name']].setdefault(name, set()).add((r['rc'], hashlib.sha256(r['out']).hexdigest()[:12],
                                                             r['out'][:200].decode('utf-8', 'replace').strip()))
                if rep >= 0:
                    res[c['name']][name].append(r['t'])
                    cpu[c['name']][name].append(r['cpu'])
        if not bar:
            print(f'  rep {rep + 1}/{a.reps} done', flush=True)
    if bar:
        bar.close()

    ref = names[0]
    rows = []
    for c in cases:
        n = c['name']
        med = {b: statistics.median(res[n][b]) for b in names}
        mcpu = {b: statistics.median(cpu[n][b]) for b in names}
        ratios = {b: statistics.median(x / y for x, y in zip(res[n][b], res[n][ref])) for b in names[1:]}
        ref_out = outs[n][ref]
        bad = []
        if len(ref_out) != 1:
            bad.append(f'{ref} unstable')
        for b in names[1:]:
            if c['cat'] != 'net' and outs[n][b] != ref_out:
                bad.append(f'{b} differs')
        rc_bad = [b for b in names if any(o[0] != 0 for o in outs[n][b])]
        if rc_bad:
            bad.append('rc!=0: ' + ','.join(rc_bad))
        sample = sorted(ref_out)[0][2].replace('\n', ' ')[:40]
        rows.append({'case': n, 'cat': c['cat'], 'median': med, 'cpu': mcpu, 'ratio': ratios,
                     'check': 'ok' if not bad else '; '.join(bad), 'out': sample,
                     'times': res[n]})

    head = '| case | cat | ' + ' | '.join(f'{b} (s) | {b} cpu' for b in names) + ''.join(f' | {b}/{ref}' for b in names[1:]) + ' | check | output |'
    print('\n' + head)
    print('|' + '---|' * (head.count('|') - 1))
    for r in rows:
        print(f"| {r['case']} | {r['cat']} | " + ' | '.join(f"{r['median'][b]:.3f} | {r['cpu'][b]:.2f}" for b in names)
              + ''.join(f" | {r['ratio'][b]:.3f}" for b in names[1:]) + f" | {r['check']} | `{r['out']}` |")
    if len(names) > 1:
        for b in names[1:]:
            g = statistics.geometric_mean([r['ratio'][b] for r in rows if r['cat'] != 'net'])
            print(f'\n📊 geomean {b}/{ref} (without net): {g:.3f}')
    fails = [r['case'] for r in rows if r['check'] != 'ok']
    print(('❌ check failed: ' + ', '.join(fails)) if fails else '✅ all outputs match')

    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, time.strftime('daily_%Y%m%d_%H%M%S.json'))
    with open(path, 'w') as f:
        json.dump({'builds': dict(builds), 'reps': a.reps, 'rows': rows}, f, indent=1)
    print(f'💾 {path}')


if __name__ == '__main__':
    main()
