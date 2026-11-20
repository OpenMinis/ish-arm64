# Client keys for the ssh cases and recordings (generated per install, never committed).
# Prints the public keys: put them into the server's ~/.ssh/authorized_keys.
mkdir -p /root/.ssh /tmp/aojit/ssh && chmod 700 /root/.ssh
k() { f=/root/.ssh/$1; shift; [ -f $f ] || ssh-keygen -q -N '' -f $f "$@"; }
k id_ed25519 -t ed25519
k id_rsa -t rsa -b 3072
k id_rsa4096 -t rsa -b 4096
k id_ecdsa -t ecdsa -b 256
k id_ecdsa384 -t ecdsa -b 384
k id_ecdsa521 -t ecdsa -b 521
# scp payloads: random, so compression and the cipher both do real work
b() { [ -s /tmp/aojit/ssh/$1 ] || head -c $2 /dev/urandom > /tmp/aojit/ssh/$1; }
b blob1m 1048576
b blob10m 10485760
b blob50m 52428800
cat /root/.ssh/id_*.pub
