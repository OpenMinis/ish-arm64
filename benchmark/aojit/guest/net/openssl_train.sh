# libcrypto / libssl recording workload, openssl command-line half (tls_train.sh runs it): the
# primitives behind ssh, curl and python TLS, each for about a second, key generation and
# signatures of every common key type, and TLS 1.2 / 1.3 handshakes over the usual suites and
# groups between s_server and s_client. A step that fails prints a line with FAIL.
# The app's rootfs has no openssl command (installing the package would upgrade libcrypto3), so
# the recording rootfs gets only the binary; without it this half is a FAIL, not a silent gap.
cd /tmp/aojit/net
W=/tmp/aojit/net/osl; rm -rf $W; mkdir -p $W
command -v openssl > /dev/null || { echo "FAIL openssl command missing (put the binary of the openssl apk in /usr/bin)"; exit 1; }
q() { "$@" > /dev/null 2>&1 || echo "FAIL $*"; }

# 1. bulk primitives, one second each (all block sizes of `speed`)
for a in aes-128-gcm aes-256-gcm aes-128-ctr aes-256-ctr aes-128-cbc aes-256-cbc chacha20-poly1305 \
         sha256 sha512 sha384 sha1 md5 sha3-256; do
    q openssl speed -seconds 1 -evp $a
done
q openssl speed -seconds 1 -hmac sha256
q openssl speed -seconds 1 ecdsap256 ecdhx25519 ed25519 rsa2048

# 2. keys and signatures
head -c 262144 /dev/urandom > $W/msg
gen() { n=$1; shift; q openssl genpkey -out $W/$n.key "$@"; q openssl pkey -in $W/$n.key -pubout -out $W/$n.pub; }
gen rsa2048 -algorithm RSA -pkeyopt rsa_keygen_bits:2048
gen rsa3072 -algorithm RSA -pkeyopt rsa_keygen_bits:3072
gen p256 -algorithm EC -pkeyopt ec_paramgen_curve:P-256
gen p384 -algorithm EC -pkeyopt ec_paramgen_curve:P-384
gen p521 -algorithm EC -pkeyopt ec_paramgen_curve:P-521
gen ed25519 -algorithm ED25519
gen x25519 -algorithm X25519
gen x25519b -algorithm X25519
for k in rsa2048 rsa3072 p256 p384 p521; do
    for d in sha256 sha384 sha512; do
        q openssl dgst -$d -sign $W/$k.key -out $W/sig $W/msg
        q openssl dgst -$d -verify $W/$k.pub -signature $W/sig $W/msg
    done
done
q openssl dgst -sha256 -sigopt rsa_padding_mode:pss -sign $W/rsa2048.key -out $W/sig $W/msg
q openssl dgst -sha256 -sigopt rsa_padding_mode:pss -verify $W/rsa2048.pub -signature $W/sig $W/msg
q openssl pkeyutl -sign -inkey $W/ed25519.key -rawin -in $W/msg -out $W/sig
q openssl pkeyutl -verify -pubin -inkey $W/ed25519.pub -rawin -in $W/msg -sigfile $W/sig
q openssl pkeyutl -derive -inkey $W/x25519.key -peerkey $W/x25519b.pub -out $W/shared
q openssl pkeyutl -derive -inkey $W/p256.key -peerkey $W/p256.pub -out $W/shared
q openssl pkeyutl -encrypt -pubin -inkey $W/rsa2048.pub -pkeyopt rsa_padding_mode:oaep -in $W/shared -out $W/wrapped
q openssl pkeyutl -decrypt -inkey $W/rsa2048.key -pkeyopt rsa_padding_mode:oaep -in $W/wrapped -out $W/unwrapped
for d in sha256 sha512 sha1 md5 sha3-256 blake2b512; do q openssl dgst -$d $W/msg; done
q openssl dgst -sha256 -hmac key $W/msg
for c in aes-256-cbc aes-128-ctr aes-256-ctr chacha20; do
    q openssl enc -$c -pbkdf2 -iter 2000 -pass pass:train -in $W/msg -out $W/enc
    q openssl enc -d -$c -pbkdf2 -iter 2000 -pass pass:train -in $W/enc -out $W/dec
    cmp -s $W/msg $W/dec || echo "FAIL enc round trip $c"
done
q openssl rand -hex 4096
q openssl kdf -keylen 32 -kdfopt digest:SHA256 -kdfopt key:secret -kdfopt salt:salt -kdfopt info:label HKDF

# 3. certificates: a CA, a request, its signature, a chain verification, PKCS#12 and PKCS#8 with a passphrase
q openssl req -new -key $W/p256.key -subj /CN=train.local -addext subjectAltName=DNS:train.local -out $W/req.csr
q openssl req -x509 -new -key $W/rsa3072.key -subj /CN=train-ca -days 30 -addext basicConstraints=critical,CA:TRUE -out $W/ca.crt
q openssl x509 -req -in $W/req.csr -CA $W/ca.crt -CAkey $W/rsa3072.key -CAcreateserial -days 30 -sha256 -copy_extensions copy -out $W/leaf.crt
q openssl verify -CAfile $W/ca.crt -verify_hostname train.local $W/leaf.crt
q openssl x509 -in $W/leaf.crt -noout -text -fingerprint -sha256
q openssl pkcs12 -export -inkey $W/p256.key -in $W/leaf.crt -passout pass:train -out $W/leaf.p12
q openssl pkcs12 -in $W/leaf.p12 -passin pass:train -nodes -out $W/leaf.pem
q openssl pkcs8 -topk8 -in $W/rsa2048.key -passout pass:train -out $W/rsa.p8
q openssl pkey -in $W/rsa.p8 -passin pass:train -noout

# 4. TLS handshakes: one request each, s_client against s_server
#    tls <port> <rsa|ec> <"Cipher is" name expected> <s_client arguments...>
serve() { openssl s_server -accept $1 -cert $2.crt -key $2.key -www -quiet > /dev/null 2>&1 & spid=$!; sleep 1; }
tls() {
    p=$1 want=$2; shift 2
    printf 'GET / HTTP/1.0\r\n\r\n' | openssl s_client -connect 127.0.0.1:$p -CAfile ca_bundle.pem -verify_return_error -ign_eof "$@" > $W/tls.out 2>&1
    grep -q "Cipher is $want" $W/tls.out && grep -q "Verify return code: 0" $W/tls.out || echo "FAIL tls $want $*"
}
for kind in rsa ec; do
    [ $kind = rsa ] && { port=8460 au=RSA; } || { port=8461 au=ECDSA; }
    serve $port $kind
    for c in AES128-GCM-SHA256 AES256-GCM-SHA384 CHACHA20-POLY1305 AES128-SHA256 AES256-SHA384; do
        tls $port ECDHE-$au-$c -tls1_2 -cipher ECDHE-$au-$c
    done
    # (P-256 stays in the list: in TLS 1.2 it also has to cover the curve of the EC certificate)
    for g in X25519 P-256 P-384 P-521; do tls $port ECDHE-$au-AES256-GCM-SHA384 -tls1_2 -cipher ECDHE-$au-AES256-GCM-SHA384 -groups $g:P-256; done
    for c in TLS_AES_128_GCM_SHA256 TLS_AES_256_GCM_SHA384 TLS_CHACHA20_POLY1305_SHA256; do
        tls $port $c -tls1_3 -ciphersuites $c
    done
    for g in X25519 P-256 P-384 P-521 X448 ffdhe2048; do tls $port TLS_AES_256_GCM_SHA384 -tls1_3 -groups $g; done
    # a resumed session (ticket) and a renegotiated key update
    tls $port TLS_AES_256_GCM_SHA384 -tls1_3 -sess_out $W/sess
    tls $port TLS_AES_256_GCM_SHA384 -tls1_3 -sess_in $W/sess
    tls $port ECDHE-$au-AES256-GCM-SHA384 -tls1_2 -sess_out $W/sess12
    tls $port ECDHE-$au-AES256-GCM-SHA384 -tls1_2 -sess_in $W/sess12
    tls $port TLS_AES_256_GCM_SHA384 -alpn h2,http/1.1 -servername localhost -status
    kill $spid; wait $spid 2> /dev/null
done
serve 8462 rsa
tls 8462 DHE-RSA-AES256-GCM-SHA384 -tls1_2 -cipher DHE-RSA-AES256-GCM-SHA384
tls 8462 DHE-RSA-CHACHA20-POLY1305 -tls1_2 -cipher DHE-RSA-CHACHA20-POLY1305
tls 8462 AES256-GCM-SHA384 -tls1_2 -cipher AES256-GCM-SHA384
kill $spid; wait $spid 2> /dev/null
rm -rf $W
echo openssl-train-ok
