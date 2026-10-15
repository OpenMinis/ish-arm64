# usage: tls_run.sh <port> <rsa|ec> <client args...>
port=$1 kind=$2; shift 2
cd /tmp/aojit/net && rm -f server_$port.ready
python3 tls_server.py $port $kind & spid=$!
while [ ! -f server_$port.ready ]; do sleep 0.1; done
python3 tls_client.py $port "$@"
kill $spid; wait $spid 2>/dev/null || true
