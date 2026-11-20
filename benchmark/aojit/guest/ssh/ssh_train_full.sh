# ssh / scp recording workload, full: every common algorithm OpenSSH negotiates, one fresh handshake
# each (as the agents use ssh), then the transfer and session paths. It is also the ssh half of the
# libcrypto recording (net/crypto_train.sh), since ssh's handshakes and ciphers run in libcrypto.
# Needs the server of server.sh at $AOJIT_SSH (default 127.0.0.1:2222) and the keys of keys.sh in
# its authorized_keys. Every step that fails prints a line with FAIL, which makes record.sh refuse
# the image; an algorithm that is not in `ssh -Q` here is left out silently (an older or newer
# OpenSSH), one the server refuses is a FAIL (server.sh is out of date).
H=${AOJIT_SSH:-127.0.0.1:2222}; host=${H%:*} port=${H#*:}
T=/tmp/aojit/ssh W=/tmp/aojit/ssh/work
rm -rf $W; mkdir -p $W
C="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=20 -o LogLevel=ERROR"
R='cat /etc/os-release | head -2; ls /tmp | wc -l; echo done'
s() { ssh -p $port $C "$@" root@$host "$R" > /dev/null || echo "SSHFAIL $*"; }
has() { ssh -Q $1 | grep -qx -- "$2"; }

# 1. defaults, as before
for i in 1 2 3; do s; done

# 2. user keys: one signature of each type (rsa twice per size: sha2-256 and sha2-512)
for k in id_ed25519 id_ecdsa id_ecdsa384 id_ecdsa521; do s -i /root/.ssh/$k -o IdentitiesOnly=yes; done
for k in id_rsa id_rsa4096; do
    for a in rsa-sha2-256 rsa-sha2-512; do s -i /root/.ssh/$k -o IdentitiesOnly=yes -o PubkeyAcceptedAlgorithms=$a; done
done

# 3. host keys: one verification of each type
for a in ssh-ed25519 ecdsa-sha2-nistp256 ecdsa-sha2-nistp384 ecdsa-sha2-nistp521 rsa-sha2-256 rsa-sha2-512; do
    s -o HostKeyAlgorithms=$a
done

# 4. key exchange
for a in curve25519-sha256 mlkem768x25519-sha256 sntrup761x25519-sha512 ecdh-sha2-nistp256 ecdh-sha2-nistp384 \
         ecdh-sha2-nistp521 diffie-hellman-group14-sha256 diffie-hellman-group16-sha512 diffie-hellman-group18-sha512 \
         diffie-hellman-group-exchange-sha256; do
    has kex $a && s -o KexAlgorithms=$a
done

# 5. ciphers. The AEAD ones carry their own MAC; CTR and CBC go with each MAC below.
for c in chacha20-poly1305@openssh.com aes128-gcm@openssh.com aes256-gcm@openssh.com; do s -o Ciphers=$c; done
for c in aes128-ctr aes192-ctr aes256-ctr aes128-cbc aes192-cbc aes256-cbc; do
    s -o Ciphers=$c -o MACs=hmac-sha2-256-etm@openssh.com
done
for m in hmac-sha2-256 hmac-sha2-512 hmac-sha2-256-etm@openssh.com hmac-sha2-512-etm@openssh.com \
         umac-64-etm@openssh.com umac-128-etm@openssh.com umac-64@openssh.com umac-128@openssh.com \
         hmac-sha1 hmac-sha1-etm@openssh.com; do
    s -o Ciphers=aes128-ctr -o MACs=$m
    s -o Ciphers=aes256-cbc -o MACs=$m
done

# 6. session shapes: compression, a long output stream, stdin, a pty, rekeying, a 4 MB pipe each
#    way under the two usual ciphers, an exit status, keep-alives
s -o Compression=yes
s -o Compression=yes -o Ciphers=aes256-ctr -o MACs=hmac-sha2-512
ssh -p $port $C root@$host "head -c 300000 $T/blob1m | od -An -tx1 | head -2000" > /dev/null || echo SSHFAIL-stream
echo 'echo from-stdin' | ssh -p $port $C root@$host sh > /dev/null || echo SSHFAIL-stdin
# a pty request: an sshd in the Mac CLI cannot hand the pty over (no fd passing between its
# processes), so the request is sent and its failure is not counted
ssh -tt -p $port $C root@$host 'echo on-a-pty' > /dev/null 2>&1 < /dev/null
for c in chacha20-poly1305@openssh.com aes256-gcm@openssh.com aes128-ctr; do
    ssh -p $port $C -o Ciphers=$c root@$host "cat $T/blob10m | head -c 4194304" > $W/pipe.down || echo "SSHFAIL pipe-down $c"
    [ "$(wc -c < $W/pipe.down)" = 4194304 ] || echo "SSHFAIL pipe-down-size $c"
    head -c 4194304 $T/blob10m | ssh -p $port $C -o Ciphers=$c root@$host 'wc -c' > $W/pipe.up || echo "SSHFAIL pipe-up $c"
    [ "$(tr -d ' ' < $W/pipe.up)" = 4194304 ] || echo "SSHFAIL pipe-up-size $c"
    ssh -p $port $C -o Ciphers=$c -o RekeyLimit=1M root@$host "cat $T/blob10m" > /dev/null || echo "SSHFAIL rekey $c"
done
ssh -p $port $C root@$host 'exit 3'; [ $? = 3 ] || echo SSHFAIL-status
ssh -p $port $C -o ServerAliveInterval=1 root@$host 'sleep 3; echo alive' > /dev/null || echo SSHFAIL-keepalive
ssh -v -p $port $C -o LogLevel=DEBUG1 root@$host true > /dev/null 2>&1 || echo SSHFAIL-verbose

# 7. known_hosts: learn the host keys, check them again hashed, ssh-keyscan
K="-o UserKnownHostsFile=$W/known -o BatchMode=yes -o ConnectTimeout=20 -o LogLevel=ERROR"
ssh -p $port $K -o StrictHostKeyChecking=accept-new root@$host true || echo SSHFAIL-known-new
ssh -p $port $K -o StrictHostKeyChecking=yes root@$host true || echo SSHFAIL-known-check
ssh -p $port $K -o StrictHostKeyChecking=yes -o HashKnownHosts=yes -o UpdateHostKeys=yes root@$host true || echo SSHFAIL-known-update
ssh-keyscan -p $port -t ed25519,ecdsa,rsa $host > $W/scan 2> /dev/null; [ -s $W/scan ] || echo SSHFAIL-keyscan
ssh-keygen -H -f $W/known > /dev/null 2>&1 || echo SSHFAIL-hash-known

# 8. a master connection: three sessions over it, then closed. Where ish cannot pass descriptors
#    over a unix socket the client says so and connects by itself, which is what the app's users
#    get too, so only the exit status counts.
M="-o ControlPath=$W/cm.sock"
ssh -p $port $C $M -o ControlMaster=yes -o ControlPersist=60 -N -f root@$host || echo SSHFAIL-master
for i in 1 2 3; do ssh -p $port $C $M root@$host "$R" > /dev/null 2>&1 || echo "SSHFAIL mux $i"; done
ssh -p $port $C $M -O check root@$host 2> /dev/null || echo SSHFAIL-mux-check
scp -q -P $port $C $M $T/blob1m root@$host:$T/up.mux 2> /dev/null || echo SCPFAIL-mux
ssh -p $port $C $M -O exit root@$host 2> /dev/null || echo SSHFAIL-mux-exit

# 9. port forwarding: -L to the sshd itself, a session through it; -D and -R set up and torn down
ssh -p $port $C -o ExitOnForwardFailure=yes -L 127.0.0.1:2392:127.0.0.1:$port -N -f -o ControlMaster=yes -o ControlPath=$W/fw.sock root@$host || echo SSHFAIL-forward
ssh -p 2392 $C root@127.0.0.1 "$R" > /dev/null || echo SSHFAIL-through-forward
ssh -p $port $C -o ControlPath=$W/fw.sock -O exit root@$host 2> /dev/null || echo SSHFAIL-forward-exit
ssh -p $port $C -o ExitOnForwardFailure=yes -D 127.0.0.1:2393 -R 127.0.0.1:2394:127.0.0.1:$port root@$host 'sleep 1' || echo SSHFAIL-forward-DR
ssh -p $port $C -o ProxyCommand="ssh -p $port $C -W %h:%p root@$host" root@$host true || echo SSHFAIL-jump

# 10. agent: keys added, one signature of each type through it
eval "$(ssh-agent -s)" > /dev/null
ssh-add -q /root/.ssh/id_ed25519 /root/.ssh/id_rsa /root/.ssh/id_ecdsa 2> /dev/null || echo SSHFAIL-agent-add
ssh-add -l > /dev/null || echo SSHFAIL-agent-list
for a in ssh-ed25519 rsa-sha2-512 ecdsa-sha2-nistp256; do
    ssh -p $port $C -o IdentityFile=none -o IdentityAgent=$SSH_AUTH_SOCK -o PubkeyAcceptedAlgorithms=$a root@$host true || echo "SSHFAIL agent $a"
done
ssh -A -p $port $C -o IdentityFile=none root@$host 'ssh-add -l > /dev/null 2>&1; true' || echo SSHFAIL-agent-forward
ssh-agent -k > /dev/null

# 11. key generation and the other ssh-keygen jobs (libcrypto: RSA prime search, EC key generation)
for t in "ed25519" "ecdsa -b 256" "ecdsa -b 384" "ecdsa -b 521" "rsa -b 2048" "rsa -b 3072"; do
    f=$W/gen_$(echo $t | tr -d ' -'); ssh-keygen -q -t $t -N '' -f $f || echo "SSHFAIL keygen $t"
    ssh-keygen -l -f $f.pub > /dev/null || echo "SSHFAIL fingerprint $t"
    ssh-keygen -y -f $f > /dev/null || echo "SSHFAIL pubout $t"
done
ssh-keygen -q -p -N secret -f $W/gen_ed25519 > /dev/null || echo SSHFAIL-passphrase
ssh-keygen -q -p -P secret -N '' -f $W/gen_ed25519 > /dev/null || echo SSHFAIL-passphrase-off
echo signed > $W/msg
ssh-keygen -q -Y sign -f $W/gen_ed25519 -n file $W/msg 2> /dev/null || echo SSHFAIL-sshsig
ssh-keygen -q -s $W/gen_ecdsab256 -I train -n root -V +1h $W/gen_ed25519.pub 2> /dev/null || echo SSHFAIL-cert

# 12. a failed login (no key the server accepts) and a refused algorithm: the error paths
ssh -p $port $C -o IdentitiesOnly=yes -i $W/gen_ed25519 root@$host true 2> /dev/null && echo SSHFAIL-denied-expected
ssh -p $port $C -o Ciphers=3des-cbc root@$host true 2> /dev/null && echo SSHFAIL-refused-expected

# 13. scp: both protocols (-O needs scp on the server too), 1 / 10 / 50 MB up and back under the
#     two usual ciphers and compared, then the small shapes
P="-P $port $C"
for o in "" "-O"; do
    scp -q $o $P $T/blob1m root@$host:$T/up1 || echo "SCPFAIL up $o"
    scp -q $o $P root@$host:$T/blob1m $W/down1 || echo "SCPFAIL down $o"
    for c in chacha20-poly1305@openssh.com aes256-gcm@openssh.com; do
        for z in 10m 50m; do
            scp -q $o $P -c $c $T/blob$z root@$host:$T/up$z || echo "SCPFAIL up $z $c $o"
            scp -q $o $P -c $c root@$host:$T/up$z $W/down$z || echo "SCPFAIL down $z $c $o"
            cmp -s $T/blob$z $W/down$z || echo "SCPFAIL differs $z $c $o"
        done
    done
    scp -q $o $P -c aes128-ctr -o MACs=hmac-sha2-256-etm@openssh.com $T/blob10m root@$host:$T/up10m || echo "SCPFAIL up ctr $o"
    scp -q $o $P -C $T/blob1m root@$host:$T/up1 || echo "SCPFAIL up -C $o"
    scp -q $o $P -p -l 80000 $T/blob1m root@$host:$T/up1 || echo "SCPFAIL up -p -l $o"
    scp -q $o $P /etc/os-release /etc/passwd root@$host:$T/ || echo "SCPFAIL multi $o"
    mkdir -p $W/tree/a/b && cp /etc/passwd $W/tree/a/b/p && cp /etc/os-release $W/tree/a/o
    scp -q $o $P -r $W/tree root@$host:$T/ || echo "SCPFAIL -r up $o"
    scp -q $o $P -r root@$host:$T/tree $W/tree.back || echo "SCPFAIL -r down $o"
    rm -rf $W/tree.back
    scp -q $o $P root@$host:$T/no-such-file $W/ 2> /dev/null && echo "SCPFAIL missing-expected $o"
done
scp -q -3 $P root@$host:$T/up1 root@$host:$T/up1.copy || echo SCPFAIL-3
scp $P $T/blob1m root@$host:$T/up1 > /dev/null 2>&1 || echo SCPFAIL-progress
printf 'put %s %s\nls -l %s\nget %s %s\nbye\n' $T/blob1m $T/up.sftp $T $T/up.sftp $W/down.sftp | sftp -q -b - $P root@$host > /dev/null || echo SFTPFAIL
rm -rf $W
echo ssh-train-ok
