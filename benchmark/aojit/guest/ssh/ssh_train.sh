# ssh / scp recording workload: short remote commands with a fresh handshake each (as the agents
# use ssh), across host key / user key / KEX / cipher choices, and scp both ways.
# Needs the server of server.sh at $AOJIT_SSH (default 127.0.0.1:2222).
H=${AOJIT_SSH:-127.0.0.1:2222}; host=${H%:*} port=${H#*:}
C="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR"
s() { ssh -p $port $C "$@" root@$host 'cat /etc/os-release | head -2; ls /tmp | wc -l; echo done' > /dev/null || echo "SSHFAIL $*"; }
for i in 1 2 3; do s; done
s -i /root/.ssh/id_rsa
s -o HostKeyAlgorithms=ecdsa-sha2-nistp256
s -o HostKeyAlgorithms=rsa-sha2-512
s -o KexAlgorithms=curve25519-sha256
s -o KexAlgorithms=mlkem768x25519-sha256
s -o Ciphers=aes256-gcm@openssh.com
s -o Ciphers=aes128-ctr -o MACs=hmac-sha2-256-etm@openssh.com
s -o Compression=yes
ssh -p $port $C root@$host 'head -c 300000 /tmp/aojit/ssh/blob1m | od -An -tx1 | head -2000' > /dev/null || echo SSHFAIL-stream
echo 'echo from-stdin' | ssh -p $port $C root@$host sh > /dev/null || echo SSHFAIL-stdin
P="-P $port $C"
for o in "" "-O"; do
    scp -q $o $P /tmp/aojit/ssh/blob1m root@$host:/tmp/aojit/ssh/up1 || echo "SCPFAIL up $o"
    scp -q $o $P root@$host:/tmp/aojit/ssh/blob1m /tmp/aojit/ssh/down1 || echo "SCPFAIL down $o"
done
scp -q -O $P /etc/os-release /etc/passwd root@$host:/tmp/aojit/ssh/ || echo SCPFAIL-multi
echo ssh-train-ok
