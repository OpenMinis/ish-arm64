for i in $(seq 1 20); do python3 -c "import json; d=json.load(open('/tmp/aojit/py/config.json')); print(d['token'])"; done
