# Candidate workload (profile.sh), needs network: short HTTPS API calls (each a new process + TLS handshake), one compressed JSON, one 2 MB download
for i in 1 2 3 4 5 6; do curl -s -m 20 -o /dev/null -w '%{http_code} ' https://www.google.com/generate_204 || echo CURLFAIL; done
for i in 1 2 3; do curl -s -m 20 --compressed -o /dev/null https://www.cloudflare.com/cdn-cgi/trace || echo CURLFAIL; done
curl -s -m 60 -o /dev/null https://speed.cloudflare.com/__down?bytes=2000000 || echo CURLFAIL
echo
