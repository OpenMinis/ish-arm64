python3 -c "
from PIL import Image, ImageChops, ImageDraw, ImageStat
prev, diffs = None, []
for k in range(6):
    im = Image.open(f'/tmp/aojit/py/frame_{k}.jpg').convert('RGB')
    if prev is not None:
        diffs.append(round(sum(ImageStat.Stat(ImageChops.difference(im, prev)).mean), 2))
    prev = im
    t = im.copy(); t.thumbnail((320, 240)); d = ImageDraw.Draw(t); d.text((10, 10), f'frame {k}', fill=(255, 255, 255))
    t.save(f'/tmp/aojit/py/thumb_{k}.png')
print(diffs)
"
