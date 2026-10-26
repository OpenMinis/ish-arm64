#!/usr/bin/env python3
"""Run guest/bench.py in each environment, alternating environments per round.

    run.py --rootfs <rootfs> --out <dir> [--envs docker,container,ish-jit,ish-gadget]
           [--rounds 3] [--reps 1] [--only ...] [--skip ...]

Environments:
  docker      OrbStack Docker, image macjit-bench (imported from the rootfs)
  container   Apple container, image macjit-bench (imported from the rootfs)
  ish-jit     <worktree>/build-jit/ish -r <rootfs>
  ish-gadget  <worktree>/build-gadget/ish -r <rootfs>
  ish:NAME=PATH[@VAR=VAL;VAR=VAL]  that ish binary (PATH relative to the worktree),
              labelled ish-NAME, with those variables set
ish environments start a fresh ish per case, since the JIT code region is never
reclaimed and one long session fills it (pass --session to run all cases in one ish).
Each round runs every environment once, so slow drift on the host hits all of them.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

WORKTREE = Path(__file__).resolve().parents[2]
IMAGE = 'macjit-bench'
CONTAINER = '/opt/homebrew/opt/container/bin/container'
BENCH = ['/usr/bin/python3', '/opt/macjit/guest/bench.py']


def env_cmd(env, rootfs):
    if env == 'docker':
        return ['docker', 'run', '--rm', IMAGE] + BENCH
    if env == 'container':
        return [CONTAINER, 'run', '--rm', '-c', '8', '-m', '8G', IMAGE] + BENCH
    if env.startswith('ish:'):
        return [str(WORKTREE / env.split('=', 1)[1].split('@')[0]), '-r', rootfs] + BENCH
    if env.startswith('ish-'):
        return [str(WORKTREE / f'build-{env[4:]}' / 'ish'), '-r', rootfs] + BENCH
    raise SystemExit(f'unknown env {env}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rootfs', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--envs', default='docker,container,ish-jit,ish-gadget')
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--reps', type=int, default=1)
    ap.add_argument('--only', default='')
    ap.add_argument('--skip', default='')
    ap.add_argument('--session', action='store_true', help='one ish for all cases')
    ap.add_argument('--max-load', type=float, default=0,
                    help='before each environment\'s turn, wait until the 1-minute load average is below this')
    ap.add_argument('--tag', default='', help='label ish results <env>-<tag> (e.g. the ISH_* variables set)')
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    envs = args.envs.split(',')
    for rnd in range(args.rounds):
        for env in envs:
            extra = ['--env', env, '--reps', str(args.reps)]
            if args.only:
                extra += ['--only', args.only]
            if args.skip:
                extra += ['--skip', args.skip]
            while args.max_load and os.getloadavg()[0] > args.max_load:
                print(f'⏳ load {os.getloadavg()[0]:.1f} > {args.max_load}, waiting', file=sys.stderr, flush=True)
                time.sleep(30)
            print(f'🔁 round {rnd + 1}/{args.rounds} · {env}', file=sys.stderr, flush=True)
            t0 = time.time()
            cmd = env_cmd(env, args.rootfs)
            is_ish = env.startswith(('ish-', 'ish:'))
            label = env.split('=', 1)[0].replace('ish:', 'ish-')
            if is_ish:
                label += f'-{args.tag}' if args.tag else ''
                label += '-session' if args.session else ''
            extra[1] = label
            batches = [extra]
            if is_ish and not args.session:
                names = subprocess.run(cmd + ['--list'], capture_output=True, text=True).stdout.split()
                batches = [extra + ['--only', n] for n in names
                           if (not args.only or n in args.only.split(','))
                           and n not in args.skip.split(',')]
                batches = [[a for a in b if a] for b in batches]
            rc = 0
            with open(out / f'{label}.jsonl', 'a') as f:
                run_env = dict(os.environ)
                if env.startswith('ish:') and '@' in env:
                    run_env.update(kv.split('=', 1) for kv in env.split('@', 1)[1].split(';'))
                for b in batches:
                    rc |= subprocess.run(cmd + b, stdout=f, env=run_env).returncode
            print(f'   {label} done in {time.time() - t0:.0f}s rc={rc}', file=sys.stderr, flush=True)


if __name__ == '__main__':
    main()
