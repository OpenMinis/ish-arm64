# The sshd the ssh cases talk to. Run it in a SEPARATE ish (on the Mac: a fakefs rootfs, since
# sshd refuses a realfs /var/empty that is not owned by root; in the app: any iSH session),
# so the measured/recorded ish only runs the client:
#   apk add openssh-server openssh-sftp-server; sh server.sh [port (2222)] < client public keys
mkdir -p /root/.ssh /tmp/aojit/ssh && chmod 700 /root/.ssh
cat >> /root/.ssh/authorized_keys; chmod 600 /root/.ssh/authorized_keys
ssh-keygen -A > /dev/null
[ -f /tmp/aojit/ssh/blob1m ] || head -c 1048576 /dev/urandom > /tmp/aojit/ssh/blob1m
exec /usr/sbin/sshd -D -e -p ${1:-2222} -o ListenAddress=127.0.0.1 -o PermitRootLogin=prohibit-password -o UseDNS=no -o "Subsystem=sftp /usr/lib/ssh/sftp-server"
