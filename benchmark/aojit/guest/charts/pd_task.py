import pandas as pd
df = pd.read_csv('/tmp/aojit/charts/sales.csv')
df['date'] = pd.to_datetime(df['date'])
df['revenue'] = df['qty'] * df['price']
df['month'] = df['date'].dt.month
by = df.groupby(['region', 'product']).agg(qty=('qty', 'sum'), revenue=('revenue', 'sum'), orders=('order_id', 'count'))
pivot = df.pivot_table(index='region', columns='month', values='revenue', aggfunc='sum')
prod = pd.read_csv('/tmp/aojit/charts/products.csv')
m = df.merge(prod, on='product')
m['margin'] = m['revenue'] - m['qty'] * m['cost']
top = m.groupby('category')['margin'].sum().sort_values(ascending=False)
out = by.sort_values('revenue', ascending=False).head(50).to_json()
print(len(by), pivot.shape, round(top.iloc[0], 2), len(out))
