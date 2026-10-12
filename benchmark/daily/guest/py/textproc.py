# A typical agent script: parse JSON, scan a log with regexes, summarise.
import json, re, sys, collections, hashlib
log, data = sys.argv[1], sys.argv[2]
pat = re.compile(r'^(\S+) (\S+) (\w+) (svc\d+) user=(\d+) (\w+) (\w+) took=(\d+)ms$')
levels = collections.Counter()
slow = collections.defaultdict(list)
users = set()
with open(log) as f:
    for line in f:
        m = pat.match(line)
        if not m:
            continue
        levels[m.group(3)] += 1
        users.add(m.group(5))
        if int(m.group(8)) > 850:
            slow[m.group(4)].append(int(m.group(8)))
with open(data) as f:
    items = json.load(f)['items']
by_tag = collections.Counter(t for it in items for t in it['tags'])
cheap = sorted((it for it in items if it['price'] < 50), key=lambda it: (it['price'], it['id']))
report = {
    'levels': dict(sorted(levels.items())),
    'users': len(users),
    'slow': {k: (len(v), max(v)) for k, v in sorted(slow.items())},
    'tags': by_tag.most_common(5),
    'cheap': [it['id'] for it in cheap[:20]],
    'value': round(sum(it['price'] * it['stock'] for it in items), 2),
}
out = json.dumps(report, sort_keys=True)
print(hashlib.sha256(out.encode()).hexdigest()[:16], len(out))
