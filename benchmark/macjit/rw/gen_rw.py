#!/usr/bin/env python3
"""Write the phase-4 (real-world) inputs into <rootfs>/opt/macjit/rw: projects,
data files and a file tree. Deterministic (fixed seeds), so ish and docker,
which run from images of the same rootfs, see the same bytes.

    gen_rw.py <rootfs>
"""
import json
import os
import random
import shutil
import sys

root = sys.argv[1]
RW = os.path.join(root, 'opt/macjit/rw')
rnd = random.Random(1234)


def write(rel, text):
    p = os.path.join(RW, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w') as f:
        f.write(text)


# ---- P3: pytest project, 300 tests with fixtures
mods = []
for m in range(10):
    funcs = []
    for k in range(10):
        funcs.append(f'''
def f{k}(xs, scale={k + 1}):
    out = {{}}
    for i, x in enumerate(xs):
        key = (x * scale + i) % 17
        out[key] = out.get(key, 0) + x
    return sorted(out.items())
''')
    write(f'pyproj/pkg/mod{m}.py', ''.join(funcs))
write('pyproj/pkg/__init__.py', '')
write('pyproj/tests/conftest.py', '''
import pytest

@pytest.fixture
def data():
    return list(range(200))

@pytest.fixture(scope="module")
def table(tmp_path_factory):
    p = tmp_path_factory.mktemp("t") / "t.txt"
    p.write_text("\\n".join(str(i) for i in range(1000)))
    return p
''')
for m in range(10):
    tests = [f'import pkg.mod{m} as M\nimport pytest\n']
    for k in range(10):
        tests.append(f'''
def test_f{k}_sum(data):
    assert sum(v for _, v in M.f{k}(data)) == sum(data)

def test_f{k}_keys(data):
    assert all(0 <= k < 17 for k, _ in M.f{k}(data))

@pytest.mark.parametrize("n", [3, 7])
def test_f{k}_table(table, n):
    xs = [int(l) for l in table.read_text().split()][: n * 10]
    assert len(M.f{k}(xs)) <= 17
''')
    write(f'pyproj/tests/test_mod{m}.py', ''.join(tests))
write('pyproj/pytest.ini', '[pytest]\npythonpath = .\n')

# ---- P4: data
os.makedirs(os.path.join(RW, 'data'), exist_ok=True)
cities = ['tokyo', 'paris', 'lima', 'oslo', 'cairo', 'delhi', 'quito', 'perth', 'dakar', 'riga']
with open(os.path.join(RW, 'data/sales.csv'), 'w') as f:
    f.write('id,city,product,qty,price,ts\n')
    for i in range(500000):
        f.write(f'{i},{rnd.choice(cities)},p{rnd.randrange(300)},{rnd.randrange(1, 20)},'
                f'{rnd.randrange(100, 100000) / 100:.2f},{1700000000 + i * 7}\n')
recs = [{'id': i, 'user': f'u{rnd.randrange(5000)}', 'tags': [rnd.choice(cities) for _ in range(3)],
         'score': rnd.random(), 'nested': {'a': rnd.randrange(100), 'b': [rnd.randrange(10) for _ in range(5)]}}
        for i in range(150000)]
with open(os.path.join(RW, 'data/events.json'), 'w') as f:
    json.dump(recs, f)
methods = ['GET', 'POST', 'PUT', 'DELETE']
with open(os.path.join(RW, 'data/access.log'), 'w') as f:
    for i in range(300000):
        f.write(f'10.{rnd.randrange(256)}.{rnd.randrange(256)}.{rnd.randrange(256)} - - '
                f'[26/Oct/2026:{rnd.randrange(24):02d}:{rnd.randrange(60):02d}:{rnd.randrange(60):02d} +0000] '
                f'"{rnd.choice(methods)} /api/v{rnd.randrange(3)}/item/{rnd.randrange(10000)} HTTP/1.1" '
                f'{rnd.choice([200, 200, 200, 301, 404, 500])} {rnd.randrange(100, 50000)} '
                f'"-" "agent/{rnd.randrange(20)}" rt={rnd.random():.4f}\n')
with open(os.path.join(RW, 'data/events.jsonl'), 'w') as f:
    for i in range(100000):
        f.write(json.dumps({'id': i, 'lvl': rnd.choice(['info', 'warn', 'error']),
                            'svc': rnd.choice(cities), 'ms': rnd.randrange(2000)}) + '\n')

write('py/csv_agg.py', '''import csv, collections, hashlib, sys
tot = collections.defaultdict(float); cnt = collections.Counter()
with open("/opt/macjit/rw/data/sales.csv", newline="") as f:
    for r in csv.DictReader(f):
        tot[(r["city"], r["product"])] += int(r["qty"]) * float(r["price"])
        cnt[r["city"]] += 1
out = "\\n".join(f"{k[0]},{k[1]},{v:.2f}" for k, v in sorted(tot.items())) + str(sorted(cnt.items()))
print(hashlib.md5(out.encode()).hexdigest())
''')
write('py/json_agg.py', '''import json, collections, hashlib
recs = json.load(open("/opt/macjit/rw/data/events.json"))
by = collections.defaultdict(list)
for r in recs:
    for t in r["tags"]:
        by[t].append(r["score"] + r["nested"]["a"] + sum(r["nested"]["b"]))
out = json.dumps({k: [len(v), round(sum(v), 3)] for k, v in sorted(by.items())})
print(hashlib.md5(out.encode()).hexdigest())
''')
write('py/sqlite_bulk.py', '''import sqlite3, hashlib, os, random
p = "/tmp/rw_bulk.db"
if os.path.exists(p): os.remove(p)
db = sqlite3.connect(p); rnd = random.Random(7)
db.execute("create table t(id integer primary key, k text, v real, n int)")
db.executemany("insert into t(k, v, n) values (?, ?, ?)",
               ((f"k{rnd.randrange(1000)}", rnd.random(), rnd.randrange(100)) for _ in range(200000)))
db.commit()
db.execute("create index ik on t(k)")
rows = db.execute("select k, count(*), round(sum(v), 4), max(n) from t group by k order by k").fetchall()
for i in range(2000):
    db.execute("select count(*) from t where k = ? and n > ?", (f"k{i % 1000}", i % 100)).fetchone()
db.close(); os.remove(p)
print(hashlib.md5(repr(rows).encode()).hexdigest())
''')
write('py/re_log.py', '''import re, collections, hashlib
pat = re.compile(r'^(\\S+) \\S+ \\S+ \\[([^]]+)\\] "(\\w+) (\\S+) [^"]*" (\\d{3}) (\\d+) "[^"]*" "([^"]*)" rt=([\\d.]+)$')
st = collections.Counter(); by = collections.defaultdict(float)
for line in open("/opt/macjit/rw/data/access.log"):
    m = pat.match(line)
    if m:
        st[(m.group(3), m.group(5))] += 1
        by[m.group(4).split("/")[2]] += float(m.group(8))
print(hashlib.md5((repr(sorted(st.items())) + repr(sorted((k, round(v, 3)) for k, v in by.items()))).encode()).hexdigest())
''')

# ---- P5: flask app + client
write('py/flask_app.py', '''from flask import Flask, jsonify
import sys
app = Flask(__name__)
@app.route("/item/<int:i>")
def item(i):
    return jsonify(id=i, sq=i * i, name=f"item{i}")
if __name__ == "__main__":
    import logging; logging.getLogger("werkzeug").setLevel(logging.ERROR)
    app.run(host="127.0.0.1", port=int(sys.argv[1]), threaded=False)
''')
write('py/web_bench.py', '''# start a server (argv: kind), time to first response, then N requests on one keep-alive connection
import http.client, json, subprocess, sys, time, hashlib
kind, n = sys.argv[1], int(sys.argv[2])
port = {"flask": 18080, "express": 18081}[kind]
cmd = {"flask": ["python3", "/opt/macjit/rw/py/flask_app.py", str(port)],
       "express": ["node", "/opt/macjit/rw/js/express_app.js", str(port)]}[kind]
t0 = time.time()
srv = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
while True:
    try:
        c = http.client.HTTPConnection("127.0.0.1", port, timeout=10); c.request("GET", "/item/1"); c.getresponse().read(); break
    except OSError:
        time.sleep(0.02)
        if time.time() - t0 > 120: srv.kill(); sys.exit("no server")
first = time.time() - t0
h = hashlib.md5(); t1 = time.time()
c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
for i in range(n):
    c.request("GET", f"/item/{i}")
    r = c.getresponse(); body = r.read()
    if r.getheader("connection", "").lower() == "close" or r.version == 10:
        c.close(); c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    h.update(json.dumps(json.loads(body), sort_keys=True).encode())
dt = time.time() - t1
srv.kill(); srv.wait()
print(f"first_response_s={first:.3f} req_per_s={n / dt:.1f} md5={h.hexdigest()}")
''')

# ---- P6: a mid-size real code base: part of the stdlib
src = os.path.join(root, 'usr/lib/python3.12')
dst = os.path.join(RW, 'pycode')
if os.path.exists(dst):
    shutil.rmtree(dst)
for d in ['email', 'json', 'http', 'asyncio', 'logging', 'xml', 'urllib', 'importlib', 'concurrent', 'unittest']:
    shutil.copytree(os.path.join(src, d), os.path.join(dst, d),
                    ignore=shutil.ignore_patterns('__pycache__', 'test', 'tests'))

# ---- N3/N6: a TypeScript project (120 modules)
for i in range(120):
    deps = [f'm{j}' for j in sorted(rnd.sample(range(i), min(i, 3)))]
    imports = ''.join(f"import {{ calc as {d} }} from './{d}';\n" for d in deps)
    body = ''.join(f'  acc += {d}(n - 1, opts);\n' for d in deps)
    write(f'tsproj/src/m{i}.ts', f'''{imports}
export interface Opts{i} {{ scale: number; label?: string; tags: string[] }}
export class Model{i} {{
  private items: Map<string, number> = new Map();
  constructor(private readonly opts: Opts{i}) {{}}
  add(key: string, v: number): this {{ this.items.set(key, (this.items.get(key) ?? 0) + v * this.opts.scale); return this; }}
  top(k: number): Array<[string, number]> {{ return [...this.items.entries()].sort((a, b) => b[1] - a[1]).slice(0, k); }}
  toJSON(): Record<string, unknown> {{ return {{ label: this.opts.label ?? 'm{i}', n: this.items.size, tags: this.opts.tags }}; }}
}}
export function calc(n: number, opts: {{ scale: number }}): number {{
  if (n <= 0) return opts.scale;
  let acc = {i};
{body}  return acc % 1000003;
}}
export const NAME_{i} = `module-${{{i}}}`;
''')
write('tsproj/src/index.ts', ''.join(f"export * as m{i} from './m{i}';\n" for i in range(120)))
write('tsproj/tsconfig.json', json.dumps({'compilerOptions': {'target': 'ES2020', 'module': 'commonjs', 'strict': True,
                                                              'outDir': 'out', 'declaration': True, 'skipLibCheck': True},
                                          'include': ['src']}, indent=2))
write('tsproj/eslint.config.mjs', 'export default [{ files: ["**/*.js"], rules: { "no-unused-vars": "warn", "eqeqeq": "error", "no-var": "error" } }];\n')

# ---- N4: jest and node:test, 200 cases each
for f in range(10):
    cases = ''.join(f'''
test('case {f}-{k}', () => {{
  const xs = Array.from({{ length: 300 }}, (_, i) => (i * {k + 3}) % 97);
  const m = new Map(); for (const x of xs) m.set(x % 13, (m.get(x % 13) || 0) + x);
  expect([...m.values()].reduce((a, b) => a + b, 0)).toBe(xs.reduce((a, b) => a + b, 0));
}});
''' for k in range(20))
    write(f'js/jest/t{f}.test.js', cases)
    write(f'js/nodetest/t{f}.test.js', "const test = require('node:test'); const assert = require('node:assert');\n" +
          cases.replace('expect(', 'assert.strictEqual(').replace(').toBe(', ', '))
write('js/jest/jest.config.js', 'module.exports = { testEnvironment: "node", cache: false };\n')
write('js/express_app.js', '''const express = require('/opt/macjit/rw/node-app/node_modules/express');
const app = express();
app.get('/item/:i', (req, res) => { const i = +req.params.i; res.json({ id: i, sq: i * i, name: `item${i}` }); });
app.listen(+process.argv[2], '127.0.0.1');
''')

# ---- S4: logs for shell pipelines (access.log above) and a jq input (events.jsonl)
print('ok', RW)
