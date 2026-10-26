#!/usr/bin/env python3
"""Sample every case under ish and split its CPU time by what ran (prof.py categories).

    survey.py --rootfs <rootfs> --out <dir> [--ish build-dev/ish] [--only a,b] [--secs 4]

Each case runs in a loop inside one ish for about --secs seconds (its time
from --times, a results jsonl, decides the count) while macOS `sample`
records it; the report is one row per case with the share of each category.
ISH_* variables are passed through from the environment.
"""
import argparse
import importlib.util
import json
import os
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import prof  # noqa: E402

COLS = ['generated code', 'dispatch', 'compile:gadget', 'compile:jit', 'block lookup', 'tlb', 'invalidate',
        'syscall', 'mmap/munmap', 'fork', 'exec', 'exit/mm teardown', 'gadgets', 'signal']


def load_cases():
    spec = importlib.util.spec_from_file_location('bench', HERE / 'guest' / 'bench.py')
    bench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bench)
    return [(n.rstrip('*'), g, c) for n, g, c in bench.CASES]


def case_times(path):
    if not path:
        return {}
    t = defaultdict(list)
    for line in open(path):
        if line.startswith('{'):
            r = json.loads(line)
            t[r['case']].append(r['secs'])
    return {k: statistics.median(v) for k, v in t.items()}


def categories(sample_file):
    cats = prof.Counter()
    leaves, detail = prof.Counter(), prof.Counter()
    stack, counts = [], []
    text = open(sample_file, errors='replace').read().split('Sort by top of stack')[0]
    for line in text.splitlines():
        if prof.THREAD.match(line):
            while stack:
                prof.flush(stack, counts, cats, leaves, detail, '')
            continue
        m = prof.LINE.match(line)
        if not m:
            continue
        depth = len(m.group(1))
        while counts and counts[-1][2] >= depth:
            prof.flush(stack, counts, cats, leaves, detail, '')
        if counts:
            counts[-1][1] += int(m.group(2))
        stack.append((m.group(3).split(' + ')[0].strip(), m.group(4)))
        counts.append([int(m.group(2)), 0, depth])
    while stack:
        prof.flush(stack, counts, cats, leaves, detail, '')
    cats.pop('wait', None)
    return cats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rootfs', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--ish', default=str(HERE.parents[1] / 'build-dev' / 'ish'))
    ap.add_argument('--times', default='', help='jsonl with this ish\'s per-case times (sets the loop counts)')
    ap.add_argument('--only', default='')
    ap.add_argument('--secs', type=float, default=4.0)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    times = case_times(args.times)
    only = set(filter(None, args.only.split(',')))
    rows = []
    for name, group, cmd in load_cases():
        if only and name not in only and group not in only:
            continue
        if group == 'coremark':
            continue
        n = max(1, round(args.secs / times.get(name, 1.0)))
        loop = f'i=0; while [ $i -lt {n} ]; do ( {cmd} ) >/dev/null 2>&1; i=$((i+1)); done'
        p = subprocess.Popen([args.ish, '-r', args.rootfs, '/bin/sh', '-c', loop])
        time.sleep(0.05)
        sf = out / f'{name}.sample.txt'
        subprocess.run(['sample', str(p.pid), str(max(1, int(args.secs * 0.8))), '1', '-file', str(sf)],
                       capture_output=True)
        p.wait()
        cats = categories(sf)
        busy = sum(cats.values()) or 1
        rows.append((name, busy, {k: 100 * v / busy for k, v in cats.items()}))
        print(f'🔎 {name:14} {busy:6} samples', file=sys.stderr, flush=True)

    print('| case | samples | ' + ' | '.join(COLS) + ' | other |')
    print('|' + '---|' * (len(COLS) + 3))
    for name, busy, pct in rows:
        other = 100 - sum(pct.get(c, 0) for c in COLS)
        print(f'| {name} | {busy} | ' + ' | '.join(f'{pct.get(c, 0):.0f}' for c in COLS) + f' | {other:.0f} |')


if __name__ == '__main__':
    main()
