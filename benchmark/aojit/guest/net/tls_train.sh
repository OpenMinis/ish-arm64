# TLS/crypto recording workload (sizes, key types and ciphers differ from the measured runs).
cd /tmp/aojit/net
sh tls_run.sh 8450 rsa 20 40 4 2 8 ECDHE-RSA-CHACHA20-POLY1305
sh tls_run.sh 8451 ec 20 30 2 1 4 ECDHE-ECDSA-AES256-GCM-SHA384
sh agent_cold.sh 8452 ec 3
python3 -c "
import hashlib, os
d = os.urandom(1 << 20)
for a in ('sha3_256', 'blake2b', 'sha224', 'sha384'): hashlib.new(a, d).digest()
"
echo tls-train-ok
