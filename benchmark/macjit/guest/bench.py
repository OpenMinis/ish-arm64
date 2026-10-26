#!/usr/bin/env python3
"""Run the macjit cases inside one environment and print one JSON line per run.

Runs unchanged in Docker, Apple container and ish, so every environment pays
the same harness cost. Progress goes to stderr, results to stdout.

    bench.py [--reps N] [--only name,name] [--skip name,name] [--env label] [--list]
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import time

G = '/opt/macjit/guest'
D = '/opt/macjit/data'
CM = '/opt/macjit/bin'

# (name, group, shell command). Outputs are hashed so environments can be
# checked against each other; cases marked * print nondeterministic output.
CASES = [
    ('sh_loop', 'shell', 'i=0; s=0; while [ $i -lt 200000 ]; do s=$((s+i)); i=$((i+1)); done; echo $s'),
    ('fork_exec', 'shell', 'for i in $(seq 300); do /bin/true; done'),
    ('grep_r', 'shell', 'grep -r -c import /usr/lib/python3.12 | wc -l'),
    ('find', 'shell', 'find /usr/lib /usr/share -type f | wc -l'),
    ('sort_uniq', 'shell', f'sort {D}/words.txt | uniq -c | sort -rn | head -3'),
    ('sed_awk', 'shell', f"sed 's/HTTP/H/' {D}/access.log | awk '{{c[$9]++}} END{{for (k in c) print k, c[k]}}' | sort"),
    ('sha256', 'shell', f'sha256sum {D}/blob.bin'),
    ('gzip', 'shell', f'gzip -6 -c {D}/access.log | wc -c'),
    ('tar_gz*', 'shell', 'tar czf - -C /usr/lib python3.12 | wc -c'),
    ('py_startup', 'python', 'for i in $(seq 20); do python3 -c pass; done'),
    ('py_import', 'python', 'for i in $(seq 5); do python3 -c "import json, re, asyncio, email.parser, '
                            'http.client, sqlite3, decimal, argparse, subprocess, urllib.request, ssl, hashlib"; done'),
    ('py_json', 'python', f'python3 {G}/py_data.py json'),
    ('py_regex', 'python', f'python3 {G}/py_data.py regex'),
    ('py_sqlite', 'python', f'python3 {G}/py_data.py sqlite'),
    ('py_nbody', 'python', f'python3 {G}/py_compute.py nbody'),
    ('py_fannkuch', 'python', f'python3 {G}/py_compute.py fannkuch'),
    ('py_spectral', 'python', f'python3 {G}/py_compute.py spectral'),
    ('pip_list', 'python', 'python3 -m pip list --disable-pip-version-check'),
    ('node_startup', 'node', 'for i in $(seq 10); do node -e 0; done'),
    ('node_json', 'node', f'node {G}/node_bench.js json'),
    ('node_regex', 'node', f'node {G}/node_bench.js regex'),
    ('node_compute', 'node', f'node {G}/node_bench.js compute'),
    ('node_fswalk', 'node', f'node {G}/node_bench.js fswalk'),
    ('coremark_1t*', 'coremark', f'{CM}/coremark_st'),
    ('coremark_4t*', 'coremark', f'{CM}/coremark_mt4'),
]


def run_case(cmd):
    t0 = time.perf_counter()
    p = subprocess.run(['/bin/sh', '-c', cmd], capture_output=True)
    return time.perf_counter() - t0, p


def coremark_score(out):
    m = re.search(rb'CoreMark 1\.0 : ([0-9.]+)', out)
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--only', default='')
    ap.add_argument('--skip', default='')
    ap.add_argument('--env', default='?')
    ap.add_argument('--list', action='store_true', help='print the case names and exit')
    args = ap.parse_args()
    if args.list:
        print(' '.join(n.rstrip('*') for n, _, _ in CASES))
        return
    only = set(filter(None, args.only.split(',')))
    skip = set(filter(None, args.skip.split(',')))

    for raw_name, group, cmd in CASES:
        name = raw_name.rstrip('*')
        if (only and name not in only and group not in only) or name in skip or group in skip:
            continue
        # CoreMark calibrates itself to >= 10 s: no warm-up, one run.
        reps = 1 if group == 'coremark' else args.reps
        if group != 'coremark':
            run_case(cmd)
        for rep in range(reps):
            secs, p = run_case(cmd)
            rec = {'env': args.env, 'case': name, 'group': group, 'rep': rep, 'secs': round(secs, 4),
                   'rc': p.returncode, 'md5': hashlib.md5(p.stdout).hexdigest()[:8],
                   'det': not raw_name.endswith('*')}
            if group == 'coremark':
                rec['score'] = coremark_score(p.stdout)
            if p.returncode != 0:
                rec['err'] = p.stderr.decode(errors='replace')[-300:]
            print(json.dumps(rec), flush=True)
            print(f'  {args.env:10} {name:14} {secs:8.3f}s rc={p.returncode}'
                  + (f" score={rec['score']}" if group == 'coremark' else ''), file=sys.stderr, flush=True)


if __name__ == '__main__':
    main()
