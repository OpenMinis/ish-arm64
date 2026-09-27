# python extension modules recording workload: the script plus a few `python3 -c` one-liners, with the local TLS server
cd /tmp/aojit/net && rm -f server_8470.ready
python3 tls_server.py 8470 rsa & spid=$!
while [ ! -f server_8470.ready ]; do sleep 0.1; done
python3 /tmp/aojit/pyext/pyext_train.py 8470
for i in 1 2 3; do echo '{"a": [1, 2, {"b": "c"}]}' | python3 -c 'import json, sys, hashlib; d = json.load(sys.stdin); print(hashlib.sha256(json.dumps(d).encode()).hexdigest()[:8])'; done
kill $spid; wait $spid 2>/dev/null || true
