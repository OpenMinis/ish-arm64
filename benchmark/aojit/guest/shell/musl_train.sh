# musl recording workload: one ish run over pip, matplotlib, python stdlib,
# node, the busybox pipeline, bzip2 and self-modifying code, so every
# translation shares one registry (consistent module-context indices).
python3 -m pip list > /dev/null
python3 /tmp/aojit/charts/mpl_chart.py
python3 -c "
import hashlib,json,zlib,re,sqlite3
d=json.dumps(list(range(300000))).encode()
print(hashlib.sha256(zlib.decompress(zlib.compress(d))).hexdigest()[:16], sum(len(m.group()) for m in re.finditer(r'\d+', d.decode())))
c=sqlite3.connect(':memory:'); c.execute('create table t(x)'); c.executemany('insert into t values(?)',[(i,) for i in range(50000)]); print(c.execute('select sum(x) from t').fetchone())"
node /tmp/aojit/shell/node.js
sh /tmp/aojit/shell/shell_out.sh
bzip2 -c /tmp/aojit/shell/bz.in | bzip2 -dc | wc -c
python3 /tmp/aojit/shell/selfmod.py
