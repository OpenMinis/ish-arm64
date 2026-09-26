for i in $(seq 1 3); do python3 -c "import requests, httpx, urllib.request; print(requests.__version__, httpx.__version__)"; done
