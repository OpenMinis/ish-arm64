import json, random
random.seed(42)
regions = ['north', 'south', 'east', 'west', 'central', 'coast', 'hills', 'valley']
products = [f'p{i:03d}' for i in range(120)]
with open('/tmp/aojit/charts/sales.csv', 'w') as f:
    f.write('order_id,date,region,product,qty,price,channel\n')
    for i in range(200000):
        f.write(f'{i},2026-{random.randint(1, 12):02d}-{random.randint(1, 28):02d},{random.choice(regions)},'
                f'{random.choice(products)},{random.randint(1, 20)},{random.uniform(1, 500):.2f},'
                f'{random.choice(["web", "app", "store"])}\n')
with open('/tmp/aojit/charts/products.csv', 'w') as f:
    f.write('product,category,cost\n')
    for p in products:
        f.write(f'{p},c{random.randint(1, 9)},{random.uniform(1, 300):.2f}\n')
rows = [{'id': i, 'day': i * 365 // 50000, 'region': regions[i % 8], 'temp': round(20 + random.gauss(0, 3), 3),
         'load': round(random.lognormvariate(3, 0.6), 3), 'tags': [random.choice('abcdefgh') for _ in range(3)]}
        for i in range(50000)]
json.dump({'rows': rows}, open('/tmp/aojit/charts/chart.json', 'w'))
