# One-shot agent HTTPS calls: each a new python3 process, verify=True against a full CA bundle.
# usage: agent_cold.sh <port> <rsa|ec> <calls>
port=$1 kind=$2 n=$3
cd /tmp/aojit/net && rm -f server_$port.ready
python3 tls_server.py $port $kind & spid=$!
while [ ! -f server_$port.ready ]; do sleep 0.1; done
i=0; while [ $i -lt $n ]; do
  python3 -c "import requests; r = requests.get('https://localhost:$port/api/$i', verify='/tmp/aojit/net/ca_bundle.pem', timeout=30); print(len(r.json()['items']))" > /dev/null || echo FAIL
  i=$((i + 1))
done
kill $spid; wait $spid 2>/dev/null || true
echo agent-cold-ok
