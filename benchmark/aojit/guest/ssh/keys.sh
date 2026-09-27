# Client keys for the ssh cases and recordings (generated per install, never committed).
# Prints the public keys: put them into the server's ~/.ssh/authorized_keys.
mkdir -p /root/.ssh && chmod 700 /root/.ssh
[ -f /root/.ssh/id_ed25519 ] || ssh-keygen -q -t ed25519 -N '' -f /root/.ssh/id_ed25519
[ -f /root/.ssh/id_rsa ] || ssh-keygen -q -t rsa -b 3072 -N '' -f /root/.ssh/id_rsa
[ -f /tmp/aojit/ssh/blob1m ] || head -c 1048576 /dev/urandom > /tmp/aojit/ssh/blob1m
cat /root/.ssh/id_ed25519.pub /root/.ssh/id_rsa.pub
