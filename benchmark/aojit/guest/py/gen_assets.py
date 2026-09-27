# One-time inputs that need Pillow / cryptography.
from PIL import Image, ImageDraw
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
import random
random.seed(3)
for k in range(6):
    im = Image.new('RGB', (1600, 1200), (20 * k, 40, 90))
    d = ImageDraw.Draw(im)
    for _ in range(300):
        x, y = random.randint(0, 1500), random.randint(0, 1100)
        d.rectangle([x, y, x + random.randint(10, 100), y + random.randint(10, 100)], fill=tuple(random.randint(0, 255) for _ in range(3)))
    im.save(f'/tmp/aojit/py/frame_{k}.jpg', quality=85)
key = ec.generate_private_key(ec.SECP256R1())
open('/tmp/aojit/py/key.p8', 'wb').write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
print('setup ok')
