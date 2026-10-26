"""Everyday Python data work: JSON, regex log parsing, sqlite."""
import json
import re
import sqlite3
import sys
from collections import Counter

DATA = '/opt/macjit/data'
which = sys.argv[1]

if which == 'json':
    raw = open(f'{DATA}/data.json').read()
    for _ in range(3):
        obj = json.loads(raw)
        raw2 = json.dumps(obj, sort_keys=True)
    print(len(obj), len(raw2), sum(1 for r in obj if r['active']))

elif which == 'regex':
    pat = re.compile(r'^(\S+) \S+ \S+ \[([^\]]+)\] "(\w+) ([^ ?]+)[^"]*" (\d{3}) (\d+)')
    status, paths, total = Counter(), Counter(), 0
    with open(f'{DATA}/access.log') as f:
        for line in f:
            m = pat.match(line)
            if not m:
                continue
            status[m.group(5)] += 1
            paths[m.group(4)] += 1
            total += int(m.group(6))
    print(total, sorted(status.items()), paths.most_common(3))

elif which == 'sqlite':
    db = sqlite3.connect(':memory:')
    db.execute('create table t (id integer primary key, k text, v real)')
    db.executemany('insert into t (k, v) values (?, ?)',
                   ((f'k{i % 997}', i * 0.5) for i in range(200000)))
    db.execute('create index ik on t(k)')
    rows = db.execute('select k, count(*), sum(v) from t group by k order by 3 desc limit 3').fetchall()
    n = sum(db.execute('select count(*) from t where k = ?', (f'k{i}',)).fetchone()[0] for i in range(2000))
    print(rows, n)
