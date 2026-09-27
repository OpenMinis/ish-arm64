# Deterministic checks for the three-way comparison (verify.sh): every line must be the same with
# AOT on, AOT off and ISH_JIT=0. Covers the images' modules: python extensions and libpython, zlib,
# ssl/socket/select against a local HTTPS server, sqlite, threads (futex), ssh/scp and rg.
# The ssh part needs AOJIT_SSH=host:port (the sshd of ../ssh/server.sh); without it, it is skipped.
V=/tmp/aojit/verify
cd $V
echo "== python json/hashlib/blake2/struct/datetime/binascii/math/zlib"
python3 -c '
import json, hashlib, struct, datetime, binascii, math, zlib, base64
d = [{"i": i, "s": str(i) * 3, "f": i / 7} for i in range(20000)]; s = json.dumps(d, sort_keys=True); b = s.encode()
print(len(s), hashlib.sha256(b).hexdigest(), hashlib.blake2b(b).hexdigest()[:16], hashlib.md5(b).hexdigest())
print(json.loads(s)[12345], struct.pack("<IqHd", 1, -2, 3, 4.5).hex(), binascii.crc32(b), zlib.adler32(zlib.decompress(zlib.compress(b, 9))))
print(base64.b64encode(b[:48]).decode(), datetime.datetime(2026, 9, 27, 12, 0).isoformat(), round(sum(math.sqrt(i) + math.sin(i) for i in range(50000)), 6))'
echo "== python ssl/socket/select against a local https server"
cd /tmp/aojit/net && rm -f server_8490.ready; python3 tls_server.py 8490 rsa & spid=$!
while [ ! -f server_8490.ready ]; do sleep 0.2; done
python3 -c '
import json, ssl, socket, select, urllib.request
c = ssl.create_default_context(cafile="/tmp/aojit/net/ca_bundle.pem")
for i in range(3):
    r = urllib.request.urlopen(f"https://localhost:8490/api/{i}", context=c, timeout=30); print(r.status, json.loads(r.read())["path"])
s = c.wrap_socket(socket.create_connection(("127.0.0.1", 8490)), server_hostname="localhost")
s.sendall(b"GET /blob/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"); n = 0
while True:
    rd, _, _ = select.select([s], [], [], 20)
    x = s.recv(65536) if rd else b""
    if not x: break
    n += len(x)
print("blob bytes > 1MB:", n > 1 << 20)'
kill $spid; wait $spid 2>/dev/null; cd $V
echo "== sqlite"
python3 -c '
import sqlite3, hashlib
c = sqlite3.connect("/tmp/aojit/verify/t.db"); c.execute("drop table if exists t"); c.execute("create table t(id integer primary key, k text, v real)")
c.executemany("insert into t(k, v) values(?, ?)", [(f"k{i % 97}", i * 1.5) for i in range(20000)]); c.commit()
print(c.execute("select count(*), sum(v), count(distinct k) from t").fetchone())
print(c.execute("select k, round(avg(v), 3) from t group by k order by 2 desc limit 3").fetchall())
print(hashlib.sha256(repr(c.execute("select * from t where v > 29000 order by id").fetchall()).encode()).hexdigest()[:16])'
echo "== threads (futex)"
python3 $V/futex_stress.py
echo "== ssh"
if [ -n "$AOJIT_SSH" ]; then
    host=${AOJIT_SSH%:*} port=${AOJIT_SSH#*:}
    O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR"
    for i in 1 2 3 4; do ssh -p $port $O root@$host 'echo $((6 * 7 + '$i')); head -1 /etc/alpine-release | cut -c1-4; sha256sum /etc/passwd | cut -c1-16'; done
    rm -f $V/d10m $V/pw.back
    scp -q -P $port $O $V/r10m root@$host:/tmp/aojit/ssh/u10m && scp -q -P $port $O root@$host:/tmp/aojit/ssh/u10m $V/d10m && cmp $V/r10m $V/d10m && echo "scp sftp-mode 10MB round trip: identical"
    scp -q -O -P $port $O /etc/passwd root@$host:/tmp/aojit/ssh/pw && scp -q -O -P $port $O root@$host:/tmp/aojit/ssh/pw $V/pw.back && cmp /etc/passwd $V/pw.back && echo "scp legacy mode round trip: identical"
else
    echo "skipped (no AOJIT_SSH)"
fi
echo "== rg"
cd /usr/lib/python3.12 && rg -c 'def ' . | sort | md5sum | cut -c1-16; rg -l 'import os' . | wc -l; rg -n -P '(?<=self\.)_\w+ = ' . | wc -l; rg -i -w 'deprecated' . | wc -l
