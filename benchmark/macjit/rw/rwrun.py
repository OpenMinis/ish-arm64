#!/usr/bin/env python3
"""Run rw.py's scenarios in docker and ish, a fresh environment per scenario
(so its first run is cold), alternating environments scenario by scenario.

    rwrun.py --rootfs <rootfs> --out <dir> [--envs docker,ish:NAME=PATH[@VAR=VAL;...]]
             [--reps 3] [--only ...] [--skip ...] [--max-load 10]

docker runs the image macjit-rw (gen_rw.py's rootfs, imported) with rw.py
mounted in; ish runs the same rootfs with -r. report: rwreport.py.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKTREE = HERE.parents[2]
GUEST = '/opt/macjit/rw/rw.py'


def wait_load(limit):
    while limit and os.getloadavg()[0] > limit:
        time.sleep(10)


def cmd_for(env, rootfs, args):
    tail = ['/usr/bin/python3', GUEST] + args
    if env == 'docker':
        return ['docker', 'run', '--rm', '-v', f'{HERE / "rw.py"}:{GUEST}:ro', 'macjit-rw'] + tail, None
    name, spec = env[4:].split('=', 1)
    path, _, vars_ = spec.partition('@')
    envv = dict(os.environ)
    for kv in filter(None, vars_.split(';')):
        k, v = kv.split('=', 1)
        envv[k] = v
    return [str(WORKTREE / path), '-r', rootfs] + tail, envv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rootfs', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--envs', default='docker,ish:fm=build-dev/ish')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--only', default='')
    ap.add_argument('--skip', default='')
    ap.add_argument('--max-load', type=float, default=10)
    ap.add_argument('--timeout', type=int, default=900)
    a = ap.parse_args()
    # rw.py inside the rootfs as well, for ish
    dst = Path(a.rootfs) / GUEST.lstrip('/')
    dst.write_bytes((HERE / 'rw.py').read_bytes())
    names = subprocess.run([sys.executable, str(HERE / 'rw.py'), '--list'] +
                           (['--only', a.only] if a.only else []) + (['--skip', a.skip] if a.skip else []),
                           capture_output=True, text=True).stdout.split('\n')
    names = [n.split()[0] for n in names if n.strip()]
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    envs = a.envs.split(',')
    files = {e: open(out / f"{'docker' if e == 'docker' else 'ish-' + e[4:].split('=')[0]}.jsonl", 'a') for e in envs}
    for name in names:
        for env in envs:
            label = 'docker' if env == 'docker' else 'ish-' + env[4:].split('=')[0]
            wait_load(a.max_load)
            cmd, envv = cmd_for(env, a.rootfs, ['--only', name, '--reps', str(a.reps), '--env', label])
            t0 = time.time()
            try:
                p = subprocess.run(cmd, env=envv, capture_output=True, text=True, timeout=a.timeout)
                lines, err = p.stdout, p.stderr
            except subprocess.TimeoutExpired as e:
                lines, err = (e.stdout or b'').decode() if isinstance(e.stdout, bytes) else (e.stdout or ''), 'TIMEOUT'
            files[env].write(''.join(l + '\n' for l in lines.splitlines() if l.startswith('{')))
            files[env].flush()
            sys.stderr.write(''.join(l + '\n' for l in err.splitlines() if l.startswith('  ')) or f'  {label} {name} {err[-200:]}\n')
            sys.stderr.write(f'   {label} {name} done in {time.time() - t0:.0f}s\n')
            sys.stderr.flush()


if __name__ == '__main__':
    main()
