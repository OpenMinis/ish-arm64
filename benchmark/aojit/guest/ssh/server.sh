# The sshd the ssh cases talk to. Run it in a SEPARATE ish (on the Mac: a fakefs rootfs, since
# sshd refuses a realfs /var/empty that is not owned by root; in the app: any iSH session),
# so the measured/recorded ish only runs the client:
#   apk add openssh-server openssh-sftp-server openssh-keygen openssh-client-common
#   sh server.sh [port (2222)] < client public keys
# (openssh-client-common is for scp -O, which runs scp on this side.)
# It offers every algorithm ssh_train_full.sh asks for: a host key of each type (ed25519, rsa 3072,
# ecdsa nistp256/384/521), the CBC ciphers and SHA-1 MACs sshd leaves out by default, the
# finite-field KEX groups, and TCP forwarding.
mkdir -p /root/.ssh /tmp/aojit/ssh /etc/ssh && chmod 700 /root/.ssh
cat >> /root/.ssh/authorized_keys; chmod 600 /root/.ssh/authorized_keys
ssh-keygen -A > /dev/null
for b in 384 521; do
    [ -f /etc/ssh/ssh_host_ecdsa${b}_key ] || ssh-keygen -q -t ecdsa -b $b -N '' -f /etc/ssh/ssh_host_ecdsa${b}_key
done
b() { [ -s /tmp/aojit/ssh/$1 ] || head -c $2 /dev/urandom > /tmp/aojit/ssh/$1; }
b blob1m 1048576
b blob10m 10485760
b blob50m 52428800
exec /usr/sbin/sshd -D -e -p ${1:-2222} -o ListenAddress=127.0.0.1 -o PermitRootLogin=prohibit-password -o UseDNS=no \
    -o "Subsystem=sftp /usr/lib/ssh/sftp-server" \
    -o HostKey=/etc/ssh/ssh_host_ed25519_key -o HostKey=/etc/ssh/ssh_host_rsa_key -o HostKey=/etc/ssh/ssh_host_ecdsa_key \
    -o HostKey=/etc/ssh/ssh_host_ecdsa384_key -o HostKey=/etc/ssh/ssh_host_ecdsa521_key \
    -o Ciphers=+aes128-cbc,aes192-cbc,aes256-cbc \
    -o MACs=+hmac-sha1,hmac-sha1-etm@openssh.com \
    -o KexAlgorithms=+diffie-hellman-group14-sha256,diffie-hellman-group16-sha512,diffie-hellman-group18-sha512,diffie-hellman-group-exchange-sha256 \
    -o HostKeyAlgorithms=ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256 \
    -o PubkeyAcceptedAlgorithms=ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256 \
    -o AllowTcpForwarding=yes -o MaxStartups=100:30:200 -o MaxSessions=20
