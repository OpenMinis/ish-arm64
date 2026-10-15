python3 -c "
import jwt, time
key = open('/tmp/aojit/py/key.p8').read()
from cryptography.hazmat.primitives import serialization
pub = serialization.load_pem_private_key(key.encode(), None).public_key()
n = 0
for i in range(100):
    tok = jwt.encode({'iss': 'cbb077f2', 'iat': int(time.time()) - 60, 'exp': int(time.time()) + 1200, 'jti': str(i), 'aud': 'appstoreconnect-v1'}, key, algorithm='ES256', headers={'kid': 'NRNMF2K6P3'})
    n += jwt.decode(tok, pub, algorithms=['ES256'], audience='appstoreconnect-v1')['iat'] > 0
print(n)
"
