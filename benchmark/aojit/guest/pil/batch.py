# The batch: PNG decode -> Lanczos resize -> UnsharpMask -> JPEG + WebP encode.
import glob, hashlib, io, sys, time
from PIL import Image, ImageFilter
stage = {'decode': 0.0, 'resize': 0.0, 'unsharp': 0.0, 'jpeg': 0.0, 'webp': 0.0}
h = hashlib.md5()
for path in sorted(glob.glob('/tmp/aojit/pil/in/*.png')):
    t = time.perf_counter()
    im = Image.open(path); im.load(); im = im.convert('RGB')
    t1 = time.perf_counter(); stage['decode'] += t1 - t
    im = im.resize((im.width // 2, im.height // 2), Image.LANCZOS)
    t2 = time.perf_counter(); stage['resize'] += t2 - t1
    im = im.filter(ImageFilter.UnsharpMask(radius=2, percent=150, threshold=3))
    t3 = time.perf_counter(); stage['unsharp'] += t3 - t2
    b = io.BytesIO(); im.save(b, 'JPEG', quality=85); h.update(b.getvalue())
    t4 = time.perf_counter(); stage['jpeg'] += t4 - t3
    b = io.BytesIO(); im.save(b, 'WEBP', quality=80, method=4); h.update(b.getvalue())
    stage['webp'] += time.perf_counter() - t4
print('md5', h.hexdigest())
print(' '.join(f'{k}={v:.2f}s' for k, v in stage.items()), file=sys.stderr)
