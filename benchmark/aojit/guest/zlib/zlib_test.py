import gzip, random, zlib
random.seed(7)
words = [''.join(random.choice('abcdefghijklmnop') for _ in range(random.randint(2, 9))).encode() for _ in range(3000)]
text = b' '.join(random.choice(words) for _ in range(900000))
tot = 0
for lvl in (3, 6, 8):
    c = zlib.compress(text, lvl); tot += len(c)
    assert zlib.decompress(c) == text
for _ in range(3):
    g = gzip.compress(text, 5); tot += len(gzip.decompress(g))
print('zlib-test', tot, zlib.crc32(text))
