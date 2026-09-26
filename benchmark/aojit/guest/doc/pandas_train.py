import numpy as np, pandas as pd
rng = np.random.default_rng(5)
df = pd.DataFrame({'k': rng.integers(0, 50, 80000), 'g': rng.choice(list('abcdefgh'), 80000), 'v': rng.normal(size=80000), 'w': rng.random(80000)})
a = df.groupby(['k', 'g']).agg(v=('v', 'mean'), w=('w', 'sum')).reset_index()
b = df.pivot_table(index='g', columns='k', values='v', aggfunc='std')
m = df.merge(a, on=['k', 'g'], suffixes=('', '_m'))
m['z'] = (m['v'] - m['v_m']) / (m['w'].rolling(20).mean() + 1)
print(len(a), b.shape, round(float(m['z'].abs().sum()), 2), np.percentile(m['w'], [5, 50, 95]).round(3).tolist())
