# python extension modules recording workload: the script plus a few `python3 -c` one-liners, with the local TLS server
# (without certificates -- setup.sh makes them when openssl is installed -- the TLS part is left out)
port=0
if [ -s /tmp/aojit/net/rsa.crt ]; then
    cd /tmp/aojit/net && rm -f server_8470.ready
    python3 tls_server.py 8470 rsa & spid=$!
    n=0; while [ ! -f server_8470.ready ] && [ $n -lt 300 ]; do sleep 0.1; n=$((n + 1)); done
    if [ -f server_8470.ready ]; then port=8470; else echo "⚠️  TLS server did not start, recording without it"; fi
else
    echo "⚠️  no certificates (setup.sh), recording without the TLS server"
fi
python3 /tmp/aojit/pyext/pyext_train.py $port
for i in 1 2 3; do echo '{"a": [1, 2, {"b": "c"}]}' | python3 -c 'import json, sys, hashlib; d = json.load(sys.stdin); print(hashlib.sha256(json.dumps(d).encode()).hexdigest()[:8])'; done
[ -n "$spid" ] && { kill $spid; wait $spid 2>/dev/null || true; }
