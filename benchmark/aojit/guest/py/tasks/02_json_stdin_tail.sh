for i in $(seq 1 20); do cat /tmp/aojit/py/screen.json | python3 -c "import json,sys; d=json.load(sys.stdin); print('\n'.join(d['lines'][-40:]))"; done
