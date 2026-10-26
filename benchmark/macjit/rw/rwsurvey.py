#!/usr/bin/env python3
"""Where each phase-4 scenario spends its time in ish: run it in a loop in
one ish while macOS `sample` records it, split the samples by prof.py's
categories (busy samples only; "wait" is shown apart).

    rwsurvey.py --rootfs <rootfs> --out <dir> [--ish bins/ish-X] [--only a,b] [--secs 6]
"""
import argparse
import importlib.util
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKTREE = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import prof  # noqa: E402

COLS = ['generated code', 'dispatch', 'compile:gadget', 'compile:jit', 'block lookup', 'tlb', 'invalidate',
        'syscall:fs', 'syscall', 'mmap/munmap', 'fork', 'exec', 'exit/mm teardown', 'gadgets', 'signal']


def categories(path):
    return prof.analyze(path)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rootfs', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--ish', default='build-dev/ish')
    ap.add_argument('--only', default='')
    ap.add_argument('--secs', type=int, default=6)
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location('rw', HERE / 'rw.py')
    rw = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rw)
    only = set(filter(None, a.only.split(',')))
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    print('| scenario | busy samples | wait | ' + ' | '.join(COLS) + ' | other |')
    print('|---|---|---|' + '---|' * (len(COLS) + 1))
    for name, group, setup, cmd, kind in rw.S:
        if only and name not in only:
            continue
        loop = f'while true; do {setup or ":"}; {cmd}; done > /dev/null 2>&1'
        p = subprocess.Popen([str(WORKTREE / a.ish), '-r', a.rootfs, '/bin/sh', '-c', loop],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.5)
        f = out / f'{name}.txt'
        subprocess.run(['sample', str(p.pid), str(a.secs), '1', '-mayDie', '-file', str(f)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        p.kill(); p.wait()
        subprocess.run(['pkill', '-9', '-f', f'{a.ish} -r {a.rootfs}'], stderr=subprocess.DEVNULL)
        c = categories(f)
        busy = sum(v for k, v in c.items() if k != 'wait')
        if not busy:
            print(f'| {name} | 0 | | ' + ' | '.join('' for _ in COLS) + ' | |')
            continue
        other = busy - sum(c.get(k, 0) for k in COLS)
        print(f"| {name} | {busy} | {c.get('wait', 0) / (busy + c.get('wait', 0)) * 100:.0f}% | " +
              ' | '.join(f'{100 * c.get(k, 0) / busy:.0f}' for k in COLS) + f' | {100 * other / busy:.0f} |', flush=True)


if __name__ == '__main__':
    main()
