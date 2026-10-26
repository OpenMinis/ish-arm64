#!/usr/bin/env python3
"""Phase-4 scenarios: real Python / Node / shell work, run unchanged inside an
environment (docker, ish). One JSON line per run: the first run of a scenario
is "cold" (just after the environment started), the next ones "hot".

    rw.py [--reps 3] [--only name,name] [--skip name,name] [--env label] [--list]

Every scenario has a check (an md5 of its normalized output, a test count)
so the environments can be compared on what they computed, not just on time.
Inputs live in /opt/macjit/rw (gen_rw.py, plus the packages installed there).
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import time

RW = '/opt/macjit/rw'
NA = f'{RW}/node-app'
NM = f'{NA}/node_modules'
SRC = f'{RW}/src'

# (name, group, setup (untimed, before every run), command, check: 'md5' of
# stdout, or a regex whose first group is kept). Commands run with sh -c.
S = [
    # ---- Python
    ('py_c_pass', 'python', '', 'for i in 1 2 3 4 5; do python3 -c pass; done', 'md5'),
    ('py_json_tool', 'python', '', f'head -c 2000000 {RW}/data/events.json | python3 -m json.tool --compact | md5sum', 'md5'),
    ('pip_version', 'python', '', 'pip3 --version | cut -d" " -f1-2', 'md5'),
    ('pip_list', 'python', '', 'pip3 list --disable-pip-version-check 2>/dev/null | wc -l', 'md5'),
    ('pip_install', 'python', 'rm -rf /tmp/rwv',
     f'python3 -m venv /tmp/rwv && /tmp/rwv/bin/pip install -q --no-index --find-links {RW}/wheels '
     'requests jinja2 click pyyaml rich && /tmp/rwv/bin/python -c "import requests, jinja2, click, yaml, rich, markupsafe; '
     'print(yaml.__version__, markupsafe.__name__)"', 'md5'),
    ('pytest', 'python', f'rm -rf {RW}/pyproj/.pytest_cache',
     f'cd {RW}/pyproj && python3 -m pytest -q -p no:cacheprovider --basetemp=/tmp/rwpt tests 2>&1 | tail -1', r'(\d+ passed)'),
    ('py_csv', 'python', '', f'python3 {RW}/py/csv_agg.py', 'md5'),
    ('py_json', 'python', '', f'python3 {RW}/py/json_agg.py', 'md5'),
    ('py_sqlite', 'python', '', f'python3 {RW}/py/sqlite_bulk.py', 'md5'),
    ('py_re_log', 'python', '', f'python3 {RW}/py/re_log.py', 'md5'),
    ('flask_5k', 'python', '', f'python3 {RW}/py/web_bench.py flask 5000', r'(md5=\w+)'),
    ('ruff', 'python', '', f'ruff check --no-cache --select E,F,W --quiet {RW}/pycode | wc -l', 'md5'),
    ('black', 'python', '', f'black --check -q {RW}/pycode 2>&1 | wc -l', 'md5'),
    # ---- Node
    ('node_e0', 'node', '', 'for i in 1 2 3 4 5; do node -e 0; done', 'md5'),
    ('npm_version', 'node', '', 'npm --version', 'md5'),
    ('npx_local', 'node', '', f'cd {NA} && npx --offline prettier --version', 'md5'),
    ('npm_ci', 'node', f'rm -rf /tmp/rwnpm && mkdir /tmp/rwnpm && cp {NA}/package.json {NA}/package-lock.json /tmp/rwnpm/',
     'cd /tmp/rwnpm && npm ci --offline --no-audit --no-fund --loglevel=error >/dev/null && ls node_modules | wc -l', 'md5'),
    ('tsc', 'node', f'rm -rf {RW}/tsproj/out', f'cd {RW}/tsproj && node {NM}/typescript/bin/tsc -p . && ls out | wc -l', 'md5'),
    ('esbuild', 'node', '',
     f'cd {RW}/tsproj && {NM}/.bin/esbuild src/index.ts --bundle --platform=node --log-level=warning | md5sum', 'md5'),
    ('jest', 'node', '', f'cd {RW}/js/jest && node {NM}/jest/bin/jest.js --ci 2>&1 | grep -E "^Tests:"', r'(\d+ passed)'),
    ('node_test', 'node', '', f'cd {RW}/js/nodetest && node --test 2>&1 | grep -E "^# pass"', r'(# pass \d+)'),
    ('express_5k', 'node', '', f'python3 {RW}/py/web_bench.py express 5000', r'(md5=\w+)'),
    ('eslint', 'node', '',
     f'cd {RW}/tsproj && node {NM}/eslint/bin/eslint.js --no-config-lookup -c eslint.config.mjs {NM}/express/lib '
     f'{NM}/fastify/lib 2>&1 | tail -1', 'md5'),
    ('prettier', 'node', '', f'cd {RW}/tsproj && node {NM}/prettier/bin/prettier.cjs --check src 2>&1 | tail -1', 'md5'),
    # ---- Shell / development
    ('git_clone', 'shell', 'rm -rf /tmp/rwgit', f'git clone -q {SRC}/curl.git /tmp/rwgit && git -C /tmp/rwgit rev-parse HEAD', 'md5'),
    ('git_ops', 'shell', f'[ -d /tmp/rwgit ] || git clone -q {SRC}/curl.git /tmp/rwgit',
     'cd /tmp/rwgit && git checkout -q master 2>/dev/null || git checkout -q main; git status --short | wc -l; '
     'git log --oneline | wc -l; git diff --stat curl-8_10_0 curl-8_11_0 | tail -1; '
     'git checkout -q curl-8_10_0 && git rev-parse HEAD', 'md5'),
    ('make_lua', 'shell', f'rm -rf /tmp/rwlua && mkdir /tmp/rwlua && tar xzf {SRC}/lua-5.4.7.tar.gz -C /tmp/rwlua',
     'cd /tmp/rwlua/lua-5.4.7 && make -s linux >/dev/null 2>&1 && src/lua -e "print(1 << 40, math.pi)"', 'md5'),
    ('gcc_sqlite', 'shell', f'rm -rf /tmp/rwsq && mkdir /tmp/rwsq && cd /tmp/rwsq && unzip -q {SRC}/sqlite-amalgamation-3490100.zip',
     'cd /tmp/rwsq/sqlite-amalgamation-3490100 && gcc -O1 -c sqlite3.c -o sqlite3.o && size sqlite3.o | tail -1 | cut -f1-3', 'md5'),
    ('find_tree', 'shell', '', f'find {RW}/tree -type f | wc -l', 'md5'),
    ('grep_tree', 'shell', '', f'grep -r -l "import" {RW}/tree | wc -l', 'md5'),
    ('tar_gz_tree', 'shell', '', f'tar cf - -C {RW}/tree py1 | gzip -1 | wc -c | cut -c1-2', 'md5'),
    ('sort_log', 'shell', '', f'sort -k9,9 -k1,1 {RW}/data/access.log | md5sum', 'md5'),
    ('loop_awk_sed', 'shell', '',
     f'n=0; for i in $(seq 300); do n=$((n + i)); done; echo $n; '
     f'awk \'{{s[$9] += $10}} END {{for (k in s) print k, s[k]}}\' {RW}/data/access.log | sort; '
     f'sed -n "s/.*\\"\\(GET\\|POST\\) \\([^ ]*\\) .*/\\2/p" {RW}/data/access.log | cut -d/ -f3 | sort | uniq -c | sort -rn | head -3', 'md5'),
    ('jq', 'shell', '', f'jq -c "select(.lvl == \\"error\\") | {{svc, ms}}" {RW}/data/events.jsonl | jq -s "group_by(.svc) | map({{svc: .[0].svc, n: length, ms: (map(.ms) | add)}})" | md5sum', 'md5'),
]


def run(setup, cmd):
    if setup:
        subprocess.run(['/bin/sh', '-c', setup], capture_output=True)
    t0 = time.perf_counter()
    p = subprocess.run(['/bin/sh', '-c', cmd], capture_output=True)
    return time.perf_counter() - t0, p


def check(kind, out):
    if kind == 'md5':
        return hashlib.md5(out).hexdigest()[:8]
    m = re.search(kind.encode(), out)
    return m.group(1).decode() if m else 'NOMATCH'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--only', default='')
    ap.add_argument('--skip', default='')
    ap.add_argument('--env', default='?')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    only = set(filter(None, args.only.split(',')))
    skip = set(filter(None, args.skip.split(',')))
    for name, group, setup, cmd, kind in S:
        if (only and name not in only) or name in skip:
            continue
        if args.list:
            print(name, group)
            continue
        for rep in range(args.reps + 1):
            secs, p = run(setup, cmd)
            rec = {'env': args.env, 'case': name, 'group': group, 'cold': rep == 0, 'rep': rep,
                   'secs': round(secs, 4), 'rc': p.returncode, 'check': check(kind, p.stdout)}
            if p.returncode:
                rec['err'] = p.stderr.decode(errors='replace')[-300:]
            print(json.dumps(rec), flush=True)
            print(f"  {args.env:10} {name:14} {'cold' if rep == 0 else 'hot '} {secs:8.3f}s rc={p.returncode} "
                  f"{rec['check']}", file=sys.stderr, flush=True)


if __name__ == '__main__':
    main()
