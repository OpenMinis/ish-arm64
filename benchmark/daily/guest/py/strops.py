# Strings, lists, dicts, sets, comprehensions, sorting.
words = ['w%d' % (i * 7919 % 10007) for i in range(120000)]
counts = {}
for w in words:
    counts[w] = counts.get(w, 0) + 1
joined = ','.join(sorted(set(words)))
parts = joined.split(',')
upper = [p.upper() for p in parts if p.endswith('7')]
fmt = ['%s:%d' % (k, v) for k, v in sorted(counts.items())[:5000]]
s = ''.join(reversed(joined))
print(len(counts), len(parts), len(upper), len(fmt), s.count('1'), hash(tuple(fmt[:10])) & 0)
