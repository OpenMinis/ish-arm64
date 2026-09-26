# Inputs of the python/node agent tasks (deterministic, not timed).
import datetime, json, random
random.seed(7)
D = '/tmp/aojit/py/'
words = 'alpha beta gamma delta ls cd make build error warning ok done fetch token session screen commit'.split()
line = lambda: ' '.join(random.choice(words) for _ in range(random.randint(4, 14)))
json.dump({'id': 'B420', 'lines': [line() for _ in range(300)], 'cursor': [12, 80]}, open(D + 'screen.json', 'w'))
json.dump({'token': 'x' * 64, 'host': 'mini2.example', 'port': 6770, 'extra': {str(i): i for i in range(50)}}, open(D + 'config.json', 'w'))
items = [{'id': i, 'name': line(), 'tags': random.sample(words, 3), 'score': random.random(),
          'created': (datetime.datetime(2026, 1, 1) + datetime.timedelta(minutes=i)).isoformat(),
          'owner': {'login': random.choice(words), 'id': random.randint(1, 10**6)}} for i in range(9000)]
json.dump({'total': len(items), 'items': items}, open(D + 'api.json', 'w'))
with open(D + 'app.log', 'w') as f:
    t = datetime.datetime(2026, 8, 11)
    lvl = ['INFO', 'DEBUG', 'WARN', 'ERROR']
    tags = ['MemMonitor', 'Network', 'ForkGuard', 'UI', 'Agent', 'Shell', 'memoryWarning']
    for i in range(250000):
        t += datetime.timedelta(milliseconds=random.randint(1, 400))
        f.write(f"{t.isoformat(timespec='milliseconds')} [{random.choice(lvl)}] {random.choice(tags)}: {line()} footprint={random.randint(100, 900)}MB\n")
open(D + 'blob.bin', 'wb').write(bytes(random.getrandbits(8) for _ in range(1 << 20)))
with open(D + 'page.html', 'w') as f:
    f.write('<html><head><title>t</title></head><body>')
    for i in range(6000):
        f.write(f'<div class="item c{i % 7}"><a href="/p/{i}">{line()}</a><p>{line()} <b>{line()}</b></p><span data-x="{i}">{line()}</span></div>\n')
    f.write('</body></html>')
print('py data ok')
