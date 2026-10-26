#!/usr/bin/env python3
"""Summarize run.py results: median seconds (or CoreMark score) per case and environment.

    report.py <results dir> [--base docker]

Ratios are base_time / env_time for timed cases (higher = faster than base) and
env_score / base_score for CoreMark, so ">1" always means faster than the base.
Outputs that differ from the base (deterministic cases only) are flagged with !.
"""
import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path

ORDER = ['docker', 'container', 'ish-jit', 'ish-jit-session', 'ish-gadget', 'macos']


def load(d):
    runs = defaultdict(list)
    meta = {}
    for f in sorted(Path(d).glob('*.jsonl')):
        for line in f.read_text().splitlines():
            if not line.startswith('{'):
                continue
            r = json.loads(line)
            runs[(r['case'], r['env'])].append(r)
            meta[r['case']] = (r['group'], r['det'])
    return runs, meta


def value(rs):
    ok = [r for r in rs if r['rc'] == 0]
    if not ok:
        return None
    if 'score' in ok[0]:
        return statistics.median(r['score'] for r in ok if r.get('score'))
    return statistics.median(r['secs'] for r in ok)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--base', default='docker')
    args = ap.parse_args()
    runs, meta = load(args.dir)
    seen = list(dict.fromkeys(k[1] for k in runs))
    envs = [e for e in ORDER if e in seen] + sorted(e for e in seen if e not in ORDER)
    cases = list(dict.fromkeys(k[0] for k in runs))

    head = ['case', 'group'] + envs + [f'{e} vs {args.base}' for e in envs if e != args.base]
    print('| ' + ' | '.join(head) + ' |')
    print('|' + '---|' * len(head))
    geo = defaultdict(list)
    for case in cases:
        group, det = meta[case]
        is_cm = group == 'coremark'
        base_rs = runs.get((case, args.base), [])
        base_md5 = {r['md5'] for r in base_rs}
        base_v = value(base_rs)
        row = [case, group]
        for e in envs:
            rs = runs.get((case, e), [])
            v = value(rs)
            if v is None:
                row.append('FAIL' if rs else '–')
                continue
            cell = f'{v:.0f}' if is_cm else f'{v:.2f}s'
            if det and base_md5 and e != args.base and {r['md5'] for r in rs} - base_md5:
                cell += ' !'
            row.append(cell)
        for e in envs:
            if e == args.base:
                continue
            v = value(runs.get((case, e), []))
            if v is None or base_v is None:
                row.append('–')
                continue
            ratio = v / base_v if is_cm else base_v / v
            geo[(e, group)].append(ratio)
            geo[(e, 'all')].append(ratio)
            row.append(f'{ratio:.2f}×')
        print('| ' + ' | '.join(row) + ' |')

    print(f'\nGeometric mean of speed relative to {args.base} (>1 = faster):\n')
    groups = list(dict.fromkeys(g for g, _ in meta.values())) + ['all']
    print('| env | ' + ' | '.join(groups) + ' |')
    print('|' + '---|' * (len(groups) + 1))
    for e in envs:
        if e == args.base:
            continue
        cells = []
        for g in groups:
            xs = geo.get((e, g))
            cells.append(f'{statistics.geometric_mean(xs):.2f}×' if xs else '–')
        print(f'| {e} | ' + ' | '.join(cells) + ' |')


if __name__ == '__main__':
    main()
