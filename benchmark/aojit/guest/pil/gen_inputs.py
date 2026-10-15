# Inputs for the batch (not timed): photo-like RGB PNGs (gradients + noise).
# usage: gen_inputs.py <n> <w> <h> [dir (in)] [seed (1)]
import os, random, sys
from PIL import Image, ImageDraw, ImageFilter
n, w, h = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
out = '/tmp/aojit/pil/' + (sys.argv[4] if len(sys.argv) > 4 else 'in')
os.makedirs(out, exist_ok=True)
random.seed(int(sys.argv[5]) if len(sys.argv) > 5 else 1)
for i in range(n):
    im = Image.linear_gradient('L').resize((w, h)).convert('RGB')
    d = ImageDraw.Draw(im)
    for _ in range(60):
        x, y = random.randrange(w), random.randrange(h)
        d.ellipse((x, y, x + random.randrange(20, 200), y + random.randrange(20, 200)),
                  fill=(random.randrange(256), random.randrange(256), random.randrange(256)))
    noise = Image.effect_noise((w, h), 24).convert('RGB')
    im = Image.blend(im, noise, 0.25).filter(ImageFilter.GaussianBlur(1))
    im.save(f'{out}/{i:03d}.png')
print(n, 'inputs', sum(os.path.getsize(f'{out}/{f}') for f in os.listdir(out)) // 1024, 'KB')
