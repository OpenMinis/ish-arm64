# Recording workload for the python3.12 lib-dynload extension modules agents lean on:
# _json, _hashlib (+_blake2), _struct, _datetime, binascii, math, zlib, _socket, select, _ssl.
import base64, binascii, datetime, hashlib, json, math, os, select, socket, ssl, struct, sys, zlib
import urllib.request

docs = [{'id': i, 'name': f'item-{i}', 'tags': ['a', 'b', str(i % 7)], 'score': i / 7, 'ok': i % 3 == 0,
         'nested': {'k': [1, 2, {'x': None}], 'u': 'ünïcødé ✓'}} for i in range(4000)]
for indent in (None, 2):
    s = json.dumps(docs, indent=indent, sort_keys=indent is not None, ensure_ascii=indent is None)
    assert json.loads(s) == docs
json.loads('{"a": [1, 2.5e3, -0.1, true, false, null, "s\\u00e9\\n"]}')
blob = os.urandom(1 << 19)
for alg in ('md5', 'sha1', 'sha256', 'sha512', 'blake2b', 'blake2s', 'sha3_256'):
    hashlib.new(alg, blob).hexdigest()
hashlib.pbkdf2_hmac('sha256', b'pw', b'salt', 2000)
for b in (blob[:4096], blob):
    assert base64.b64decode(base64.b64encode(b)) == b and binascii.unhexlify(binascii.hexlify(b)) == b
binascii.crc32(blob); base64.urlsafe_b64encode(blob[:999]); base64.b32encode(blob[:500])
for fmt in ('<I', '>q', '<3sHd', '!BBHI'):
    size = struct.calcsize(fmt)
    for i in range(3000):
        struct.unpack(fmt, struct.pack(fmt, *( [b'abc', 7, 1.5] if fmt == '<3sHd' else [i % 200] * len(fmt.strip('<>!')))))
now = datetime.datetime(2026, 9, 27, 10, 0, tzinfo=datetime.timezone.utc)
for i in range(3000):
    t = now + datetime.timedelta(minutes=i)
    datetime.datetime.fromisoformat(t.isoformat()); t.strftime('%Y-%m-%d %H:%M:%S'); (t - now).total_seconds()
sum(math.sqrt(i) + math.log1p(i) + math.sin(i) + math.floor(i / 3) for i in range(20000)); math.isclose(0.1 + 0.2, 0.3)
for lvl in (1, 6, 9):
    c = zlib.compress(blob[:200000] + json.dumps(docs).encode(), lvl); zlib.decompress(c)
d = zlib.decompressobj(); d.decompress(zlib.compress(b'x' * 100000)); zlib.crc32(blob); zlib.adler32(blob)
# sockets, select and TLS against the suite's local HTTPS server (net/tls_server.py)
port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
if port:
    ctx = ssl.create_default_context(cafile='/tmp/aojit/net/ca_bundle.pem')
    for i in range(6):
        with urllib.request.urlopen(f'https://localhost:{port}/api/{i}', context=ctx, timeout=20) as r:
            json.loads(r.read())
    s = socket.create_connection(('127.0.0.1', port)); tls = ctx.wrap_socket(s, server_hostname='localhost')
    tls.sendall(b'GET /blob/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'); n = 0
    while True:
        r, _, _ = select.select([tls], [], [], 10)
        chunk = tls.recv(65536) if r else b''
        if not chunk: break
        n += len(chunk)
    tls.close(); assert n > 1 << 20
print('pyext-train-ok')
