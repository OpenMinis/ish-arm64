# Deterministic large JSON for mpl_chart.py: 200k sensor readings over one year, 8 regions.
import json, random
random.seed(42)
regions = ['north', 'south', 'east', 'west', 'central', 'coast', 'hills', 'valley']
rows = []
for i in range(200_000):
    day = i * 365 // 200_000
    base = 20 + 10 * ((day % 365) / 365 - 0.5) ** 2 * 4
    rows.append({'id': i, 'day': day, 'region': regions[i % len(regions)],
                 'temp': round(base + random.gauss(0, 3), 3),
                 'load': round(random.lognormvariate(3, 0.6), 3),
                 'tags': [random.choice('abcdefgh') for _ in range(3)]})
with open('/tmp/aojit/charts/mpl_data.json', 'w') as f:
    json.dump({'meta': {'source': 'synthetic', 'n': len(rows)}, 'rows': rows}, f)
print('mpl data ok')
