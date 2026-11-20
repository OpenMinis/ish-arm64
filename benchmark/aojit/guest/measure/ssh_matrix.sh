# ssh / scp step by step, for comparing image sets (not a profile.sh candidate: one step per ish
# run, timed by the caller, so each run's /proc/ish/jit holds the hits of that step alone).
#   hs-*    five fresh connections with one choice of cipher / host key + user key
#   up-*, down-*   scp of 10 or 50 MB, sftp mode with chacha20-poly1305 or aes256-gcm, or -O (legacy)
#   mix     what an agent session looks like: commands, pipes, small and legacy copies, compression
#   tail-*  five fresh connections with a less common choice (finite-field or NIST or sntrup KEX,
#           CBC / CTR with SHA-1 or UMAC, a 4096-bit RSA or a P-521 key)
#   tls-*   the same libraries under TLS: python requests (certificate-verifying, new process per
#           call) and openssl s_client handshakes, TLS 1.3 and 1.2, against local servers
# usage: AOJIT_SSH=host:port sh ssh_matrix.sh <on|off> <ssh binary> <step>     (no step: list them)
#   off: echo off > /proc/ish/jit first (the same ish without its images)
# Prints FAIL on a failed step, then "@@ jit" and /proc/ish/jit.
STEPS="hs-chacha20 hs-aes-gcm hs-ed25519 hs-rsa hs-ecdsa up-10m-chacha20 down-10m-chacha20 up-50m-chacha20 down-50m-chacha20 up-10m-aes256-gcm down-10m-aes256-gcm up-50m-aes256-gcm down-50m-aes256-gcm up-50m-legacy down-50m-legacy mix tail-dh-group16 tail-ecdh-p521 tail-sntrup tail-cbc-sha1 tail-ctr-umac tail-rsa4096 tail-ecdsa521 tls-python tls-s_client"
[ -n "$3" ] || { echo $STEPS; exit 0; }
[ -n "$AOJIT_SSH" ] || { echo "measure/ssh_matrix.sh: set AOJIT_SSH=host:port" >&2; exit 1; }
host=${AOJIT_SSH%:*} port=${AOJIT_SSH#*:}; T=/tmp/aojit/ssh M=/tmp/aojit/measure
B=$2
[ "$1" = off ] && echo off > /proc/ish/jit
O="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=20 -o LogLevel=ERROR -o IdentitiesOnly=yes"
I="-i /root/.ssh/id_ed25519"
S="-q -S $B -P $port $O $I"
x5() { for i in 1 2 3 4 5; do $B -p $port $O "$@" root@$host 'head -1 /etc/os-release' > /dev/null || return 1; done; }
cp_() {   # cp_ <up|down> <10m|50m> <scp options...>
    d=$1 z=$2; shift 2
    if [ $d = up ]; then scp $S "$@" $T/blob$z root@$host:$T/mx_up; else scp $S "$@" root@$host:$T/blob$z $M/mx_down; fi
}
mix() {
    for i in 1 2 3; do $B -p $port $O $I root@$host 'ls /etc | wc -l' || return 1; done
    $B -p $port $O -i /root/.ssh/id_rsa -o KexAlgorithms=diffie-hellman-group14-sha256 -o Ciphers=aes128-ctr -o MACs=hmac-sha2-256-etm@openssh.com root@$host 'uname -a' || return 1
    $B -p $port $O -i /root/.ssh/id_ecdsa -o KexAlgorithms=ecdh-sha2-nistp256 -o Compression=yes root@$host "head -c 2097152 $T/blob10m | od -An -tx1 | head -20000" || return 1
    head -c 4194304 $T/blob10m | $B -p $port $O $I root@$host 'wc -c' || return 1
    $B -p $port $O $I root@$host "cat $T/blob10m" | wc -c || return 1
    scp $S $T/blob1m root@$host:$T/mx_up && scp $S root@$host:$T/blob1m $M/mx_down || return 1
    scp -O $S -c aes128-ctr $T/blob10m root@$host:$T/mx_up && scp -O $S /etc/passwd /etc/os-release root@$host:$T/ || return 1
    printf 'put %s %s\nget %s %s\n' $T/blob1m $T/mx_up $T/mx_up $M/mx_down | sftp -q -S $B -b - -P $port $O $I root@$host
}
case $3 in
    hs-chacha20) x5 $I -o Ciphers=chacha20-poly1305@openssh.com ;;
    hs-aes-gcm)  x5 $I -o Ciphers=aes256-gcm@openssh.com ;;
    hs-ed25519)  x5 $I -o HostKeyAlgorithms=ssh-ed25519 ;;
    hs-rsa)      x5 -i /root/.ssh/id_rsa -o HostKeyAlgorithms=rsa-sha2-512 ;;
    hs-ecdsa)    x5 -i /root/.ssh/id_ecdsa -o HostKeyAlgorithms=ecdsa-sha2-nistp256 ;;
    *-legacy)    d=${3%%-*}; cp_ $d 50m -O ;;
    up-*|down-*) d=${3%%-*} r=${3#*-}; z=${r%%-*} c=${r#*-}
                 [ $c = chacha20 ] && c=chacha20-poly1305@openssh.com || c=$c@openssh.com
                 cp_ $d $z -c $c ;;
    mix)         mix > /dev/null ;;
    tail-dh-group16) x5 $I -o KexAlgorithms=diffie-hellman-group16-sha512 ;;
    tail-ecdh-p521)  x5 $I -o KexAlgorithms=ecdh-sha2-nistp521 ;;
    tail-sntrup)     x5 $I -o KexAlgorithms=sntrup761x25519-sha512 ;;
    tail-cbc-sha1)   x5 $I -o Ciphers=aes256-cbc -o MACs=hmac-sha1 ;;
    tail-ctr-umac)   x5 $I -o Ciphers=aes128-ctr -o MACs=umac-128-etm@openssh.com ;;
    tail-rsa4096)    x5 -i /root/.ssh/id_rsa4096 -o HostKeyAlgorithms=rsa-sha2-256 -o PubkeyAcceptedAlgorithms=rsa-sha2-256 ;;
    tail-ecdsa521)   x5 -i /root/.ssh/id_ecdsa521 -o HostKeyAlgorithms=ecdsa-sha2-nistp521 ;;
    tls-python)      ! sh /tmp/aojit/net/agent_cold.sh 8470 rsa 4 | grep -q FAIL ;;
    tls-s_client)
        cd /tmp/aojit/net; openssl s_server -accept 8471 -cert ec.crt -key ec.key -www -quiet > /dev/null 2>&1 & sp=$!; sleep 1; rc=0
        for a in "-tls1_3" "-tls1_3 -ciphersuites TLS_CHACHA20_POLY1305_SHA256" "-tls1_3 -groups P-384" "-tls1_2" "-tls1_2 -cipher ECDHE-ECDSA-CHACHA20-POLY1305"; do
            printf 'GET / HTTP/1.0\r\n\r\n' | openssl s_client -connect 127.0.0.1:8471 -CAfile ca_bundle.pem -verify_return_error -ign_eof $a 2>&1 | grep -q "Verify return code: 0" || rc=1
        done
        kill $sp; wait $sp 2> /dev/null; [ $rc = 0 ] ;;
    *)           echo "FAIL unknown step $3" ;;
esac
[ $? = 0 ] || echo "FAIL $3"
rm -f $M/mx_down
echo "@@ jit"; cat /proc/ish/jit
