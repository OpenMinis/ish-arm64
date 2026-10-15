# Load the large JSON, aggregate it and render a 4-panel chart to PNG.
import time, sys
t = [time.perf_counter()]
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
t.append(time.perf_counter())

with open('/tmp/aojit/charts/mpl_data.json') as f:
    data = json.load(f)
rows = data['rows']
t.append(time.perf_counter())

regions = sorted({r['region'] for r in rows})
days = np.arange(365)
daily = {g: np.zeros(365) for g in regions}
count = {g: np.zeros(365) for g in regions}
for r in rows:
    daily[r['region']][r['day']] += r['temp']
    count[r['region']][r['day']] += 1
loads = np.array([r['load'] for r in rows])
temps = np.array([r['temp'] for r in rows])
tags = {}
for r in rows:
    for tg in r['tags']:
        tags[tg] = tags.get(tg, 0) + 1
t.append(time.perf_counter())

fig, ax = plt.subplots(2, 2, figsize=(14, 9))
for g in regions:
    m = np.divide(daily[g], count[g], out=np.zeros(365), where=count[g] > 0)
    ax[0, 0].plot(days, np.convolve(m, np.ones(7) / 7, mode='same'), label=g, lw=1)
ax[0, 0].set_title('7-day mean temperature by region'); ax[0, 0].legend(fontsize=7, ncol=2)
ax[0, 1].hist(loads, bins=120, color='tab:orange'); ax[0, 1].set_title('load distribution')
ax[1, 0].scatter(temps[::10], loads[::10], s=2, alpha=0.3); ax[1, 0].set_title('temp vs load (1/10 sample)')
ax[1, 1].bar(sorted(tags), [tags[k] for k in sorted(tags)], color='tab:green'); ax[1, 1].set_title('tag counts')
fig.tight_layout()
t.append(time.perf_counter())
fig.savefig('/tmp/aojit/charts/mpl_chart.png', dpi=110)
t.append(time.perf_counter())

names = ['import', 'json.load', 'aggregate', 'plot', 'savefig']
print(' '.join('%s=%.2f' % (n, t[i + 1] - t[i]) for i, n in enumerate(names)), 'total=%.2f' % (t[-1] - t[0]), file=sys.stderr)
