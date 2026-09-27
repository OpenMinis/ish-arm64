# Candidate workload (profile.sh): py_local.sh plus urllib HTTPS (ssl/socket/select) against the
# suite's local TLS server (net/tls_server.py; certificates from setup.sh).
cd /tmp/aojit/net && rm -f server_8480.ready; python3 tls_server.py 8480 rsa & spid=$!
n=0; while [ ! -f server_8480.ready ] && [ $n -lt 300 ]; do sleep 0.1; n=$((n + 1)); done
sh /tmp/aojit/measure/py_local.sh
for i in 1 2; do python3 -c 'import json, ssl, urllib.request; c = ssl.create_default_context(cafile="/tmp/aojit/net/ca_bundle.pem"); print(len(json.loads(urllib.request.urlopen("https://localhost:8480/api/1", context=c).read())["items"]))'; done
kill $spid; wait $spid 2>/dev/null || true
