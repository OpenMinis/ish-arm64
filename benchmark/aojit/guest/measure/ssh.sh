# Candidate workload (profile.sh): short remote commands with a fresh handshake each, as agents use
# ssh, plus scp of 1 MB both ways. Needs AOJIT_SSH=host:port (the sshd of ../ssh/server.sh).
[ -n "$AOJIT_SSH" ] || { echo "measure/ssh.sh: set AOJIT_SSH=host:port" >&2; exit 1; }
host=${AOJIT_SSH%:*} port=${AOJIT_SSH#*:}; M=/tmp/aojit/measure
[ -s $M/blob1m ] || head -c 1048576 /dev/urandom > $M/blob1m
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR"
for i in 1 2 3 4 5 6 7 8; do ssh -p $port $O root@$host 'cat /etc/os-release | head -1' > /dev/null || echo SSHFAIL; done
for i in 1 2; do
    scp -q -O -P $port $O $M/blob1m root@$host:/tmp/aojit/ssh/up$i || echo SCPFAIL
    scp -q -O -P $port $O root@$host:/tmp/aojit/ssh/blob1m $M/down$i || echo SCPFAIL
done
