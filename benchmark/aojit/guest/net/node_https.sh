port=$1 n=$2
cd /tmp/aojit/net && rm -f server_$port.ready
python3 tls_server.py $port rsa & spid=$!
while [ ! -f server_$port.ready ]; do sleep 0.1; done
node node_https.js $port $n
kill $spid; wait $spid 2>/dev/null || true
