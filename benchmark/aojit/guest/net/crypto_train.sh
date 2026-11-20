# libcrypto recording workload: TLS (python, requests, the openssl command line) and ssh, whose
# key exchange, signatures, ciphers and MACs all run in libcrypto. The ssh half needs the sshd of
# ../ssh/server.sh (AOJIT_SSH, default 127.0.0.1:2222), like the ssh and scp images.
sh /tmp/aojit/net/tls_train.sh
sh /tmp/aojit/ssh/ssh_train_full.sh
echo crypto-train-ok
