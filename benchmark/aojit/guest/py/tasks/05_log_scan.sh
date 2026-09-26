python3 -c "
import re, datetime, collections
pat = re.compile(r'^(\S+) \[(\w+)\] (\w+): (.*) footprint=(\d+)MB$')
keys = ('MemMonitor', 'ForkGuard', 'memoryWarning')
cnt, peak, first = collections.Counter(), {}, None
for line in open('/tmp/aojit/py/app.log'):
    m = pat.match(line)
    if not m or m.group(3) not in keys:
        continue
    t = datetime.datetime.fromisoformat(m.group(1))
    first = first or t
    cnt[(m.group(2), m.group(3))] += 1
    peak[m.group(3)] = max(peak.get(m.group(3), 0), int(m.group(5)))
print(sorted(cnt.items())[:5], peak, first)
"
