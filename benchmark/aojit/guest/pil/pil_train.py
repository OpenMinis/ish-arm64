# Recording workload (different from the measured batch): other sizes, ratios and quality settings.
import glob, io
from PIL import Image, ImageFilter
for i, path in enumerate(sorted(glob.glob('/tmp/aojit/pil/train/*.png'))):
    im = Image.open(path).convert('RGB')
    for div, q, m in ((3, 75, 4), (2, 90, 6)):
        r = im.resize((im.width // div, im.height // div), Image.LANCZOS if i % 2 == 0 else Image.BICUBIC)
        r = r.filter(ImageFilter.UnsharpMask(radius=1 + i % 3, percent=120, threshold=2))
        r.save(io.BytesIO(), 'JPEG', quality=q, optimize=(m == 6))
        r.save(io.BytesIO(), 'WEBP', quality=q, method=m)
        r.save(io.BytesIO(), 'PNG')
print('trained')
