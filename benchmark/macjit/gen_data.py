#!/usr/bin/env python3
"""Write the benchmark inputs and guest scripts into a rootfs under /opt/macjit.

The same rootfs is imported as the Docker and Apple container image, so all
environments read byte-identical inputs. Inputs are deterministic (fixed seed).

    gen_data.py <rootfs>
"""
import json
import random
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def gen_log(path, lines, rng):
    methods = ['GET', 'GET', 'GET', 'POST', 'PUT', 'DELETE']
    paths = ['/', '/api/users', '/api/items', '/static/app.js', '/login', '/search', '/img/logo.png']
    codes = ['200', '200', '200', '200', '301', '304', '404', '500']
    with open(path, 'w') as f:
        for i in range(lines):
            ip = f'10.{rng.randrange(256)}.{rng.randrange(256)}.{rng.randrange(256)}'
            ts = f'21/Oct/2026:{rng.randrange(24):02d}:{rng.randrange(60):02d}:{rng.randrange(60):02d} +0800'
            req = f'{rng.choice(methods)} {rng.choice(paths)}?id={rng.randrange(100000)} HTTP/1.1'
            f.write(f'{ip} - - [{ts}] "{req}" {rng.choice(codes)} {rng.randrange(50, 50000)} '
                    f'"-" "Mozilla/5.0 bench/{i % 17}"\n')


def gen_words(path, lines, rng):
    alphabet = 'abcdefghijklmnopqrstuvwxyz'
    vocab = [''.join(rng.choice(alphabet) for _ in range(rng.randrange(3, 10))) for _ in range(20000)]
    with open(path, 'w') as f:
        for _ in range(lines):
            f.write(rng.choice(vocab) + '\n')


def gen_json(path, records, rng):
    data = []
    for i in range(records):
        data.append({
            'id': i,
            'name': f'user{i}',
            'email': f'user{i}@example.com',
            'score': rng.random() * 100,
            'active': rng.random() < 0.7,
            'tags': [f't{rng.randrange(50)}' for _ in range(rng.randrange(1, 6))],
            'address': {'city': f'city{rng.randrange(300)}', 'zip': f'{rng.randrange(100000):05d}'},
        })
    with open(path, 'w') as f:
        json.dump(data, f)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1]) / 'opt/macjit'
    data = root / 'data'
    data.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261026)
    gen_log(data / 'access.log', 300_000, rng)
    gen_words(data / 'words.txt', 500_000, rng)
    gen_json(data / 'data.json', 60_000, rng)
    (data / 'blob.bin').write_bytes(rng.randbytes(32 << 20))
    shutil.copytree(HERE / 'guest', root / 'guest', dirs_exist_ok=True)
    print(f'✅ inputs and guest scripts written to {root}')


if __name__ == '__main__':
    main()
