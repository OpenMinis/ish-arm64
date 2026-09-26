"""HTTPS client work like an agent's: many short API calls (fresh TLS handshakes), keep-alive
sessions, bulk transfer, plus hashlib. usage: tls_client.py <port> <handshakes> <session calls> <MB down> <MB up> <MB hashed> [ciphers]"""
import hashlib, hmac, json, os, ssl, sys, time
import requests, urllib3
port, hs, calls, down, up, hashed = (int(x) for x in sys.argv[1:7])
ciphers = sys.argv[7] if len(sys.argv) > 7 else None
urllib3.disable_warnings()
url = f'https://127.0.0.1:{port}'
t = {}
t0 = time.time(); acc = 0
for i in range(hs):                                    # new connection each time: full handshake
    acc += len(requests.get(f'{url}/api/{i}', verify=False).json()['items'])
t['handshakes'] = time.time() - t0; t0 = time.time()
with requests.Session() as s:
    s.verify = False
    for i in range(calls):
        acc += s.post(f'{url}/api/post', json={'i': i, 'msg': 'x' * 512}).json()['len']
    if down: acc += len(s.get(f'{url}/blob/{down}').content)
    if up: acc += s.post(f'{url}/up', data=os.urandom(1 << 20) * up).json()['len']
t['session+bulk'] = time.time() - t0; t0 = time.time()
if ciphers:                                            # TLS 1.2 with a forced cipher (e.g. ChaCha20)
    ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2; ctx.set_ciphers(ciphers)
    import http.client
    c = http.client.HTTPSConnection('127.0.0.1', port, context=ctx)
    c.request('GET', f'/blob/{max(1, down // 2)}'); acc += len(c.getresponse().read()); c.close()
t['tls12'] = time.time() - t0; t0 = time.time()
data = os.urandom(1 << 20)
for i in range(hashed):
    for alg in ('sha256', 'sha1', 'md5', 'sha512'):
        acc += hashlib.new(alg, data).digest()[0]
    acc += hmac.new(b'k', data, 'sha256').digest()[0]
t['hashlib'] = time.time() - t0
print(' '.join(f'{k}={v:.2f}s' for k, v in t.items()), file=sys.stderr)
print('acc', acc)
