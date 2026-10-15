python3 -c "
import json
d = json.load(open('/tmp/aojit/py/api.json'))
by = {}
for it in d['items']:
    by.setdefault(it['owner']['login'], []).append({'id': it['id'], 'score': round(it['score'], 3), 'tags': sorted(it['tags'])})
out = {k: sorted(v, key=lambda x: -x['score'])[:50] for k, v in sorted(by.items())}
s = json.dumps(out, indent=2, ensure_ascii=False)
print(len(s), len(json.loads(s)))
"
