#!/usr/bin/env python3
"""Table of rwrun.py's results: per scenario docker and ish, cold and hot
(median of the hot runs), the ratio and whether it is within 2x; checks that
differ from docker's are flagged.

    rwreport.py <dir> [--ish ish-fm]
"""
import argparse
import glob
import json
import math
import os
import statistics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--ish', default='')
    a = ap.parse_args()
    runs = {}
    for f in glob.glob(os.path.join(a.dir, '*.jsonl')):
        for l in open(f):
            r = json.loads(l)
            runs.setdefault((r['env'], r['case']), []).append(r)
    envs = sorted({e for e, _ in runs})
    ish = a.ish or next(e for e in envs if e != 'docker')
    cases, groups = [], {}
    for (e, c), rs in runs.items():
        if c not in cases:
            cases.append(c)
            groups[c] = rs[0]['group']
    print(f'| scenario | group | docker cold | docker hot | {ish} cold | {ish} hot | hot ratio | cold ratio | ≤2× | check |')
    print('|---|---|---|---|---|---|---|---|---|---|')
    lg = {}
    for c in cases:
        d, i = runs.get(('docker', c), []), runs.get((ish, c), [])
        if not d or not i:
            continue
        def cold(rs): return next((r['secs'] for r in rs if r['cold']), float('nan'))
        def hot(rs): return statistics.median([r['secs'] for r in rs if not r['cold']] or [float('nan')])
        dc, dh, ic, ih = cold(d), hot(d), cold(i), hot(i)
        dchk = {r['check'] for r in d if r['rc'] == 0}
        ichk = {r['check'] for r in i}
        bad = [r for r in i if r['rc'] != 0]
        ok = 'ok' if ichk <= dchk and not bad else f"DIFF {sorted(ichk)} vs {sorted(dchk)}" + (f' rc={bad[0]["rc"]}' if bad else '')
        rh, rc = ih / dh, ic / dc
        lg.setdefault(groups[c], []).append(math.log(rh))
        print(f'| {c} | {groups[c]} | {dc:.3f} | {dh:.3f} | {ic:.3f} | {ih:.3f} | {rh:.2f}× | {rc:.2f}× | '
              f"{'✅' if rh <= 2 else '❌'} | {ok} |")
    print()
    for g, v in lg.items():
        med = math.exp(statistics.median(v))
        print(f'{g}: hot ratio geomean {math.exp(sum(v) / len(v)):.2f}×, median {med:.2f}×, '
              f'{sum(1 for x in v if x <= math.log(2))}/{len(v)} within 2×')


if __name__ == '__main__':
    main()
