# Throwaway localhost certificates for the TLS server (generated per install, never committed).
cd /tmp/aojit/net
openssl req -x509 -newkey rsa:2048 -nodes -keyout rsa.key -out rsa.crt -days 365 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout ec.key -out ec.crt -days 365 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 2>/dev/null
cat /etc/ssl/certs/ca-certificates.crt rsa.crt ec.crt > ca_bundle.pem
echo certs ok
