python3 -c "
import gzip, io, random, zipfile, zlib
random.seed(1)
text = ' '.join(random.choice(['alpha', 'beta', 'gamma', 'delta', 'data', 'zlib', 'agent']) for _ in range(400000)).encode()
blob = bytes(random.getrandbits(8) for _ in range(300000))
for lvl in (1, 6, 9):
    c = zlib.compress(text, lvl); assert zlib.decompress(c) == text
    zlib.decompress(zlib.compress(blob, lvl))
g = gzip.compress(text); assert gzip.decompress(g) == text
b = io.BytesIO()
with zipfile.ZipFile(b, 'w', zipfile.ZIP_DEFLATED) as z:
    for i in range(20): z.writestr(f'f{i}.txt', text[i * 1000:i * 1000 + 50000])
with zipfile.ZipFile(b) as z: sum(len(z.read(n)) for n in z.namelist())
co = zlib.compressobj(5, zlib.DEFLATED, -15); d = co.compress(text) + co.flush(); zlib.decompress(d, -15)
print('zlib-train-ok', zlib.crc32(text), zlib.adler32(text))
"
