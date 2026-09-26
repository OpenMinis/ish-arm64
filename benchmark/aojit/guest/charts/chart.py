import json, matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt, numpy as np
rows = json.load(open('/tmp/aojit/charts/chart.json'))['rows']
regions = sorted({r['region'] for r in rows}); days = np.arange(365)
daily = {g: np.zeros(365) for g in regions}; cnt = {g: np.zeros(365) for g in regions}
for r in rows:
    daily[r['region']][r['day']] += r['temp']; cnt[r['region']][r['day']] += 1
loads = np.array([r['load'] for r in rows]); temps = np.array([r['temp'] for r in rows])
tags = {}
for r in rows:
    for g in r['tags']:
        tags[g] = tags.get(g, 0) + 1
fig, ax = plt.subplots(2, 2, figsize=(14, 9))
for g in regions:
    m = np.divide(daily[g], cnt[g], out=np.zeros(365), where=cnt[g] > 0)
    ax[0, 0].plot(days, np.convolve(m, np.ones(7) / 7, mode='same'), label=g, lw=1)
ax[0, 0].legend(fontsize=7, ncol=2); ax[0, 1].hist(loads, bins=120)
ax[1, 0].scatter(temps[::10], loads[::10], s=2, alpha=0.3); ax[1, 1].bar(sorted(tags), [tags[k] for k in sorted(tags)])
fig.tight_layout(); fig.savefig('/tmp/aojit/charts/chart.png', dpi=110)
